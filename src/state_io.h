#ifndef GP32EMU_STATE_IO_H
#define GP32EMU_STATE_IO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* One byte stream for the existing file layout, caller-owned memory and size
 * counting. Failed operations are sticky and never copy a partial memory span. */
typedef enum state_io_mode {
    STATE_IO_FILE, STATE_IO_READ, STATE_IO_WRITE, STATE_IO_COUNT
} state_io_mode_t;

typedef struct state_io {
    state_io_mode_t mode;
    FILE *file;
    const uint8_t *input;
    uint8_t *output;
    size_t size, pos;
    int failed;
} state_io_t;

static inline state_io_t state_io_file(FILE *file) {
    state_io_t io = {0};
    io.mode = STATE_IO_FILE;
    io.file = file;
    io.failed = file == NULL;
    return io;
}

static inline state_io_t state_io_reader(const void *data, size_t size) {
    state_io_t io = {0};
    io.mode = STATE_IO_READ;
    io.input = (const uint8_t *)data;
    io.size = size;
    io.failed = data == NULL;
    return io;
}

static inline state_io_t state_io_writer(void *data, size_t size) {
    state_io_t io = {0};
    io.mode = STATE_IO_WRITE;
    io.output = (uint8_t *)data;
    io.size = size;
    io.failed = data == NULL;
    return io;
}

static inline state_io_t state_io_counter(void) {
    state_io_t io = {0};
    io.mode = STATE_IO_COUNT;
    return io;
}

static inline int state_io_fail(state_io_t *io) {
    if (io) io->failed = 1;
    return 0;
}

static inline int state_io_write(state_io_t *io, const void *data, size_t bytes) {
    if (!io || io->failed || bytes > SIZE_MAX - io->pos) return state_io_fail(io);
    if (bytes && !data) return state_io_fail(io);
    if (io->mode == STATE_IO_WRITE) {
        if (bytes > io->size - io->pos) return state_io_fail(io);
        if (bytes) memcpy(io->output + io->pos, data, bytes);
    } else if (io->mode == STATE_IO_FILE) {
        if (bytes && fwrite(data, 1, bytes, io->file) != bytes) return state_io_fail(io);
    } else if (io->mode != STATE_IO_COUNT) {
        return state_io_fail(io);
    }
    io->pos += bytes;
    return 1;
}

static inline int state_io_read(state_io_t *io, void *data, size_t bytes) {
    if (!io || io->failed || bytes > SIZE_MAX - io->pos || (bytes && !data)) return state_io_fail(io);
    if (io->mode == STATE_IO_READ) {
        if (bytes > io->size - io->pos) return state_io_fail(io);
        if (bytes) memcpy(data, io->input + io->pos, bytes);
    } else if (io->mode == STATE_IO_FILE) {
        if (bytes && fread(data, 1, bytes, io->file) != bytes) return state_io_fail(io);
    } else {
        return state_io_fail(io);
    }
    io->pos += bytes;
    return 1;
}

/* Advance a reader past bytes that carry no data. The v0014 SmartMedia section
 * pads its entry region to the session budget, so the loader skips the tail
 * instead of copying megabytes of zeros. Only reading modes are accepted: a
 * writer that pads must write real zero bytes. */
static inline int state_io_skip(state_io_t *io, size_t bytes) {
    if (!io || io->failed || bytes > SIZE_MAX - io->pos) return state_io_fail(io);
    if (io->mode == STATE_IO_READ) {
        if (bytes > io->size - io->pos) return state_io_fail(io);
    } else if (io->mode == STATE_IO_FILE) {
        if (bytes && fseek(io->file, (long)bytes, SEEK_CUR) != 0) return state_io_fail(io);
    } else {
        return state_io_fail(io);
    }
    io->pos += bytes;
    return 1;
}

#endif /* GP32EMU_STATE_IO_H */
