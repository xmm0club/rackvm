#define _GNU_SOURCE
#include "rackvm.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
        text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return text;
}

static int decode_value(const char *input, char *output, size_t size, char *error, size_t error_size)
{
    size_t used = 0;
    char quote = 0;
    if (*input == '\'' || *input == '"')
        quote = *input++;
    while (*input) {
        if (quote && *input == quote) {
            input++;
            if (*trim((char *)input) != '\0') {
                rackvm_set_error(error, error_size, "characters after closing quote");
                return -1;
            }
            if (used >= size)
                return -1;
            output[used] = '\0';
            return 0;
        }
        char value = *input++;
        if (value == '\\' && *input) {
            value = *input++;
            if (value == 'n')
                value = '\n';
            else if (value == 't')
                value = '\t';
        }
        if (used + 1 >= size) {
            rackvm_set_error(error, error_size, "value is too long");
            return -1;
        }
        output[used++] = value;
    }
    if (quote) {
        rackvm_set_error(error, error_size, "unterminated quoted value");
        return -1;
    }
    while (used > 0 && isspace((unsigned char)output[used - 1]))
        used--;
    output[used] = '\0';
    return 0;
}

static int parse_u64(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *result)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *trim(end) || value < minimum || value > maximum)
        return -1;
    *result = value;
    return 0;
}

static int parse_bool(const char *text, bool *result)
{
    if (!strcasecmp(text, "true") || !strcasecmp(text, "yes") || !strcmp(text, "1")) {
        *result = true;
        return 0;
    }
    if (!strcasecmp(text, "false") || !strcasecmp(text, "no") || !strcmp(text, "0")) {
        *result = false;
        return 0;
    }
    return -1;
}

void rackvm_config_defaults(struct rackvm_config *config)
{
    memset(config, 0, sizeof(*config));
    snprintf(config->name, sizeof(config->name), "rackvm");
    snprintf(config->cmdline, sizeof(config->cmdline), "console=ttyS0,115200n8 earlyprintk=serial,ttyS0,115200 panic=-1 reboot=k");
    config->memory_mib = 512;
    config->cpus = 1;
    config->interactive = true;
}

int rackvm_config_load(const char *path, struct rackvm_config *config, char *error, size_t error_size)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    unsigned long number = 0;
    int status = 0;
    while (getline(&line, &capacity, file) >= 0) {
        number++;
        char *current = trim(line);
        if (!*current || *current == '#')
            continue;
        char *equals = strchr(current, '=');
        if (!equals) {
            rackvm_set_error(error, error_size, "%s:%lu: expected key=value", path, number);
            status = -1;
            break;
        }
        *equals = '\0';
        char *key = trim(current);
        char *value = trim(equals + 1);
        char decoded[RACKVM_CMDLINE_MAX];
        char detail[256] = {0};
        if (decode_value(value, decoded, sizeof(decoded), detail, sizeof(detail)) < 0) {
            rackvm_set_error(error, error_size, "%s:%lu: %s", path, number, detail[0] ? detail : "invalid value");
            status = -1;
            break;
        }
        if (!strcmp(key, "name")) {
            if (strlen(decoded) >= sizeof(config->name))
                status = -1;
            else
                memcpy(config->name, decoded, strlen(decoded) + 1);
        } else if (!strcmp(key, "kernel")) {
            if (strlen(decoded) >= sizeof(config->kernel))
                status = -1;
            else
                snprintf(config->kernel, sizeof(config->kernel), "%s", decoded);
        } else if (!strcmp(key, "initrd")) {
            if (strlen(decoded) >= sizeof(config->initrd))
                status = -1;
            else
                snprintf(config->initrd, sizeof(config->initrd), "%s", decoded);
        } else if (!strcmp(key, "disk")) {
            if (strlen(decoded) >= sizeof(config->disk))
                status = -1;
            else
                snprintf(config->disk, sizeof(config->disk), "%s", decoded);
        } else if (!strcmp(key, "cmdline")) {
            if (strlen(decoded) >= sizeof(config->cmdline))
                status = -1;
            else
                snprintf(config->cmdline, sizeof(config->cmdline), "%s", decoded);
        } else if (!strcmp(key, "memory")) {
            status = parse_u64(decoded, 64, 65536, &config->memory_mib);
        } else if (!strcmp(key, "cpus")) {
            uint64_t cpus = 0;
            status = parse_u64(decoded, 1, RACKVM_MAX_VCPUS, &cpus);
            config->cpus = (unsigned int)cpus;
        } else if (!strcmp(key, "interactive")) {
            status = parse_bool(decoded, &config->interactive);
        } else {
            rackvm_set_error(error, error_size, "%s:%lu: unknown key '%s'", path, number, key);
            status = -1;
            break;
        }
        if (status < 0) {
            rackvm_set_error(error, error_size, "%s:%lu: invalid value for '%s'", path, number, key);
            break;
        }
    }
    if (ferror(file) && status == 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        status = -1;
    }
    free(line);
    fclose(file);
    return status;
}

static int resolve_one(char path[RACKVM_PATH_MAX], const char *directory, char *error, size_t error_size)
{
    if (!path[0] || path[0] == '/')
        return 0;
    char combined[RACKVM_PATH_MAX];
    int length = snprintf(combined, sizeof(combined), "%s/%s", directory, path);
    if (length < 0 || (size_t)length >= sizeof(combined)) {
        rackvm_set_error(error, error_size, "resolved path is too long");
        return -1;
    }
    snprintf(path, RACKVM_PATH_MAX, "%s", combined);
    return 0;
}

int rackvm_config_resolve_paths(struct rackvm_config *config, const char *config_path, char *error, size_t error_size)
{
    char absolute[RACKVM_PATH_MAX];
    if (!realpath(config_path, absolute)) {
        rackvm_set_error(error, error_size, "%s: %s", config_path, strerror(errno));
        return -1;
    }
    char *slash = strrchr(absolute, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    if (resolve_one(config->kernel, absolute, error, error_size) < 0)
        return -1;
    if (resolve_one(config->initrd, absolute, error, error_size) < 0)
        return -1;
    return resolve_one(config->disk, absolute, error, error_size);
}

int rackvm_config_validate(const struct rackvm_config *config, char *error, size_t error_size)
{
    if (!config->name[0]) {
        rackvm_set_error(error, error_size, "name cannot be empty");
        return -1;
    }
    for (const unsigned char *current = (const unsigned char *)config->name; *current; current++) {
        if (*current < 0x20 || *current == 0x7f) {
            rackvm_set_error(error, error_size, "name contains a control character");
            return -1;
        }
    }
    if (!config->kernel[0]) {
        rackvm_set_error(error, error_size, "kernel is required");
        return -1;
    }
    if (access(config->kernel, R_OK) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", config->kernel, strerror(errno));
        return -1;
    }
    if (config->initrd[0] && access(config->initrd, R_OK) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", config->initrd, strerror(errno));
        return -1;
    }
    if (config->disk[0] && access(config->disk, R_OK | W_OK) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", config->disk, strerror(errno));
        return -1;
    }
    if (config->memory_mib < 64 || config->memory_mib > 65536) {
        rackvm_set_error(error, error_size, "memory must be between 64 and 65536 MiB");
        return -1;
    }
    if (config->cpus < 1 || config->cpus > RACKVM_MAX_VCPUS) {
        rackvm_set_error(error, error_size, "cpus must be between 1 and %u", RACKVM_MAX_VCPUS);
        return -1;
    }
    if (strlen(config->cmdline) >= RACKVM_CMDLINE_MAX - 1) {
        rackvm_set_error(error, error_size, "kernel command line is too long");
        return -1;
    }
    for (const unsigned char *current = (const unsigned char *)config->cmdline; *current; current++) {
        if (*current < 0x20 || *current == 0x7f) {
            rackvm_set_error(error, error_size, "kernel command line contains a control character");
            return -1;
        }
    }
    return 0;
}
