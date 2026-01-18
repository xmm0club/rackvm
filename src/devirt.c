#define _GNU_SOURCE
#include "rackvm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int join_path(char output[RACKVM_PATH_MAX], const char *directory, const char *name, char *error, size_t error_size)
{
    int length = snprintf(output, RACKVM_PATH_MAX, "%s/%s", directory, name);
    if (length < 0 || length >= RACKVM_PATH_MAX) {
        rackvm_set_error(error, error_size, "Output path is too long");
        return -1;
    }
    return 0;
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

static FILE *create_text_file(const char *path, mode_t mode, char *error, size_t error_size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        return NULL;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        close(fd);
        unlink(path);
        return NULL;
    }
    return file;
}

static int finish_text_file(FILE *file, const char *path, char *error, size_t error_size)
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
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(saved_errno));
        unlink(path);
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

static void cleanup_bundle(const char *output, const char *kernel, const char *initrd, const char *grub, const char *manifest, const char *checksums)
{
    if (*checksums)
        unlink(checksums);
    if (*manifest)
        unlink(manifest);
    if (*grub)
        unlink(grub);
    if (*initrd)
        unlink(initrd);
    if (*kernel)
        unlink(kernel);
    rmdir(output);
}

int rackvm_devirtualise(const struct rackvm_config *config, const char *output)
{
    char error[512] = {0};
    if (rackvm_mkdir(output, 0755, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        return 1;
    }
    char kernel_path[RACKVM_PATH_MAX] = {0};
    char initrd_path[RACKVM_PATH_MAX] = {0};
    char grub_path[RACKVM_PATH_MAX] = {0};
    char manifest_path[RACKVM_PATH_MAX] = {0};
    char checksums_path[RACKVM_PATH_MAX] = {0};
    if (join_path(kernel_path, output, "vmlinuz", error, sizeof(error)) < 0 ||
        join_path(initrd_path, output, "initramfs", error, sizeof(error)) < 0 ||
        join_path(grub_path, output, "grub.cfg", error, sizeof(error)) < 0 ||
        join_path(manifest_path, output, "manifest.json", error, sizeof(error)) < 0 ||
        join_path(checksums_path, output, "SHA256SUMS", error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    if (rackvm_copy_file(config->kernel, kernel_path, 0644, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    if (config->initrd[0] && rackvm_copy_file(config->initrd, initrd_path, 0644, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    FILE *grub = create_text_file(grub_path, 0644, error, sizeof(error));
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
    if (finish_text_file(grub, grub_path, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    char kernel_hash[65];
    char initrd_hash[65] = {0};
    rackvm_sha256_file(kernel_path, kernel_hash, error, sizeof(error));
    if (!kernel_hash[0]) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    if (config->initrd[0]) {
        rackvm_sha256_file(initrd_path, initrd_hash, error, sizeof(error));
        if (!initrd_hash[0]) {
            fprintf(stderr, "RackVM: %s\n", error);
            goto failure;
        }
    }
    FILE *manifest = create_text_file(manifest_path, 0644, error, sizeof(error));
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
    if (finish_text_file(manifest, manifest_path, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    FILE *checksums = create_text_file(checksums_path, 0644, error, sizeof(error));
    if (!checksums) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    fprintf(checksums, "%s  vmlinuz\n", kernel_hash);
    if (config->initrd[0])
        fprintf(checksums, "%s  initramfs\n", initrd_hash);
    if (finish_text_file(checksums, checksums_path, error, sizeof(error)) < 0) {
        fprintf(stderr, "RackVM: %s\n", error);
        goto failure;
    }
    printf("Bare-metal bundle created at %s\n", output);
    printf("Copy its contents to /rackvm on a GRUB-bootable system.\n");
    return 0;

failure:
    cleanup_bundle(output, kernel_path, initrd_path, grub_path, manifest_path, checksums_path);
    return 1;
}
