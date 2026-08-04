#ifndef CUSTOMBAR_WAYLAND_SHM_BUFFER_H
#define CUSTOMBAR_WAYLAND_SHM_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

// A double-buffered wl_shm allocation for one output's bar surface. Two same-sized
// buffers share a single memfd-backed pool; redraws ping-pong between them so we're
// never writing into a buffer the compositor might still be reading from a
// previously-attached frame.
typedef struct {
    int fd;                    // memfd backing the whole pool (both buffers)
    uint8_t *data;              // mmap'd base of the pool
    int width, height, stride;  // stride is bytes per row (cairo's, includes padding)
    struct wl_buffer *buffer[2];
    bool busy[2];                // true from attach until the compositor's wl_buffer.release
    int front;                   // index of the buffer to draw into on the next redraw
} ShmBuffers;

// (Re)allocates the pool/buffers for the given pixel size. Safe to call again with a
// different size later (e.g. output reconfigure) -- any previous allocation is torn
// down first. Returns false on failure (sb is left zeroed).
// `*sb` must be zero-initialized (e.g. `ShmBuffers sb = {0};`) before the first call.
bool shm_buffers_resize(ShmBuffers *sb, struct wl_shm *shm, int width, int height);

// Returns the buffer currently safe to draw into (not `busy`) and its mmap'd pixel
// data via *out_data, or NULL if both buffers are still held by the compositor. The
// latter should be rare given frame-callback throttling -- callers should just skip
// this redraw and let the next dirty tick retry. The returned buffer is immediately
// marked busy (the caller is expected to attach+commit it) and stays that way until
// the compositor's wl_buffer.release event fires for it.
struct wl_buffer *shm_buffers_acquire(ShmBuffers *sb, uint8_t **out_data);

void shm_buffers_destroy(ShmBuffers *sb);

#endif
