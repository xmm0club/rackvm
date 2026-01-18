#define _GNU_SOURCE
#include "rackvm.h"

#include <asm/bootparam.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fprintf(stream,
        "RackVM %s\n"
        "A compact Linux/KVM virtual machine monitor.\n\n"
        "Usage:\n"
        "  rackvm run CONFIG [OPTIONS]\n"
        "  rackvm run --kernel FILE [OPTIONS]\n"
        "  rackvm validate CONFIG\n"
        "  rackvm verify CONFIG\n"
        "  rackvm inspect KERNEL\n"
        "  rackvm devirtualise CONFIG --output DIRECTORY\n"
        "  rackvm doctor\n\n"
        "Run options:\n"
        "  --kernel FILE       Linux bzImage to boot\n"
        "  --initrd FILE       Initial RAM filesystem\n"
        "  --name NAME         Virtual machine name\n"
        "  --memory MIB        Guest memory, from 64 to 65536 MiB\n"
        "  --cpus COUNT        Virtual CPUs, from 1 to %u\n"
        "  --cmdline TEXT      Linux kernel command line\n"
        "  --no-interactive    Disable serial-console input\n",
        RACKVM_VERSION, RACKVM_MAX_VCPUS);
}

static int parse_number(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *output)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum)
        return -1;
    *output = value;
    return 0;
}

static int set_text(char *destination, size_t size, const char *value, const char *option)
{
    if (strlen(value) >= size) {
        fprintf(stderr, "RackVM: Value for %s is too long.\n", option);
        return -1;
    }
    snprintf(destination, size, "%s", value);
    return 0;
}

static int load_config(const char *path, struct rackvm_config *config)
{
    char error[512];
    rackvm_config_defaults(config);
    if (rackvm_config_load(path, config, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return -1;
    }
    if (rackvm_config_resolve_paths(config, path, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return -1;
    }
    return 0;
}

static int validate_config(const struct rackvm_config *config)
{
    char error[512];
    if (rackvm_config_validate(config, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return -1;
    }
    return 0;
}

static int apply_run_options(int argc, char **argv, int start, struct rackvm_config *config)
{
    for (int index = start; index < argc; index++) {
        const char *option = argv[index];
        if (!strcmp(option, "--no-interactive")) {
            config->interactive = false;
            continue;
        }
        if (index + 1 >= argc) {
            fprintf(stderr, "RackVM: %s requires a value.\n", option);
            return -1;
        }
        const char *value = argv[++index];
        if (!strcmp(option, "--kernel")) {
            if (set_text(config->kernel, sizeof(config->kernel), value, option) < 0)
                return -1;
        } else if (!strcmp(option, "--initrd")) {
            if (set_text(config->initrd, sizeof(config->initrd), value, option) < 0)
                return -1;
        } else if (!strcmp(option, "--name")) {
            if (set_text(config->name, sizeof(config->name), value, option) < 0)
                return -1;
        } else if (!strcmp(option, "--cmdline")) {
            if (set_text(config->cmdline, sizeof(config->cmdline), value, option) < 0)
                return -1;
        } else if (!strcmp(option, "--memory")) {
            if (parse_number(value, 64, 65536, &config->memory_mib) < 0) {
                fprintf(stderr, "RackVM: --memory must be between 64 and 65536 MiB.\n");
                return -1;
            }
        } else if (!strcmp(option, "--cpus")) {
            uint64_t cpus;
            if (parse_number(value, 1, RACKVM_MAX_VCPUS, &cpus) < 0) {
                fprintf(stderr, "RackVM: --cpus must be between 1 and %u.\n", RACKVM_MAX_VCPUS);
                return -1;
            }
            config->cpus = (unsigned int)cpus;
        } else {
            fprintf(stderr, "RackVM: Unknown option '%s'.\n", option);
            return -1;
        }
    }
    return 0;
}

static int command_run(int argc, char **argv)
{
    struct rackvm_config config;
    int start = 2;
    if (start < argc && argv[start][0] != '-') {
        if (load_config(argv[start], &config) < 0)
            return 1;
        start++;
    } else {
        rackvm_config_defaults(&config);
    }
    if (apply_run_options(argc, argv, start, &config) < 0 || validate_config(&config) < 0)
        return 1;
    return rackvm_vm_run(&config);
}

static int command_validate(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "RackVM: validate requires one configuration file.\n");
        return 1;
    }
    struct rackvm_config config;
    if (load_config(argv[2], &config) < 0 || validate_config(&config) < 0)
        return 1;
    printf("Configuration is valid.\n");
    printf("  Name:       %s\n", config.name);
    printf("  Memory:     %llu MiB\n", (unsigned long long)config.memory_mib);
    printf("  vCPUs:      %u\n", config.cpus);
    printf("  Kernel:     %s\n", config.kernel);
    printf("  Initramfs:  %s\n", config.initrd[0] ? config.initrd : "None");
    return 0;
}

int rackvm_kernel_inspect(const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    char error[512];
    if (rackvm_read_file(path, &data, &size, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return 1;
    }
    if (size < 0x1f1 + sizeof(struct setup_header)) {
        fprintf(stderr, "RackVM: %s is not a Linux boot-protocol image.\n", path);
        free(data);
        return 1;
    }
    struct setup_header header;
    memcpy(&header, data + 0x1f1, sizeof(header));
    if (header.boot_flag != 0xaa55 || header.header != 0x53726448) {
        fprintf(stderr, "RackVM: %s has an invalid Linux boot header.\n", path);
        free(data);
        return 1;
    }
    size_t setup_sectors = header.setup_sects ? header.setup_sects : 4;
    printf("Linux boot image\n");
    printf("  File:              %s\n", path);
    printf("  Size:              %.2f MiB\n", (double)size / (1024.0 * 1024.0));
    printf("  Protocol:          %u.%02x\n", header.version >> 8, header.version & 0xff);
    printf("  Architecture:      %s\n", header.xloadflags & XLF_KERNEL_64 ? "x86-64" : "x86");
    printf("  Setup sectors:     %zu\n", setup_sectors);
    printf("  Relocatable:       %s\n", header.relocatable_kernel ? "Yes" : "No");
    printf("  Command-line limit: %u bytes\n", header.cmdline_size ? header.cmdline_size : 255);
    printf("  Initramfs ceiling: 0x%08x\n", header.initrd_addr_max);
    free(data);
    return 0;
}

int rackvm_doctor(void)
{
    struct utsname system;
    if (uname(&system) < 0) {
        fprintf(stderr, "RackVM: uname failed: %s\n", strerror(errno));
        return 1;
    }
    bool architecture_ok = !strcmp(system.machine, "x86_64");
    printf("RackVM host diagnostics\n");
    printf("  Architecture:  %-12s %s\n", system.machine, architecture_ok ? "Ready" : "Unsupported");
    int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("  KVM device:    %-12s Unavailable (%s)\n", "/dev/kvm", strerror(errno));
        printf("  Result:        Host is not ready for guest execution.\n");
        return 1;
    }
    int api = ioctl(fd, KVM_GET_API_VERSION, 0);
    int memory = ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    int irqchip = ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP);
    int pit = ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_PIT2);
    int maximum = ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
    printf("  KVM device:    %-12s Ready\n", "/dev/kvm");
    printf("  KVM API:       %-12d %s\n", api, api == KVM_API_VERSION ? "Ready" : "Unsupported");
    printf("  Guest memory:  %-12s %s\n", memory > 0 ? "Supported" : "Missing", memory > 0 ? "Ready" : "Unsupported");
    printf("  IRQ chip:      %-12s %s\n", irqchip > 0 ? "Supported" : "Missing", irqchip > 0 ? "Ready" : "Unsupported");
    printf("  PIT2:          %-12s %s\n", pit > 0 ? "Supported" : "Missing", pit > 0 ? "Ready" : "Unsupported");
    printf("  Maximum vCPUs: %d\n", maximum);
    close(fd);
    bool ready = architecture_ok && api == KVM_API_VERSION && memory > 0 && irqchip > 0 && pit > 0;
    printf("  Result:        Host is %s.\n", ready ? "ready for RackVM" : "not ready for guest execution");
    return ready ? 0 : 1;
}

static int command_devirtualise(int argc, char **argv)
{
    if (argc != 5 || strcmp(argv[3], "--output")) {
        fprintf(stderr, "RackVM: Usage: rackvm devirtualise CONFIG --output DIRECTORY\n");
        return 1;
    }
    struct rackvm_config config;
    if (load_config(argv[2], &config) < 0 || validate_config(&config) < 0)
        return 1;
    return rackvm_devirtualise(&config, argv[4]);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version")) {
        printf("RackVM %s\n", RACKVM_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "run"))
        return command_run(argc, argv);
    if (!strcmp(argv[1], "validate"))
        return command_validate(argc, argv);
    if (!strcmp(argv[1], "verify")) {
        if (argc != 3) {
            fprintf(stderr, "RackVM: verify requires one configuration file.\n");
            return 1;
        }
        struct rackvm_config config;
        if (load_config(argv[2], &config) < 0 || validate_config(&config) < 0)
            return 1;
        return rackvm_guest_verify(&config);
    }
    if (!strcmp(argv[1], "inspect")) {
        if (argc != 3) {
            fprintf(stderr, "RackVM: inspect requires one Linux kernel image.\n");
            return 1;
        }
        return rackvm_kernel_inspect(argv[2]);
    }
    if (!strcmp(argv[1], "devirtualise"))
        return command_devirtualise(argc, argv);
    if (!strcmp(argv[1], "doctor")) {
        if (argc != 2) {
            fprintf(stderr, "RackVM: doctor does not accept arguments.\n");
            return 1;
        }
        return rackvm_doctor();
    }
    fprintf(stderr, "RackVM: Unknown command '%s'.\n", argv[1]);
    usage(stderr);
    return 1;
}
