#define _GNU_SOURCE
#include "rackvm.h"

#include <asm/boot.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BOOT_PARAMS_ADDRESS 0x10000ULL
#define COMMAND_LINE_ADDRESS 0x20000ULL
#define GDT_ADDRESS 0x500ULL
#define KERNEL_ADDRESS 0x100000ULL
#define MP_CONFIG_ADDRESS 0x98000ULL
#define MP_TABLE_ADDRESS 0x9fc00ULL
#define TSS_ADDRESS 0xfffbd000ULL
#define IDENTITY_MAP_ADDRESS 0xffffc000ULL
#define E820_RAM 1
#define E820_RESERVED 2

struct mp_floating_pointer {
    char signature[4];
    uint32_t configuration;
    uint8_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t feature[5];
} __attribute__((packed));

struct mp_configuration {
    char signature[4];
    uint16_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[8];
    char product[12];
    uint32_t oem_table;
    uint16_t oem_length;
    uint16_t entry_count;
    uint32_t lapic_address;
    uint16_t extended_length;
    uint8_t extended_checksum;
    uint8_t reserved;
} __attribute__((packed));

struct mp_processor {
    uint8_t type;
    uint8_t apic_id;
    uint8_t apic_version;
    uint8_t flags;
    uint32_t signature;
    uint32_t features;
    uint32_t reserved[2];
} __attribute__((packed));

struct mp_bus {
    uint8_t type;
    uint8_t id;
    char name[6];
} __attribute__((packed));

struct mp_ioapic {
    uint8_t type;
    uint8_t id;
    uint8_t version;
    uint8_t flags;
    uint32_t address;
} __attribute__((packed));

struct mp_interrupt {
    uint8_t type;
    uint8_t interrupt_type;
    uint16_t flags;
    uint8_t source_bus;
    uint8_t source_irq;
    uint8_t destination_apic;
    uint8_t destination_irq;
} __attribute__((packed));

static sigset_t saved_signal_mask;

static uint8_t checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < size; i++)
        sum += bytes[i];
    return (uint8_t)(0 - sum);
}

static void stop_vm(struct rackvm *vm, int status)
{
    bool was_stopping = atomic_exchange(&vm->stopping, true);
    if (status)
        atomic_store(&vm->exit_status, status);
    if (was_stopping)
        return;
    for (unsigned int i = 0; i < vm->vcpu_count; i++) {
        if (vm->vcpus[i].run)
            vm->vcpus[i].run->immediate_exit = 1;
        if (vm->vcpus[i].thread_started)
            pthread_kill(vm->vcpus[i].thread, SIGUSR1);
    }
}

static void wake_handler(int signal_number)
{
    (void)signal_number;
}

static int install_signals(struct rackvm *vm)
{
    (void)vm;
    struct sigaction wake_action;
    memset(&wake_action, 0, sizeof(wake_action));
    wake_action.sa_handler = wake_handler;
    sigemptyset(&wake_action.sa_mask);
    if (sigaction(SIGUSR1, &wake_action, NULL) < 0)
        return -1;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    return pthread_sigmask(SIG_BLOCK, &blocked, &saved_signal_mask);
}

static void restore_signals(void)
{
    pthread_sigmask(SIG_SETMASK, &saved_signal_mask, NULL);
    signal(SIGUSR1, SIG_DFL);
}

static void configure_segment(struct kvm_segment *segment, uint16_t selector, uint8_t type)
{
    memset(segment, 0, sizeof(*segment));
    segment->base = 0;
    segment->limit = 0xffffffff;
    segment->selector = selector;
    segment->type = type;
    segment->present = 1;
    segment->dpl = 0;
    segment->db = 1;
    segment->s = 1;
    segment->l = 0;
    segment->g = 1;
}

static int setup_boot_cpu(struct rackvm_vcpu *vcpu)
{
    struct rackvm *vm = vcpu->vm;
    uint64_t *gdt = (uint64_t *)((uint8_t *)vm->memory + GDT_ADDRESS);
    gdt[0] = 0;
    gdt[1] = 0x00cf9b000000ffffULL;
    gdt[2] = 0x00cf93000000ffffULL;
    struct kvm_sregs sregs;
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
        return -1;
    sregs.gdt.base = GDT_ADDRESS;
    sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;
    configure_segment(&sregs.cs, 1 << 3, 11);
    configure_segment(&sregs.ds, 2 << 3, 3);
    sregs.es = sregs.ds;
    sregs.fs = sregs.ds;
    sregs.gs = sregs.ds;
    sregs.ss = sregs.ds;
    sregs.cr0 |= 1;
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
        return -1;
    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = KERNEL_ADDRESS;
    regs.rsi = BOOT_PARAMS_ADDRESS;
    regs.rflags = 2;
    regs.rsp = 0x8000;
    return ioctl(vcpu->fd, KVM_SET_REGS, &regs);
}

static int setup_cpuid(struct rackvm *vm, int vcpu_fd)
{
    size_t bytes = sizeof(struct kvm_cpuid2) + 256 * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = calloc(1, bytes);
    if (!cpuid)
        return -1;
    cpuid->nent = 256;
    int status = ioctl(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid);
    if (status == 0)
        status = ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid);
    free(cpuid);
    return status;
}

static void build_mp_table(struct rackvm *vm)
{
    uint8_t *base = (uint8_t *)vm->memory + MP_TABLE_ADDRESS;
    memset(base, 0, 1024);
    uint16_t *ebda = (uint16_t *)((uint8_t *)vm->memory + 0x40e);
    *ebda = MP_TABLE_ADDRESS >> 4;
    struct mp_floating_pointer *floating = (struct mp_floating_pointer *)base;
    struct mp_configuration *configuration = (struct mp_configuration *)((uint8_t *)vm->memory + MP_CONFIG_ADDRESS);
    memset(configuration, 0, MP_TABLE_ADDRESS - MP_CONFIG_ADDRESS);
    memcpy(floating->signature, "_MP_", 4);
    floating->configuration = MP_CONFIG_ADDRESS;
    floating->length = 1;
    floating->revision = 4;
    memcpy(configuration->signature, "PCMP", 4);
    configuration->revision = 4;
    memcpy(configuration->oem, "RACKVM  ", 8);
    memcpy(configuration->product, "RACKVM VMM  ", 12);
    configuration->lapic_address = 0xfee00000;
    uint8_t *entry = (uint8_t *)(configuration + 1);
    for (unsigned int i = 0; i < vm->vcpu_count; i++) {
        struct mp_processor processor = {
            .type = 0,
            .apic_id = (uint8_t)i,
            .apic_version = 0x14,
            .flags = 1 | (i == 0 ? 2 : 0),
            .signature = 0x600,
            .features = 0x201
        };
        memcpy(entry, &processor, sizeof(processor));
        entry += sizeof(processor);
        configuration->entry_count++;
    }
    struct mp_bus bus = {.type = 1, .id = 0, .name = {'I', 'S', 'A', ' ', ' ', ' '}};
    memcpy(entry, &bus, sizeof(bus));
    entry += sizeof(bus);
    configuration->entry_count++;
    uint8_t ioapic_id = (uint8_t)vm->vcpu_count;
    struct mp_ioapic ioapic = {.type = 2, .id = ioapic_id, .version = 0x11, .flags = 1, .address = 0xfec00000};
    memcpy(entry, &ioapic, sizeof(ioapic));
    entry += sizeof(ioapic);
    configuration->entry_count++;
    for (uint8_t irq = 0; irq < 16; irq++) {
        struct mp_interrupt interrupt = {.type = 3, .interrupt_type = 0, .source_bus = 0, .source_irq = irq, .destination_apic = ioapic_id, .destination_irq = irq};
        memcpy(entry, &interrupt, sizeof(interrupt));
        entry += sizeof(interrupt);
        configuration->entry_count++;
    }
    struct mp_interrupt extint = {.type = 4, .interrupt_type = 3, .source_bus = 0, .source_irq = 0, .destination_apic = 0xff, .destination_irq = 0};
    memcpy(entry, &extint, sizeof(extint));
    entry += sizeof(extint);
    configuration->entry_count++;
    struct mp_interrupt nmi = {.type = 4, .interrupt_type = 1, .source_bus = 0, .source_irq = 0, .destination_apic = 0xff, .destination_irq = 1};
    memcpy(entry, &nmi, sizeof(nmi));
    entry += sizeof(nmi);
    configuration->entry_count++;
    configuration->length = (uint16_t)(entry - (uint8_t *)configuration);
    configuration->checksum = checksum(configuration, configuration->length);
    floating->checksum = checksum(floating, sizeof(*floating));
}

static int load_guest(struct rackvm *vm, char *error, size_t error_size)
{
    uint8_t *kernel = NULL;
    size_t kernel_size = 0;
    if (rackvm_read_file(vm->config.kernel, &kernel, &kernel_size, error, error_size) < 0)
        return -1;
    if (kernel_size < 0x1f1 + sizeof(struct setup_header)) {
        rackvm_set_error(error, error_size, "%s: not a Linux boot-protocol image", vm->config.kernel);
        free(kernel);
        return -1;
    }
    struct setup_header header;
    memcpy(&header, kernel + 0x1f1, sizeof(header));
    if (header.boot_flag != 0xaa55 || header.header != 0x53726448 || header.version < 0x0206 || !(header.loadflags & LOADED_HIGH)) {
        rackvm_set_error(error, error_size, "%s: unsupported Linux kernel image", vm->config.kernel);
        free(kernel);
        return -1;
    }
    size_t setup_sectors = header.setup_sects ? header.setup_sects : 4;
    size_t setup_size = (setup_sectors + 1) * 512;
    if (setup_size >= kernel_size) {
        rackvm_set_error(error, error_size, "%s: truncated Linux kernel image", vm->config.kernel);
        free(kernel);
        return -1;
    }
    size_t payload_size = kernel_size - setup_size;
    uint64_t runtime_size = payload_size;
    if (header.version >= 0x020a && header.init_size > runtime_size)
        runtime_size = header.init_size;
    if (KERNEL_ADDRESS + runtime_size > vm->memory_size) {
        rackvm_set_error(error, error_size, "Kernel does not fit in guest memory");
        free(kernel);
        return -1;
    }
    memcpy((uint8_t *)vm->memory + KERNEL_ADDRESS, kernel + setup_size, payload_size);
    struct boot_params *parameters = (struct boot_params *)((uint8_t *)vm->memory + BOOT_PARAMS_ADDRESS);
    memset(parameters, 0, sizeof(*parameters));
    parameters->hdr = header;
    parameters->hdr.type_of_loader = 0xff;
    parameters->hdr.loadflags |= CAN_USE_HEAP;
    parameters->hdr.heap_end_ptr = 0xfe00;
    parameters->hdr.cmd_line_ptr = COMMAND_LINE_ADDRESS;
    parameters->hdr.code32_start = KERNEL_ADDRESS;
    parameters->screen_info.orig_video_cols = 80;
    parameters->screen_info.orig_video_lines = 25;
    parameters->screen_info.orig_video_mode = 3;
    parameters->screen_info.orig_video_isVGA = VIDEO_TYPE_VGAC;
    parameters->screen_info.orig_video_points = 16;
    size_t cmdline_length = strlen(vm->config.cmdline) + 1;
    if (header.cmdline_size && cmdline_length > header.cmdline_size) {
        rackvm_set_error(error, error_size, "Kernel command line exceeds the image's %u-byte limit", header.cmdline_size);
        free(kernel);
        return -1;
    }
    if (COMMAND_LINE_ADDRESS + cmdline_length >= 0x9f000) {
        rackvm_set_error(error, error_size, "Kernel command line is too long");
        free(kernel);
        return -1;
    }
    memcpy((uint8_t *)vm->memory + COMMAND_LINE_ADDRESS, vm->config.cmdline, cmdline_length);
    parameters->e820_entries = 3;
    parameters->e820_table[0] = (struct boot_e820_entry){.addr = 0, .size = MP_CONFIG_ADDRESS, .type = E820_RAM};
    parameters->e820_table[1] = (struct boot_e820_entry){.addr = MP_CONFIG_ADDRESS, .size = 0x68000, .type = E820_RESERVED};
    parameters->e820_table[2] = (struct boot_e820_entry){.addr = 0x100000, .size = vm->memory_size - 0x100000, .type = E820_RAM};
    uint64_t legacy_memory_kib = (vm->memory_size - 0x100000) / 1024;
    parameters->alt_mem_k = legacy_memory_kib > UINT32_MAX ? UINT32_MAX : (uint32_t)legacy_memory_kib;
    if (vm->config.initrd[0]) {
        uint8_t *initrd = NULL;
        size_t initrd_size = 0;
        if (rackvm_read_file(vm->config.initrd, &initrd, &initrd_size, error, error_size) < 0) {
            free(kernel);
            return -1;
        }
        uint64_t maximum = header.initrd_addr_max ? (uint64_t)header.initrd_addr_max + 1 : 0x38000000ULL;
        if (maximum > vm->memory_size)
            maximum = vm->memory_size;
        if (initrd_size > maximum) {
            rackvm_set_error(error, error_size, "Initramfs does not fit in guest memory");
            free(initrd);
            free(kernel);
            return -1;
        }
        uint64_t address = (maximum - initrd_size) & ~0xfffULL;
        uint64_t kernel_end = (KERNEL_ADDRESS + runtime_size + 0x1fffff) & ~0x1fffffULL;
        if (address < kernel_end || address > UINT32_MAX || initrd_size > UINT32_MAX) {
            rackvm_set_error(error, error_size, "Kernel and initramfs overlap; increase guest memory");
            free(initrd);
            free(kernel);
            return -1;
        }
        memcpy((uint8_t *)vm->memory + address, initrd, initrd_size);
        parameters->hdr.ramdisk_image = (uint32_t)address;
        parameters->hdr.ramdisk_size = (uint32_t)initrd_size;
        free(initrd);
    }
    build_mp_table(vm);
    free(kernel);
    return 0;
}

static bool handle_io(struct rackvm_vcpu *vcpu)
{
    if (rackvm_serial_io(vcpu))
        return true;
    struct kvm_run *run = vcpu->run;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    if (run->io.direction == KVM_EXIT_IO_IN) {
        memset(data, 0xff, (size_t)run->io.count * run->io.size);
        return true;
    }
    for (uint32_t index = 0; index < run->io.count; index++) {
        uint64_t value = 0;
        memcpy(&value, data + (size_t)index * run->io.size, run->io.size);
        if ((run->io.port == 0x604 && (value & 0x2000)) || (run->io.port == 0x64 && (value & 0xff) == 0xfe) || (run->io.port == 0xcf9 && (value & 6))) {
            stop_vm(vcpu->vm, 0);
            return true;
        }
    }
    return true;
}

static int run_vcpu(struct rackvm_vcpu *vcpu)
{
    for (;;) {
        vcpu->run->immediate_exit = 0;
        if (atomic_load(&vcpu->vm->stopping)) {
            vcpu->run->immediate_exit = 1;
            errno = EINTR;
            return -1;
        }
        if (ioctl(vcpu->fd, KVM_RUN, 0) == 0)
            return 0;

        int error = errno;
        if (atomic_load(&vcpu->vm->stopping) || (error != EINTR && error != EAGAIN)) {
            errno = error;
            return -1;
        }

        if (error == EAGAIN)
            sched_yield();
    }
}

static void *vcpu_main(void *argument)
{
    struct rackvm_vcpu *vcpu = argument;
    struct rackvm *vm = vcpu->vm;
    while (!atomic_load(&vm->stopping)) {
        if (run_vcpu(vcpu) < 0) {
            if (atomic_load(&vm->stopping))
                break;
            fprintf(stderr, "RackVM: vCPU %u failed: %s\n", vcpu->id, strerror(errno));
            stop_vm(vm, 1);
            break;
        }
        switch (vcpu->run->exit_reason) {
        case KVM_EXIT_IO:
            handle_io(vcpu);
            break;
        case KVM_EXIT_MMIO:
            if (!vcpu->run->mmio.is_write)
                memset(vcpu->run->mmio.data, 0xff, vcpu->run->mmio.len);
            break;
        case KVM_EXIT_HLT:
        case KVM_EXIT_INTR:
            break;
        case KVM_EXIT_SYSTEM_EVENT:
            stop_vm(vm, 0);
            break;
        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr, "\nRackVM: The guest entered a shutdown or triple-fault state.\n");
            stop_vm(vm, 1);
            break;
        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr, "\nRackVM: vCPU %u failed to enter KVM (reason 0x%llx).\n", vcpu->id, (unsigned long long)vcpu->run->fail_entry.hardware_entry_failure_reason);
            stop_vm(vm, 1);
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "\nRackVM: KVM reported an internal error on vCPU %u.\n", vcpu->id);
            stop_vm(vm, 1);
            break;
        default:
            fprintf(stderr, "\nRackVM: Unsupported KVM exit %u on vCPU %u.\n", vcpu->run->exit_reason, vcpu->id);
            stop_vm(vm, 1);
            break;
        }
    }
    return NULL;
}

static void destroy_vm(struct rackvm *vm)
{
    stop_vm(vm, atomic_load(&vm->exit_status));
    rackvm_serial_destroy(vm);
    if (vm->vcpus) {
        for (unsigned int i = 0; i < vm->vcpu_count; i++) {
            if (vm->vcpus[i].run)
                munmap(vm->vcpus[i].run, vm->vcpus[i].run_size);
            if (vm->vcpus[i].fd >= 0)
                close(vm->vcpus[i].fd);
        }
    }
    free(vm->vcpus);
    if (vm->memory)
        munmap(vm->memory, vm->memory_size);
    if (vm->vm_fd >= 0)
        close(vm->vm_fd);
    if (vm->kvm_fd >= 0)
        close(vm->kvm_fd);
}

static int initialize_vm(struct rackvm *vm, char *error, size_t error_size)
{
    vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (vm->kvm_fd < 0) {
        rackvm_set_error(error, error_size, "/dev/kvm: %s", strerror(errno));
        return -1;
    }
    int api = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) {
        rackvm_set_error(error, error_size, "KVM API version %d is unsupported", api);
        return -1;
    }
    int maximum = ioctl(vm->kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
    if (maximum > 0 && vm->config.cpus > (unsigned int)maximum) {
        rackvm_set_error(error, error_size, "This KVM host supports at most %d vCPUs", maximum);
        return -1;
    }
    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0) {
        rackvm_set_error(error, error_size, "KVM_CREATE_VM: %s", strerror(errno));
        return -1;
    }
    unsigned long tss = TSS_ADDRESS;
    if (ioctl(vm->vm_fd, KVM_SET_TSS_ADDR, tss) < 0) {
        rackvm_set_error(error, error_size, "KVM_SET_TSS_ADDR: %s", strerror(errno));
        return -1;
    }
    uint64_t identity = IDENTITY_MAP_ADDRESS;
    if (ioctl(vm->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &identity) < 0) {
        rackvm_set_error(error, error_size, "KVM_SET_IDENTITY_MAP_ADDR: %s", strerror(errno));
        return -1;
    }
    if (ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        rackvm_set_error(error, error_size, "KVM_CREATE_IRQCHIP: %s", strerror(errno));
        return -1;
    }
    struct kvm_pit_config pit = {.flags = KVM_PIT_SPEAKER_DUMMY};
    if (ioctl(vm->vm_fd, KVM_CREATE_PIT2, &pit) < 0) {
        rackvm_set_error(error, error_size, "KVM_CREATE_PIT2: %s", strerror(errno));
        return -1;
    }
    vm->memory_size = vm->config.memory_mib * 1024ULL * 1024ULL;
    vm->memory = mmap(NULL, vm->memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vm->memory == MAP_FAILED) {
        vm->memory = NULL;
        rackvm_set_error(error, error_size, "Cannot allocate guest memory: %s", strerror(errno));
        return -1;
    }
    madvise(vm->memory, vm->memory_size, MADV_MERGEABLE);
    struct kvm_userspace_memory_region region = {.slot = 0, .guest_phys_addr = 0, .memory_size = vm->memory_size, .userspace_addr = (uintptr_t)vm->memory};
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        rackvm_set_error(error, error_size, "KVM_SET_USER_MEMORY_REGION: %s", strerror(errno));
        return -1;
    }
    vm->vcpu_count = vm->config.cpus;
    vm->vcpus = calloc(vm->vcpu_count, sizeof(*vm->vcpus));
    if (!vm->vcpus) {
        rackvm_set_error(error, error_size, "Cannot allocate vCPU state");
        return -1;
    }
    for (unsigned int i = 0; i < vm->vcpu_count; i++)
        vm->vcpus[i].fd = -1;
    int run_size = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (run_size < (int)sizeof(struct kvm_run)) {
        rackvm_set_error(error, error_size, "Invalid KVM vCPU mapping size");
        return -1;
    }
    for (unsigned int i = 0; i < vm->vcpu_count; i++) {
        struct rackvm_vcpu *vcpu = &vm->vcpus[i];
        vcpu->vm = vm;
        vcpu->id = i;
        vcpu->run_size = (size_t)run_size;
        vcpu->fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, i);
        if (vcpu->fd < 0) {
            rackvm_set_error(error, error_size, "KVM_CREATE_VCPU %u: %s", i, strerror(errno));
            return -1;
        }
        vcpu->run = mmap(NULL, vcpu->run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu->fd, 0);
        if (vcpu->run == MAP_FAILED) {
            vcpu->run = NULL;
            rackvm_set_error(error, error_size, "Cannot map vCPU %u: %s", i, strerror(errno));
            return -1;
        }
        if (setup_cpuid(vm, vcpu->fd) < 0) {
            rackvm_set_error(error, error_size, "Cannot configure CPUID for vCPU %u: %s", i, strerror(errno));
            return -1;
        }
    }
    if (load_guest(vm, error, error_size) < 0)
        return -1;
    if (setup_boot_cpu(&vm->vcpus[0]) < 0) {
        rackvm_set_error(error, error_size, "Cannot initialize the boot vCPU: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int rackvm_vm_run(const struct rackvm_config *config)
{
    struct rackvm vm;
    memset(&vm, 0, sizeof(vm));
    vm.config = *config;
    vm.kvm_fd = -1;
    vm.vm_fd = -1;
    atomic_init(&vm.stopping, false);
    atomic_init(&vm.exit_status, 0);
    char error[512];
    if (rackvm_serial_init(&vm) < 0) {
        fprintf(stderr, "RackVM: Cannot initialize the serial console.\n");
        return 1;
    }
    if (initialize_vm(&vm, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        destroy_vm(&vm);
        return 1;
    }
    if (install_signals(&vm) < 0) {
        fprintf(stderr, "RackVM: Cannot install signal handlers: %s\n", strerror(errno));
        destroy_vm(&vm);
        return 1;
    }
    printf("RackVM %s | %s | %llu MiB | %u vCPU%s\n", RACKVM_VERSION, vm.config.name, (unsigned long long)vm.config.memory_mib, vm.vcpu_count, vm.vcpu_count == 1 ? "" : "s");
    printf("Serial console attached. Press Ctrl+C to stop the VM.\n\n");
    fflush(stdout);
    rackvm_serial_start_input(&vm);
    for (unsigned int i = 0; i < vm.vcpu_count; i++) {
        if (pthread_create(&vm.vcpus[i].thread, NULL, vcpu_main, &vm.vcpus[i]) != 0) {
            fprintf(stderr, "RackVM: Cannot start vCPU %u.\n", i);
            stop_vm(&vm, 1);
            break;
        }
        vm.vcpus[i].thread_started = true;
    }
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    while (!atomic_load(&vm.stopping)) {
        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};
        int received = sigtimedwait(&stop_signals, NULL, &timeout);
        if (received == SIGINT || received == SIGTERM) {
            stop_vm(&vm, 0);
            break;
        }
        if (received < 0 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "RackVM: Signal wait failed: %s\n", strerror(errno));
            stop_vm(&vm, 1);
            break;
        }
    }
    for (unsigned int i = 0; i < vm.vcpu_count; i++) {
        if (vm.vcpus[i].thread_started) {
            pthread_join(vm.vcpus[i].thread, NULL);
            vm.vcpus[i].thread_started = false;
        }
    }
    stop_vm(&vm, atomic_load(&vm.exit_status));
    rackvm_serial_stop_input(&vm);
    restore_signals();
    int status = atomic_load(&vm.exit_status);
    destroy_vm(&vm);
    printf("\nRackVM: VM stopped.\n");
    return status;
}

int rackvm_guest_verify(const struct rackvm_config *config)
{
    struct rackvm vm;
    memset(&vm, 0, sizeof(vm));
    vm.config = *config;
    vm.memory_size = config->memory_mib * 1024ULL * 1024ULL;
    vm.vcpu_count = config->cpus;
    vm.memory = mmap(NULL, vm.memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vm.memory == MAP_FAILED) {
        fprintf(stderr, "RackVM: Cannot allocate verification memory: %s\n", strerror(errno));
        return 1;
    }
    char error[512];
    int status = load_guest(&vm, error, sizeof(error));
    munmap(vm.memory, vm.memory_size);
    if (status < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return 1;
    }
    printf("Guest image is bootable by RackVM.\n");
    printf("  Name:       %s\n", config->name);
    printf("  Memory:     %llu MiB\n", (unsigned long long)config->memory_mib);
    printf("  vCPUs:      %u\n", config->cpus);
    printf("  Kernel:     Linux x86 boot protocol\n");
    printf("  Initramfs:  %s\n", config->initrd[0] ? "Loaded and placed" : "Not configured");
    printf("  SMP table:  Generated\n");
    return 0;
}
