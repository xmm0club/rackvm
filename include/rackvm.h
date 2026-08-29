#ifndef RACKVM_H
#define RACKVM_H

#include <asm/bootparam.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>
#include <sys/types.h>

#define RACKVM_VERSION "0.1.0"
#define RACKVM_MAX_VCPUS 64
#define RACKVM_NAME_MAX 64
#define RACKVM_PATH_MAX 4096
#define RACKVM_CMDLINE_MAX 4096
#define RACKVM_SERIAL_QUEUE 4096

struct rackvm_config {
    char name[RACKVM_NAME_MAX];
    char kernel[RACKVM_PATH_MAX];
    char initrd[RACKVM_PATH_MAX];
    char disk[RACKVM_PATH_MAX];
    char cmdline[RACKVM_CMDLINE_MAX];
    uint64_t memory_mib;
    unsigned int cpus;
    bool interactive;
};

struct rackvm_serial {
    pthread_mutex_t lock;
    uint8_t queue[RACKVM_SERIAL_QUEUE];
    size_t head;
    size_t tail;
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scratch;
    uint16_t divisor;
    bool irq_asserted;
};

struct rackvm_serial_state {
    uint8_t queue[RACKVM_SERIAL_QUEUE];
    uint64_t head;
    uint64_t tail;
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scratch;
    uint16_t divisor;
    uint8_t irq_asserted;
};

struct rackvm;
struct rackvm_virtio_blk;

struct rackvm_vcpu {
    struct rackvm *vm;
    int fd;
    unsigned int id;
    struct kvm_run *run;
    size_t run_size;
    pthread_t thread;
    bool thread_started;
};

struct rackvm {
    struct rackvm_config config;
    int kvm_fd;
    int vm_fd;
    void *memory;
    uint64_t memory_size;
    struct rackvm_vcpu *vcpus;
    unsigned int vcpu_count;
    struct rackvm_serial serial;
    struct rackvm_virtio_blk *block;
    pthread_t input_thread;
    bool input_started;
    struct termios saved_termios;
    bool terminal_raw;
    atomic_bool stopping;
    atomic_int exit_status;
};

void rackvm_config_defaults(struct rackvm_config *config);
int rackvm_config_load(const char *path, struct rackvm_config *config, char *error, size_t error_size);
int rackvm_config_validate(const struct rackvm_config *config, char *error, size_t error_size);
int rackvm_config_resolve_paths(struct rackvm_config *config, const char *config_path, char *error, size_t error_size);

int rackvm_vm_run(const struct rackvm_config *config);
int rackvm_snapshot_create(const struct rackvm_config *config, const char *path);
int rackvm_snapshot_resume(const char *path);
int rackvm_guest_verify(const struct rackvm_config *config);
int rackvm_doctor(void);
int rackvm_kernel_inspect(const char *path);
int rackvm_devirtualise(const struct rackvm_config *config, const char *output);

int rackvm_serial_init(struct rackvm *vm);
void rackvm_serial_destroy(struct rackvm *vm);
void rackvm_serial_start_input(struct rackvm *vm);
void rackvm_serial_stop_input(struct rackvm *vm);
bool rackvm_serial_io(struct rackvm_vcpu *vcpu);
void rackvm_serial_save(struct rackvm *vm, struct rackvm_serial_state *state);
void rackvm_serial_restore(struct rackvm *vm, const struct rackvm_serial_state *state);

int rackvm_virtio_blk_init(struct rackvm *vm, char *error, size_t error_size);
void rackvm_virtio_blk_destroy(struct rackvm *vm);
bool rackvm_virtio_blk_mmio(struct rackvm_vcpu *vcpu);

int rackvm_vcpu_sandbox(void);

int rackvm_read_file(const char *path, uint8_t **data, size_t *size, char *error, size_t error_size);
int rackvm_copy_file(const char *source, const char *destination, mode_t mode, char *error, size_t error_size);
int rackvm_mkdir(const char *path, mode_t mode, char *error, size_t error_size);
const char *rackvm_basename(const char *path);
void rackvm_sha256_file(const char *path, char output[65], char *error, size_t error_size);
void rackvm_sha256_file_at(int directory, const char *name, char output[65], char *error, size_t error_size);
void rackvm_set_error(char *buffer, size_t size, const char *format, ...);

#endif
