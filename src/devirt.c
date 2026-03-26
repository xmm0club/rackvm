#define _GNU_SOURCE
#include "rackvm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int open_directory_path(const char *path, char *error, size_t error_size)
{
    int directory = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    char copy[RACKVM_PATH_MAX];
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy)) {
        rackvm_set_error(error, error_size, "Output path is too long");
        close(directory);
        return -1;
    }
    char *position = copy;
    char *component;
    while ((component = strsep(&position, "/")) != NULL) {
        if (!component[0] || !strcmp(component, "."))
            continue;
        int next = openat(directory, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            rackvm_set_error(error, error_size, "%s: cannot use output path component '%s': %s", path, component, strerror(errno));
            close(directory);
            return -1;
        }
        close(directory);
        directory = next;
    }
    return directory;
}

static int create_output_directory(const char *path, char *error, size_t error_size)
{
    char copy[RACKVM_PATH_MAX];
    int length = snprintf(copy, sizeof(copy), "%s", path);
    if (length <= 0 || length >= (int)sizeof(copy)) {
        rackvm_set_error(error, error_size, "Output path is empty or too long");
        return -1;
    }
    while (length > 1 && copy[length - 1] == '/')
        copy[--length] = '\0';
    char *slash = strrchr(copy, '/');
    char *name = slash ? slash + 1 : copy;
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) {
        rackvm_set_error(error, error_size, "%s: invalid output directory", path);
        return -1;
    }
    const char *parent = ".";
    if (slash) {
        if (slash == copy)
            parent = "/";
        else {
            *slash = '\0';
            parent = copy;
        }
    }
    int parent_fd = open_directory_path(parent, error, error_size);
    if (parent_fd < 0)
        return -1;
    if (mkdirat(parent_fd, name, 0755) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        close(parent_fd);
        return -1;
    }
    int output_fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (output_fd < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        unlinkat(parent_fd, name, AT_REMOVEDIR);
    }
    close(parent_fd);
    return output_fd;
}

static void json_string(FILE *file, const char *value)
{
    fputc('"', file);
    for (const unsigned char *current = (const unsigned char *)value; *current; current++) {
        switch (*current) {
        case '"':
            fputs("\\\"", file);
            break;
        case '\\':
            fputs("\\\\", file);
            break;
        case '\b':
            fputs("\\b", file);
            break;
        case '\f':
            fputs("\\f", file);
            break;
        case '\n':
            fputs("\\n", file);
            break;
        case '\r':
            fputs("\\r", file);
            break;
        case '\t':
            fputs("\\t", file);
            break;
        default:
            if (*current < 0x20)
                fprintf(file, "\\u%04x", *current);
            else
                fputc(*current, file);
            break;
        }
    }
    fputc('"', file);
}

static int copy_file_at(const char *source, int directory, const char *name, mode_t mode, char *error, size_t error_size)
{
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        rackvm_set_error(error, error_size, "%s: %s", source, strerror(errno));
        return -1;
    }
    int output = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (output < 0) {
        rackvm_set_error(error, error_size, "%s: %s", name, strerror(errno));
        close(input);
        return -1;
    }
    uint8_t buffer[65536];
    int status = 0;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count < 0) {
                rackvm_set_error(error, error_size, "%s: %s", source, strerror(errno));
                status = -1;
            }
            break;
        }
        size_t written = 0;
        while (written < (size_t)count) {
            ssize_t result = write(output, buffer + written, (size_t)count - written);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0) {
                rackvm_set_error(error, error_size, "%s: %s", name, strerror(errno));
                status = -1;
                break;
            }
            written += (size_t)result;
        }
        if (status < 0)
            break;
    }
    if (status == 0 && fsync(output) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", name, strerror(errno));
        status = -1;
    }
    close(input);
    close(output);
    if (status < 0)
        unlinkat(directory, name, 0);
    return status;
}

static FILE *create_text_file(int directory, const char *name, mode_t mode, char *error, size_t error_size)
{
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) {
        rackvm_set_error(error, error_size, "%s: %s", name, strerror(errno));
        return NULL;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        rackvm_set_error(error, error_size, "%s: %s", name, strerror(errno));
        close(fd);
        unlinkat(directory, name, 0);
        return NULL;
    }
    return file;
}

static int finish_text_file(FILE *file, int directory, const char *name, char *error, size_t error_size)
{
    int failure = 0;
    int saved_errno = 0;
    if (fflush(file) < 0) {
        failure = 1;
        saved_errno = errno;
    }
    if (!failure && fsync(fileno(file)) < 0) {
        failure = 1;
        saved_errno = errno;
    }
    if (fclose(file) < 0 && !failure) {
        failure = 1;
        saved_errno = errno;
    }
    if (failure) {
        rackvm_set_error(error, error_size, "%s: %s", name, strerror(saved_errno));
        unlinkat(directory, name, 0);
        return -1;
    }
    return 0;
}

static void write_grub_title(FILE *file, const char *name)
{
    for (const unsigned char *current = (const unsigned char *)name; *current; current++) {
        if ((*current >= 'a' && *current <= 'z') || (*current >= 'A' && *current <= 'Z') || (*current >= '0' && *current <= '9') || *current == ' ' || *current == '.' || *current == '_' || *current == '-')
            fputc(*current, file);
        else
            fputc('_', file);
    }
}

static void write_grub_cmdline(FILE *file, const char *cmdline)
{
    for (const unsigned char *current = (const unsigned char *)cmdline; *current; current++) {
        if ((*current >= 'a' && *current <= 'z') || (*current >= 'A' && *current <= 'Z') || (*current >= '0' && *current <= '9') || strchr(" =,./:_+-", *current))
            fputc(*current, file);
        else {
            fputc('\\', file);
            fputc(*current, file);
        }
    }
}

static void cleanup_bundle(const char *output, int directory)
{
    unlinkat(directory, "SHA256SUMS", 0);
    unlinkat(directory, "manifest.json", 0);
    unlinkat(directory, "grub.cfg", 0);
    unlinkat(directory, "initramfs", 0);
    unlinkat(directory, "vmlinuz", 0);
    close(directory);
    rmdir(output);
}

int rackvm_devirtualise(const struct rackvm_config *config, const char *output)
{
    char error[512] = {0};
    int output_fd = create_output_directory(output, error, sizeof(error));
    if (output_fd < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return 1;
    }
    if (copy_file_at(config->kernel, output_fd, "vmlinuz", 0644, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    if (config->initrd[0] && copy_file_at(config->initrd, output_fd, "initramfs", 0644, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    FILE *grub = create_text_file(output_fd, "grub.cfg", 0644, error, sizeof(error));
    if (!grub) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    fputs("set timeout=3\nset default=0\n\nmenuentry 'RackVM: ", grub);
    write_grub_title(grub, config->name);
    fputs("' {\n    linux /rackvm/vmlinuz ", grub);
    write_grub_cmdline(grub, config->cmdline);
    fputc('\n', grub);
    if (config->initrd[0])
        fputs("    initrd /rackvm/initramfs\n", grub);
    fputs("}\n", grub);
    if (finish_text_file(grub, output_fd, "grub.cfg", error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    char kernel_hash[65];
    char initrd_hash[65] = {0};
    rackvm_sha256_file_at(output_fd, "vmlinuz", kernel_hash, error, sizeof(error));
    if (!kernel_hash[0]) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    if (config->initrd[0]) {
        rackvm_sha256_file_at(output_fd, "initramfs", initrd_hash, error, sizeof(error));
        if (!initrd_hash[0]) {
            fprintf(stderr, "RackVM: %s\n", error);
            goto failure;
        }
    }
    FILE *manifest = create_text_file(output_fd, "manifest.json", 0644, error, sizeof(error));
    if (!manifest) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    fputs("{\n  \"format\": \"rackvm-bare-metal-v1\",\n  \"name\": ", manifest);
    json_string(manifest, config->name);
    fprintf(manifest, ",\n  \"memory_mib\": %llu,\n  \"cpus\": %u,\n  \"cmdline\": ", (unsigned long long)config->memory_mib, config->cpus);
    json_string(manifest, config->cmdline);
    fprintf(manifest, ",\n  \"kernel\": {\"path\": \"vmlinuz\", \"sha256\": \"%s\"}", kernel_hash);
    if (config->initrd[0])
        fprintf(manifest, ",\n  \"initramfs\": {\"path\": \"initramfs\", \"sha256\": \"%s\"}", initrd_hash);
    fputs("\n}\n", manifest);
    if (finish_text_file(manifest, output_fd, "manifest.json", error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    FILE *checksums = create_text_file(output_fd, "SHA256SUMS", 0644, error, sizeof(error));
    if (!checksums) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    fprintf(checksums, "%s  vmlinuz\n", kernel_hash);
    if (config->initrd[0])
        fprintf(checksums, "%s  initramfs\n", initrd_hash);
    if (finish_text_file(checksums, output_fd, "SHA256SUMS", error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    if (fsync(output_fd) < 0) {
        fprintf(stderr, "RackVM: %s: %s\n", output, strerror(errno));
        goto failure;
    }
    close(output_fd);
    printf("Bare-metal bundle created at %s\n", output);
    printf("Copy its contents to /rackvm on a GRUB-bootable system.\n");
    return 0;

failure:
    cleanup_bundle(output, output_fd);
    return 1;
}
