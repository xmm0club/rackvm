#define _GNU_SOURCE
#include "rackvm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct sha256_context {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
};

static const uint32_t sha256_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotate_right(uint32_t value, unsigned int amount)
{
    return (value >> amount) | (value << (32 - amount));
}

static void sha256_transform(struct sha256_context *context, const uint8_t block[64])
{
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++)
        words[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (size_t i = 16; i < 64; i++) {
        uint32_t a = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t b = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + a + words[i - 7] + b;
    }
    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t first = h + s1 + choice + sha256_constants[i] + words[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(struct sha256_context *context)
{
    const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(context->state, initial, sizeof(initial));
    context->bits = 0;
    context->used = 0;
}

static void sha256_update(struct sha256_context *context, const uint8_t *data, size_t size)
{
    context->bits += (uint64_t)size * 8;
    while (size) {
        size_t available = sizeof(context->block) - context->used;
        size_t take = size < available ? size : available;
        memcpy(context->block + context->used, data, take);
        context->used += take;
        data += take;
        size -= take;
        if (context->used == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->used = 0;
        }
    }
}

static void sha256_finish(struct sha256_context *context, uint8_t digest[32])
{
    context->block[context->used++] = 0x80;
    if (context->used > 56) {
        memset(context->block + context->used, 0, 64 - context->used);
        sha256_transform(context, context->block);
        context->used = 0;
    }
    memset(context->block + context->used, 0, 56 - context->used);
    for (size_t i = 0; i < 8; i++)
        context->block[56 + i] = (uint8_t)(context->bits >> (56 - i * 8));
    sha256_transform(context, context->block);
    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(context->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(context->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(context->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)context->state[i];
    }
}

void rackvm_set_error(char *buffer, size_t size, const char *format, ...)
{
    if (!buffer || !size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
}

int rackvm_read_file(const char *path, uint8_t **data, size_t *size, char *error, size_t error_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0 || statbuf.st_size < 0 || (uintmax_t)statbuf.st_size > SIZE_MAX) {
        rackvm_set_error(error, error_size, "%s: cannot determine file size", path);
        close(fd);
        return -1;
    }
    uint8_t *buffer = malloc((size_t)statbuf.st_size ? (size_t)statbuf.st_size : 1);
    if (!buffer) {
        rackvm_set_error(error, error_size, "Out of memory while reading %s", path);
        close(fd);
        return -1;
    }
    size_t offset = 0;
    while (offset < (size_t)statbuf.st_size) {
        ssize_t count = read(fd, buffer + offset, (size_t)statbuf.st_size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            rackvm_set_error(error, error_size, "%s: %s", path, count < 0 ? strerror(errno) : "unexpected end of file");
            free(buffer);
            close(fd);
            return -1;
        }
        offset += (size_t)count;
    }
    close(fd);
    *data = buffer;
    *size = offset;
    return 0;
}

int rackvm_copy_file(const char *source, const char *destination, mode_t mode, char *error, size_t error_size)
{
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        rackvm_set_error(error, error_size, "%s: %s", source, strerror(errno));
        return -1;
    }
    int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (output < 0) {
        rackvm_set_error(error, error_size, "%s: %s", destination, strerror(errno));
        close(input);
        return -1;
    }
    uint8_t buffer[65536];
    int status = 0;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            rackvm_set_error(error, error_size, "%s: %s", source, strerror(errno));
            status = -1;
            break;
        }
        if (!count)
            break;
        size_t written = 0;
        while (written < (size_t)count) {
            ssize_t result = write(output, buffer + written, (size_t)count - written);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0) {
                rackvm_set_error(error, error_size, "%s: %s", destination, strerror(errno));
                status = -1;
                break;
            }
            written += (size_t)result;
        }
        if (status < 0)
            break;
    }
    if (status == 0 && fsync(output) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", destination, strerror(errno));
        status = -1;
    }
    close(input);
    close(output);
    if (status < 0)
        unlink(destination);
    return status;
}

int rackvm_mkdir(const char *path, mode_t mode, char *error, size_t error_size)
{
    if (mkdir(path, mode) < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

const char *rackvm_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void rackvm_sha256_file(const char *path, char output[65], char *error, size_t error_size)
{
    output[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
        return;
    }
    struct sha256_context context;
    sha256_init(&context);
    uint8_t buffer[65536];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            rackvm_set_error(error, error_size, "%s: %s", path, strerror(errno));
            close(fd);
            return;
        }
        if (!count)
            break;
        sha256_update(&context, buffer, (size_t)count);
    }
    close(fd);
    uint8_t digest[32];
    sha256_finish(&context, digest);
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(output + i * 2, 3, "%02x", digest[i]);
}
