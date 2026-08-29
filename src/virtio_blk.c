#define _GNU_SOURCE
#include "rackvm.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_config.h>
#include <linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define VIRTIO_MMIO_BASE 0xd0000000ULL
#define VIRTIO_MMIO_SIZE 0x1000ULL
#define VIRTIO_BLOCK_IRQ 5U
#define VIRTIO_QUEUE_SIZE 128U
#define VIRTIO_VENDOR_RACK 0x4b434152U

struct rackvm_virtio_queue {
    uint16_t size;
    uint16_t last_available;
    bool ready;
    uint64_t descriptors;
    uint64_t available;
    uint64_t used;
};

struct rackvm_virtio_blk {
    int disk_fd;
    pthread_mutex_t lock;
    uint64_t capacity;
    uint32_t device_features_select;
    uint32_t driver_features_select;
    uint64_t driver_features;
    uint32_t queue_select;
    uint32_t status;
    uint32_t interrupt_status;
    struct rackvm_virtio_queue queue;
};

struct virtio_request {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static void *guest_pointer(struct rackvm *vm, uint64_t address, size_t size)
{
    if (address > vm->memory_size || size > vm->memory_size - address)
        return NULL;
    return (uint8_t *)vm->memory + address;
}

static void set_irq(struct rackvm *vm, bool level)
{
    struct kvm_irq_level irq = {.irq = VIRTIO_BLOCK_IRQ, .level = level ? 1U : 0U};
    if (vm->vm_fd >= 0)
        ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq);
}

static int read_descriptor(struct rackvm *vm, uint16_t index, struct vring_desc *descriptor)
{
    struct rackvm_virtio_queue *queue = &vm->block->queue;
    if (index >= queue->size)
        return -1;
    struct vring_desc *source = guest_pointer(vm, queue->descriptors + (uint64_t)index * sizeof(*source), sizeof(*source));
    if (!source)
        return -1;
    memcpy(descriptor, source, sizeof(*descriptor));
    descriptor->addr = htole64(le64toh(descriptor->addr));
    descriptor->len = htole32(le32toh(descriptor->len));
    descriptor->flags = htole16(le16toh(descriptor->flags));
    descriptor->next = htole16(le16toh(descriptor->next));
    return 0;
}

static int next_descriptor(struct rackvm *vm, const struct vring_desc *current, struct vring_desc *next)
{
    if (!(le16toh(current->flags) & VRING_DESC_F_NEXT))
        return -1;
    return read_descriptor(vm, le16toh(current->next), next);
}

static uint8_t execute_request(struct rackvm *vm, uint16_t head, uint32_t *written)
{
    struct vring_desc header_descriptor;
    if (read_descriptor(vm, head, &header_descriptor) < 0 || (le16toh(header_descriptor.flags) & VRING_DESC_F_WRITE) || le32toh(header_descriptor.len) < sizeof(struct virtio_request))
        return VIRTIO_BLK_S_IOERR;
    struct virtio_request *request = guest_pointer(vm, le64toh(header_descriptor.addr), sizeof(*request));
    if (!request)
        return VIRTIO_BLK_S_IOERR;

    uint32_t type = le32toh(request->type);
    uint64_t sector = le64toh(request->sector);
    struct vring_desc data_descriptor;
    if (next_descriptor(vm, &header_descriptor, &data_descriptor) < 0)
        return VIRTIO_BLK_S_IOERR;

    struct vring_desc status_descriptor;
    bool has_data = type != VIRTIO_BLK_T_FLUSH;
    if (has_data) {
        if (next_descriptor(vm, &data_descriptor, &status_descriptor) < 0)
            return VIRTIO_BLK_S_IOERR;
    } else {
        status_descriptor = data_descriptor;
    }
    if (!(le16toh(status_descriptor.flags) & VRING_DESC_F_WRITE) || le32toh(status_descriptor.len) < 1)
        return VIRTIO_BLK_S_IOERR;
    uint8_t *status = guest_pointer(vm, le64toh(status_descriptor.addr), 1);
    if (!status)
        return VIRTIO_BLK_S_IOERR;

    uint8_t result = VIRTIO_BLK_S_OK;
    uint32_t length = has_data ? le32toh(data_descriptor.len) : 0;
    void *data = has_data ? guest_pointer(vm, le64toh(data_descriptor.addr), length) : NULL;
    if (has_data && !data) {
        result = VIRTIO_BLK_S_IOERR;
    } else if (type == VIRTIO_BLK_T_IN) {
        if (!(le16toh(data_descriptor.flags) & VRING_DESC_F_WRITE) || sector > (uint64_t)INT64_MAX / 512 || pread(vm->block->disk_fd, data, length, (off_t)(sector * 512)) != (ssize_t)length)
            result = VIRTIO_BLK_S_IOERR;
        else
            *written = length;
    } else if (type == VIRTIO_BLK_T_OUT) {
        if ((le16toh(data_descriptor.flags) & VRING_DESC_F_WRITE) || sector > (uint64_t)INT64_MAX / 512 || pwrite(vm->block->disk_fd, data, length, (off_t)(sector * 512)) != (ssize_t)length)
            result = VIRTIO_BLK_S_IOERR;
    } else if (type == VIRTIO_BLK_T_FLUSH) {
        if (fdatasync(vm->block->disk_fd) < 0)
            result = VIRTIO_BLK_S_IOERR;
    } else if (type == VIRTIO_BLK_T_GET_ID) {
        static const char identifier[] = "rackvm-virtio-disk";
        if (!(le16toh(data_descriptor.flags) & VRING_DESC_F_WRITE)) {
            result = VIRTIO_BLK_S_IOERR;
        } else {
            size_t copied = length < sizeof(identifier) - 1 ? length : sizeof(identifier) - 1;
            memcpy(data, identifier, copied);
            *written = (uint32_t)copied;
        }
    } else {
        result = VIRTIO_BLK_S_UNSUPP;
    }
    *status = result;
    (*written)++;
    return result;
}

static void process_queue(struct rackvm *vm)
{
    struct rackvm_virtio_queue *queue = &vm->block->queue;
    if (!queue->ready || !queue->size || queue->size > VIRTIO_QUEUE_SIZE)
        return;
    uint16_t *available_index = guest_pointer(vm, queue->available + sizeof(uint16_t), sizeof(*available_index));
    uint16_t *used_index = guest_pointer(vm, queue->used + sizeof(uint16_t), sizeof(*used_index));
    if (!available_index || !used_index)
        return;
    uint16_t published = le16toh(*available_index);
    bool processed = false;
    while (queue->last_available != published) {
        uint64_t slot = queue->available + 2 * sizeof(uint16_t) + (uint64_t)(queue->last_available % queue->size) * sizeof(uint16_t);
        uint16_t *available = guest_pointer(vm, slot, sizeof(*available));
        if (!available)
            break;
        uint16_t head = le16toh(*available);
        uint32_t written = 0;
        execute_request(vm, head, &written);
        uint16_t used_slot = le16toh(*used_index) % queue->size;
        struct vring_used_elem *element = guest_pointer(vm, queue->used + 2 * sizeof(uint16_t) + (uint64_t)used_slot * sizeof(*element), sizeof(*element));
        if (!element)
            break;
        element->id = htole32(head);
        element->len = htole32(written);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        *used_index = htole16((uint16_t)(le16toh(*used_index) + 1));
        queue->last_available++;
        processed = true;
    }
    if (!processed || queue->last_available != published)
        return;
    vm->block->interrupt_status |= VIRTIO_MMIO_INT_VRING;
    set_irq(vm, true);
}

static uint32_t read_register(struct rackvm *vm, uint64_t offset)
{
    struct rackvm_virtio_blk *block = vm->block;
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        return 0x74726976U;
    case VIRTIO_MMIO_VERSION:
        return 2;
    case VIRTIO_MMIO_DEVICE_ID:
        return VIRTIO_ID_BLOCK;
    case VIRTIO_MMIO_VENDOR_ID:
        return VIRTIO_VENDOR_RACK;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        return block->device_features_select == 1 ? 1U << (VIRTIO_F_VERSION_1 - 32) : (1U << VIRTIO_BLK_F_FLUSH);
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return block->queue_select == 0 ? VIRTIO_QUEUE_SIZE : 0;
    case VIRTIO_MMIO_QUEUE_READY:
        return block->queue.ready ? 1 : 0;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        return block->interrupt_status;
    case VIRTIO_MMIO_STATUS:
        return block->status;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        return 0;
    default:
        if (offset >= VIRTIO_MMIO_CONFIG && offset < VIRTIO_MMIO_CONFIG + sizeof(block->capacity)) {
            uint64_t capacity = htole64(block->capacity);
            uint32_t value = 0;
            memcpy(&value, (uint8_t *)&capacity + offset - VIRTIO_MMIO_CONFIG, sizeof(value));
            return value;
        }
        return 0;
    }
}

static void update_half(uint64_t *value, bool high, uint32_t half)
{
    uint64_t shift = high ? 32 : 0;
    *value = (*value & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)half << shift);
}

static void reset_device(struct rackvm_virtio_blk *block)
{
    block->device_features_select = 0;
    block->driver_features_select = 0;
    block->driver_features = 0;
    block->queue_select = 0;
    block->status = 0;
    block->interrupt_status = 0;
    memset(&block->queue, 0, sizeof(block->queue));
}

static void write_register(struct rackvm *vm, uint64_t offset, uint32_t value)
{
    struct rackvm_virtio_blk *block = vm->block;
    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        block->device_features_select = value;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        block->driver_features_select = value;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (block->driver_features_select < 2) {
            uint64_t shift = block->driver_features_select ? 32 : 0;
            block->driver_features = (block->driver_features & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)value << shift);
        }
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        block->queue_select = value;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        if (block->queue_select == 0 && value > 0 && value <= VIRTIO_QUEUE_SIZE)
            block->queue.size = (uint16_t)value;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        if (block->queue_select == 0)
            block->queue.ready = value == 1;
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        if (value == 0)
            process_queue(vm);
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        block->interrupt_status &= ~value;
        if (!block->interrupt_status)
            set_irq(vm, false);
        break;
    case VIRTIO_MMIO_STATUS:
        if (value == 0)
            reset_device(block);
        else
            block->status = value;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        update_half(&block->queue.descriptors, offset == VIRTIO_MMIO_QUEUE_DESC_HIGH, value);
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        update_half(&block->queue.available, offset == VIRTIO_MMIO_QUEUE_AVAIL_HIGH, value);
        break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        update_half(&block->queue.used, offset == VIRTIO_MMIO_QUEUE_USED_HIGH, value);
        break;
    }
}

int rackvm_virtio_blk_init(struct rackvm *vm, char *error, size_t error_size)
{
    if (!vm->config.disk[0])
        return 0;
    struct rackvm_virtio_blk *block = calloc(1, sizeof(*block));
    if (!block) {
        rackvm_set_error(error, error_size, "Cannot allocate virtio block state");
        return -1;
    }
    block->disk_fd = open(vm->config.disk, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (block->disk_fd < 0) {
        rackvm_set_error(error, error_size, "%s: %s", vm->config.disk, strerror(errno));
        free(block);
        return -1;
    }
    struct stat status;
    if (fstat(block->disk_fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 512) {
        rackvm_set_error(error, error_size, "%s: disk image must be a regular file of at least 512 bytes", vm->config.disk);
        close(block->disk_fd);
        free(block);
        return -1;
    }
    block->capacity = (uint64_t)status.st_size / 512;
    if (pthread_mutex_init(&block->lock, NULL) != 0) {
        rackvm_set_error(error, error_size, "Cannot initialise virtio block lock");
        close(block->disk_fd);
        free(block);
        return -1;
    }
    vm->block = block;
    return 0;
}

void rackvm_virtio_blk_destroy(struct rackvm *vm)
{
    if (!vm->block)
        return;
    if (vm->block->disk_fd >= 0)
        close(vm->block->disk_fd);
    pthread_mutex_destroy(&vm->block->lock);
    free(vm->block);
    vm->block = NULL;
}

bool rackvm_virtio_blk_mmio(struct rackvm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    if (!vcpu->vm->block || run->mmio.phys_addr < VIRTIO_MMIO_BASE || run->mmio.phys_addr >= VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE)
        return false;
    uint64_t offset = run->mmio.phys_addr - VIRTIO_MMIO_BASE;
    pthread_mutex_lock(&vcpu->vm->block->lock);
    if (run->mmio.len != sizeof(uint32_t)) {
        if (!run->mmio.is_write)
            memset(run->mmio.data, 0, run->mmio.len);
        pthread_mutex_unlock(&vcpu->vm->block->lock);
        return true;
    }
    uint32_t value = 0;
    if (run->mmio.is_write) {
        memcpy(&value, run->mmio.data, sizeof(value));
        write_register(vcpu->vm, offset, le32toh(value));
    } else {
        value = htole32(read_register(vcpu->vm, offset));
        memcpy(run->mmio.data, &value, sizeof(value));
    }
    pthread_mutex_unlock(&vcpu->vm->block->lock);
    return true;
}
