#define _GNU_SOURCE // memfd_create() is a glibc/Linux-only extension, not exposed by
                     // -std=gnu11 alone -- needs this feature-test macro defined
                     // before any header is pulled in.
#include "wayland/shm_buffer.h"

#include <cairo.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void buffer_release(void *data, struct wl_buffer *buffer) {
    // The compositor is done reading this buffer (it's no longer the currently
    // displayed content, or was never actually shown), so it's safe to draw into
    // again. Matched up by pointer identity since there are only ever two buffers.
    ShmBuffers *sb = data;
    if (buffer == sb->buffer[0]) {
        sb->busy[0] = false;
    } else if (buffer == sb->buffer[1]) {
        sb->busy[1] = false;
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void destroy_buffers_only(ShmBuffers *sb) {
    for (int i = 0; i < 2; i++) {
        if (sb->buffer[i]) {
            wl_buffer_destroy(sb->buffer[i]);
            sb->buffer[i] = NULL;
        }
    }
    if (sb->data) {
        munmap(sb->data, (size_t)sb->stride * (size_t)sb->height * 2);
        sb->data = NULL;
    }
    if (sb->fd >= 0) {
        close(sb->fd);
        sb->fd = -1;
    }
}

bool shm_buffers_resize(ShmBuffers *sb, struct wl_shm *shm, int width, int height) {
    destroy_buffers_only(sb);

    // cairo's stride includes whatever row padding the pixel format needs; using
    // its own computation (rather than assuming width*4) keeps the buffer layout
    // consistent with what cairo_image_surface_create_for_data() expects later when
    // rendering into this same memory.
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    size_t buffer_size = (size_t)stride * (size_t)height;
    size_t pool_size = buffer_size * 2; // two buffers, back to back in one pool

    int fd = memfd_create("gridBar-shm", MFD_CLOEXEC);
    if (fd < 0) {
        perror("gridBar: memfd_create");
        return false;
    }
    if (ftruncate(fd, (off_t)pool_size) < 0) {
        perror("gridBar: ftruncate");
        close(fd);
        return false;
    }

    uint8_t *data = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("gridBar: mmap");
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)pool_size);
    struct wl_buffer *buf0 = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    struct wl_buffer *buf1 = wl_shm_pool_create_buffer(pool, (int32_t)buffer_size, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    // The pool object itself is only needed to carve out the two buffers above; once
    // that's done each wl_buffer is independent and the pool can be destroyed
    // immediately (this does not affect the memfd or the buffers already created).
    wl_shm_pool_destroy(pool);

    wl_buffer_add_listener(buf0, &buffer_listener, sb);
    wl_buffer_add_listener(buf1, &buffer_listener, sb);

    sb->fd = fd;
    sb->data = data;
    sb->width = width;
    sb->height = height;
    sb->stride = stride;
    sb->buffer[0] = buf0;
    sb->buffer[1] = buf1;
    sb->busy[0] = false;
    sb->busy[1] = false;
    sb->front = 0;

    return true;
}

struct wl_buffer *shm_buffers_acquire(ShmBuffers *sb, uint8_t **out_data) {
    // Prefer the buffer that wasn't drawn into last time (simple ping-pong); if
    // that one happens to still be busy, fall back to checking the other rather
    // than giving up immediately.
    int try_order[2] = { sb->front, 1 - sb->front };
    for (int i = 0; i < 2; i++) {
        int idx = try_order[i];
        if (!sb->busy[idx]) {
            sb->busy[idx] = true;
            sb->front = 1 - idx; // next acquire should prefer the other buffer
            size_t buffer_size = (size_t)sb->stride * (size_t)sb->height;
            *out_data = sb->data + (idx == 0 ? 0 : buffer_size);
            return sb->buffer[idx];
        }
    }
    return NULL; // both buffers still held by the compositor; caller should skip this redraw
}

void shm_buffers_destroy(ShmBuffers *sb) {
    destroy_buffers_only(sb);
    memset(sb, 0, sizeof(*sb));
}
