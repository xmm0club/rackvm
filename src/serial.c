#define _GNU_SOURCE
#include "rackvm.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define COM1 0x3f8

static size_t queue_size(const struct rackvm_serial *serial)
{
    return (serial->head + RACKVM_SERIAL_QUEUE - serial->tail) % RACKVM_SERIAL_QUEUE;
}

static void set_irq(struct rackvm *vm, bool level)
{
    if (vm->serial.irq_asserted == level || vm->vm_fd < 0)
        return;
    struct kvm_irq_level irq = {.irq = 4, .level = level ? 1U : 0U};
    if (ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq) == 0)
        vm->serial.irq_asserted = level;
}

static void refresh_irq(struct rackvm *vm)
{
    set_irq(vm, queue_size(&vm->serial) > 0 && (vm->serial.ier & 1));
}

static void queue_byte(struct rackvm *vm, uint8_t value)
{
    pthread_mutex_lock(&vm->serial.lock);
    size_t next = (vm->serial.head + 1) % RACKVM_SERIAL_QUEUE;
    if (next != vm->serial.tail) {
        vm->serial.queue[vm->serial.head] = value;
        vm->serial.head = next;
    }
    refresh_irq(vm);
    pthread_mutex_unlock(&vm->serial.lock);
}

static uint8_t serial_read(struct rackvm *vm, uint16_t offset)
{
    struct rackvm_serial *serial = &vm->serial;
    uint8_t value = 0xff;
    pthread_mutex_lock(&serial->lock);
    switch (offset) {
    case 0:
        if (serial->lcr & 0x80) {
            value = (uint8_t)(serial->divisor & 0xff);
        } else if (queue_size(serial)) {
            value = serial->queue[serial->tail];
            serial->tail = (serial->tail + 1) % RACKVM_SERIAL_QUEUE;
        } else {
            value = 0;
        }
        break;
    case 1:
        value = serial->lcr & 0x80 ? (uint8_t)(serial->divisor >> 8) : serial->ier;
        break;
    case 2:
        value = queue_size(serial) && (serial->ier & 1) ? 0x04 : 0x01;
        break;
    case 3:
        value = serial->lcr;
        break;
    case 4:
        value = serial->mcr;
        break;
    case 5:
        value = 0x60 | (queue_size(serial) ? 0x01 : 0);
        break;
    case 6:
        value = 0xb0;
        break;
    case 7:
        value = serial->scratch;
        break;
    }
    refresh_irq(vm);
    pthread_mutex_unlock(&serial->lock);
    return value;
}

static void serial_write(struct rackvm *vm, uint16_t offset, uint8_t value)
{
    struct rackvm_serial *serial = &vm->serial;
    if (offset == 0 && !(serial->lcr & 0x80)) {
        ssize_t ignored = write(STDOUT_FILENO, &value, 1);
        (void)ignored;
        return;
    }
    pthread_mutex_lock(&serial->lock);
    switch (offset) {
    case 0:
        serial->divisor = (serial->divisor & 0xff00) | value;
        break;
    case 1:
        if (serial->lcr & 0x80)
            serial->divisor = (uint16_t)((serial->divisor & 0x00ff) | ((uint16_t)value << 8));
        else
            serial->ier = value & 0x0f;
        break;
    case 2:
        if (value & 2)
            serial->head = serial->tail = 0;
        break;
    case 3:
        serial->lcr = value;
        break;
    case 4:
        serial->mcr = value;
        break;
    case 7:
        serial->scratch = value;
        break;
    }
    refresh_irq(vm);
    pthread_mutex_unlock(&serial->lock);
}

bool rackvm_serial_io(struct rackvm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint16_t port = run->io.port;
    if (port < COM1 || port > COM1 + 7)
        return false;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    for (uint32_t index = 0; index < run->io.count; index++) {
        uint8_t *item = data + (size_t)index * run->io.size;
        if (run->io.direction == KVM_EXIT_IO_OUT) {
            uint64_t value = 0;
            memcpy(&value, item, run->io.size);
            for (uint8_t byte = 0; byte < run->io.size; byte++)
                serial_write(vcpu->vm, port + byte, (value >> (byte * 8)) & 0xff);
        } else {
            uint64_t value = 0;
            for (uint8_t byte = 0; byte < run->io.size; byte++)
                value |= (uint64_t)serial_read(vcpu->vm, port + byte) << (byte * 8);
            memcpy(item, &value, run->io.size);
        }
    }
    return true;
}

static void *input_main(void *argument)
{
    struct rackvm *vm = argument;
    struct pollfd descriptor = {.fd = STDIN_FILENO, .events = POLLIN};
    while (!atomic_load(&vm->stopping)) {
        int ready = poll(&descriptor, 1, 100);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        uint8_t buffer[128];
        ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count <= 0)
            break;
        for (ssize_t index = 0; index < count; index++)
            queue_byte(vm, buffer[index]);
    }
    return NULL;
}

int rackvm_serial_init(struct rackvm *vm)
{
    memset(&vm->serial, 0, sizeof(vm->serial));
    vm->serial.divisor = 1;
    if (pthread_mutex_init(&vm->serial.lock, NULL) != 0)
        return -1;
    if (vm->config.interactive && isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &vm->saved_termios) == 0) {
            struct termios raw = vm->saved_termios;
            cfmakeraw(&raw);
            raw.c_oflag |= OPOST;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
                vm->terminal_raw = true;
        }
    }
    return 0;
}

void rackvm_serial_start_input(struct rackvm *vm)
{
    if (!vm->config.interactive)
        return;
    if (pthread_create(&vm->input_thread, NULL, input_main, vm) == 0)
        vm->input_started = true;
}

void rackvm_serial_stop_input(struct rackvm *vm)
{
    if (vm->input_started) {
        pthread_join(vm->input_thread, NULL);
        vm->input_started = false;
    }
    if (vm->terminal_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &vm->saved_termios);
        vm->terminal_raw = false;
    }
}

void rackvm_serial_destroy(struct rackvm *vm)
{
    rackvm_serial_stop_input(vm);
    pthread_mutex_destroy(&vm->serial.lock);
}
