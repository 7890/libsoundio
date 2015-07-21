/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "ring_buffer.hpp"
#include "soundio.hpp"
#include "util.hpp"
#include "os.hpp"

#include <stdlib.h>

struct SoundIoRingBuffer *soundio_ring_buffer_create(struct SoundIo *soundio, int requested_capacity) {
    SoundIoRingBuffer *rb = create<SoundIoRingBuffer>();

    if (!rb) {
        soundio_ring_buffer_destroy(rb);
        return nullptr;
    }

    if (soundio_ring_buffer_init(rb, requested_capacity)) {
        soundio_ring_buffer_destroy(rb);
        return nullptr;
    }

    return rb;
}

void soundio_ring_buffer_destroy(struct SoundIoRingBuffer *rb) {
    if (!rb)
        return;

    soundio_ring_buffer_deinit(rb);

    destroy(rb);
}

int soundio_ring_buffer_capacity(struct SoundIoRingBuffer *rb) {
    return rb->capacity;
}

char *soundio_ring_buffer_write_ptr(struct SoundIoRingBuffer *rb) {
    return rb->address + (rb->write_offset % rb->capacity);
}

void soundio_ring_buffer_advance_write_ptr(struct SoundIoRingBuffer *rb, int count) {
    rb->write_offset += count;
    assert(soundio_ring_buffer_fill_count(rb) >= 0);
}

char *soundio_ring_buffer_read_ptr(struct SoundIoRingBuffer *rb) {
    return rb->address + (rb->read_offset % rb->capacity);
}

void soundio_ring_buffer_advance_read_ptr(struct SoundIoRingBuffer *rb, int count) {
    rb->read_offset += count;
    assert(soundio_ring_buffer_fill_count(rb) >= 0);
}

int soundio_ring_buffer_fill_count(struct SoundIoRingBuffer *rb) {
    int count = rb->write_offset - rb->read_offset;
    assert(count >= 0);
    assert(count <= rb->capacity);
    return count;
}

int soundio_ring_buffer_free_count(struct SoundIoRingBuffer *rb) {
    return rb->capacity - soundio_ring_buffer_fill_count(rb);
}

void soundio_ring_buffer_clear(struct SoundIoRingBuffer *rb) {
    return rb->write_offset.store(rb->read_offset.load());
}

int soundio_ring_buffer_init(struct SoundIoRingBuffer *rb, int requested_capacity) {
    rb->mem = soundio_os_create_mirrored_memory(requested_capacity);
    if (!rb->mem) {
        soundio_ring_buffer_deinit(rb);
        return SoundIoErrorNoMem;
    }
    rb->address = rb->mem->address;
    rb->capacity = rb->mem->capacity;
    rb->write_offset = 0;
    rb->read_offset = 0;

    return 0;
}

void soundio_ring_buffer_deinit(struct SoundIoRingBuffer *rb) {
    soundio_os_destroy_mirrored_memory(rb->mem);
    rb->mem = nullptr;
}
