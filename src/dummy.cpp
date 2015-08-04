/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "dummy.hpp"
#include "soundio.hpp"

#include <stdio.h>
#include <string.h>
#include <math.h>

static void playback_thread_run(void *arg) {
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)arg;
    SoundIoOutStream *outstream = &os->pub;
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;

    int fill_bytes = soundio_ring_buffer_fill_count(&osd->ring_buffer);
    int free_bytes = soundio_ring_buffer_capacity(&osd->ring_buffer) - fill_bytes;
    int free_frames = free_bytes / outstream->bytes_per_frame;
    osd->frames_left = free_frames;
    outstream->write_callback(outstream, free_frames);
    double start_time = soundio_os_get_time();
    long frames_consumed = 0;

    while (osd->abort_flag.test_and_set()) {
        double now = soundio_os_get_time();
        double time_passed = now - start_time;
        double next_period = start_time +
            ceil(time_passed / outstream->period_duration) * outstream->period_duration;
        double relative_time = next_period - now;
        soundio_os_cond_timed_wait(osd->cond, nullptr, relative_time);

        int fill_bytes = soundio_ring_buffer_fill_count(&osd->ring_buffer);
        int free_bytes = soundio_ring_buffer_capacity(&osd->ring_buffer) - fill_bytes;
        int fill_frames = fill_bytes / outstream->bytes_per_frame;
        int free_frames = free_bytes / outstream->bytes_per_frame;

        double total_time = soundio_os_get_time() - start_time;
        long total_frames = total_time * outstream->sample_rate;
        int frames_to_kill = total_frames - frames_consumed;
        int read_count = min(frames_to_kill, fill_frames);
        int byte_count = read_count * outstream->bytes_per_frame;
        soundio_ring_buffer_advance_read_ptr(&osd->ring_buffer, byte_count);
        frames_consumed += read_count;

        if (frames_to_kill > fill_frames) {
            outstream->underflow_callback(outstream);
            osd->frames_left = free_frames;
            outstream->write_callback(outstream, free_frames);
            frames_consumed = 0;
            start_time = soundio_os_get_time();
        } else if (free_frames > 0) {
            osd->frames_left = free_frames;
            outstream->write_callback(outstream, free_frames);
        }
    }
}

static void capture_thread_run(void *arg) {
    SoundIoInStreamPrivate *is = (SoundIoInStreamPrivate *)arg;
    SoundIoInStream *instream = &is->pub;
    SoundIoInStreamDummy *isd = &is->backend_data.dummy;

    long frames_consumed = 0;
    double start_time = soundio_os_get_time();
    while (isd->abort_flag.test_and_set()) {
        double now = soundio_os_get_time();
        double time_passed = now - start_time;
        double next_period = start_time +
            ceil(time_passed / instream->period_duration) * instream->period_duration;
        double relative_time = next_period - now;
        soundio_os_cond_timed_wait(isd->cond, nullptr, relative_time);

        int fill_bytes = soundio_ring_buffer_fill_count(&isd->ring_buffer);
        int free_bytes = soundio_ring_buffer_capacity(&isd->ring_buffer) - fill_bytes;
        int fill_frames = fill_bytes / instream->bytes_per_frame;
        int free_frames = free_bytes / instream->bytes_per_frame;

        double total_time = soundio_os_get_time() - start_time;
        long total_frames = total_time * instream->sample_rate;
        int frames_to_kill = total_frames - frames_consumed;
        int write_count = min(frames_to_kill, free_frames);
        int byte_count = write_count * instream->bytes_per_frame;
        soundio_ring_buffer_advance_write_ptr(&isd->ring_buffer, byte_count);
        frames_consumed += write_count;

        if (frames_to_kill > free_frames) {
            // TODO overflow callback
            frames_consumed = 0;
            start_time = soundio_os_get_time();
        }
        if (fill_frames > 0) {
            isd->frames_left = fill_frames;
            instream->read_callback(instream, fill_frames);
        }
    }
}

static void destroy_dummy(SoundIoPrivate *si) {
    SoundIoDummy *sid = &si->backend_data.dummy;

    if (sid->cond)
        soundio_os_cond_destroy(sid->cond);

    if (sid->mutex)
        soundio_os_mutex_destroy(sid->mutex);
}

static void flush_events(SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;
    SoundIoDummy *sid = &si->backend_data.dummy;
    if (sid->devices_emitted)
        return;
    sid->devices_emitted = true;
    soundio->on_devices_change(soundio);
}

static void wait_events(SoundIoPrivate *si) {
    SoundIoDummy *sid = &si->backend_data.dummy;
    flush_events(si);
    soundio_os_cond_wait(sid->cond, nullptr);
}

static void wakeup(SoundIoPrivate *si) {
    SoundIoDummy *sid = &si->backend_data.dummy;
    soundio_os_cond_signal(sid->cond, nullptr);
}

static void outstream_destroy_dummy(SoundIoPrivate *si, SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;

    if (osd->thread) {
        osd->abort_flag.clear();
        soundio_os_cond_signal(osd->cond, nullptr);
        soundio_os_thread_destroy(osd->thread);
        osd->thread = nullptr;
    }
    soundio_os_cond_destroy(osd->cond);
    osd->cond = nullptr;

    soundio_ring_buffer_deinit(&osd->ring_buffer);
}

static int outstream_open_dummy(SoundIoPrivate *si, SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;
    SoundIoOutStream *outstream = &os->pub;
    SoundIoDevice *device = outstream->device;

    if (outstream->buffer_duration == 0.0)
        outstream->buffer_duration = clamp(device->buffer_duration_min, 1.0, device->buffer_duration_max);
    if (outstream->period_duration == 0.0) {
        outstream->period_duration = clamp(device->period_duration_min,
                outstream->buffer_duration / 2.0, device->period_duration_max);
    }

    int err;
    int buffer_size = outstream->bytes_per_frame * outstream->sample_rate * outstream->buffer_duration;
    if ((err = soundio_ring_buffer_init(&osd->ring_buffer, buffer_size))) {
        outstream_destroy_dummy(si, os);
        return err;
    }
    int actual_capacity = soundio_ring_buffer_capacity(&osd->ring_buffer);
    osd->buffer_frame_count = actual_capacity / outstream->bytes_per_frame;
    outstream->buffer_duration = osd->buffer_frame_count / (double) outstream->sample_rate;

    osd->cond = soundio_os_cond_create();
    if (!osd->cond) {
        outstream_destroy_dummy(si, os);
        return SoundIoErrorNoMem;
    }

    return 0;
}

static int outstream_pause_dummy(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os, bool pause) {
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;
    if (pause) {
        if (osd->thread) {
            osd->abort_flag.clear();
            soundio_os_cond_signal(osd->cond, nullptr);
            soundio_os_thread_destroy(osd->thread);
            osd->thread = nullptr;
        }
    } else {
        if (!osd->thread) {
            osd->abort_flag.test_and_set();
            int err;
            if ((err = soundio_os_thread_create(playback_thread_run, os, true, &osd->thread))) {
                return err;
            }
        }
    }
    return 0;
}

static int outstream_start_dummy(SoundIoPrivate *si, SoundIoOutStreamPrivate *os) {
    return outstream_pause_dummy(si, os, false);
}

static int outstream_begin_write_dummy(SoundIoPrivate *si,
        SoundIoOutStreamPrivate *os, SoundIoChannelArea **out_areas, int *out_frame_count)
{
    SoundIoOutStream *outstream = &os->pub;
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;

    char *write_ptr = soundio_ring_buffer_write_ptr(&osd->ring_buffer);
    for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
        osd->areas[ch].ptr = write_ptr + outstream->bytes_per_sample * ch;
        osd->areas[ch].step = outstream->bytes_per_frame;
    }

    *out_frame_count = osd->frames_left;
    *out_areas = osd->areas;
    return 0;
}

static int outstream_end_write_dummy(SoundIoPrivate *si, SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;
    SoundIoOutStream *outstream = &os->pub;
    int byte_count = osd->frames_left * outstream->bytes_per_frame;
    soundio_ring_buffer_advance_write_ptr(&osd->ring_buffer, byte_count);
    osd->frames_left = 0;
    return 0;
}

static int outstream_clear_buffer_dummy(SoundIoPrivate *si, SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamDummy *osd = &os->backend_data.dummy;
    soundio_ring_buffer_clear(&osd->ring_buffer);
    return 0;
}

static void instream_destroy_dummy(SoundIoPrivate *si, SoundIoInStreamPrivate *is) {
    SoundIoInStreamDummy *isd = &is->backend_data.dummy;

    if (isd->thread) {
        isd->abort_flag.clear();
        soundio_os_cond_signal(isd->cond, nullptr);
        soundio_os_thread_destroy(isd->thread);
        isd->thread = nullptr;
    }
    soundio_os_cond_destroy(isd->cond);
    isd->cond = nullptr;

    soundio_ring_buffer_deinit(&isd->ring_buffer);
}

static int instream_open_dummy(SoundIoPrivate *si, SoundIoInStreamPrivate *is) {
    SoundIoInStreamDummy *isd = &is->backend_data.dummy;
    SoundIoInStream *instream = &is->pub;
    SoundIoDevice *device = instream->device;

    if (instream->buffer_duration == 0.0)
        instream->buffer_duration = clamp(device->buffer_duration_min, 1.0, device->buffer_duration_max);
    if (instream->period_duration == 0.0) {
        instream->period_duration = clamp(instream->device->period_duration_min,
                instream->buffer_duration / 8.0, instream->device->period_duration_max);
    }

    int err;
    int buffer_size = instream->bytes_per_frame * instream->sample_rate * instream->buffer_duration;
    if ((err = soundio_ring_buffer_init(&isd->ring_buffer, buffer_size))) {
        instream_destroy_dummy(si, is);
        return err;
    }

    int actual_capacity = soundio_ring_buffer_capacity(&isd->ring_buffer);
    isd->buffer_frame_count = actual_capacity / instream->bytes_per_frame;
    instream->buffer_duration = isd->buffer_frame_count / (double) instream->sample_rate;

    isd->cond = soundio_os_cond_create();
    if (!isd->cond) {
        instream_destroy_dummy(si, is);
        return SoundIoErrorNoMem;
    }

    return 0;
}

static int instream_pause_dummy(SoundIoPrivate *si, SoundIoInStreamPrivate *is, bool pause) {
    SoundIoInStreamDummy *isd = &is->backend_data.dummy;
    if (pause) {
        if (isd->thread) {
            isd->abort_flag.clear();
            soundio_os_cond_signal(isd->cond, nullptr);
            soundio_os_thread_destroy(isd->thread);
            isd->thread = nullptr;
        }
    } else {
        if (!isd->thread) {
            isd->abort_flag.test_and_set();
            int err;
            if ((err = soundio_os_thread_create(capture_thread_run, is, true, &isd->thread))) {
                return err;
            }
        }
    }
    return 0;
}

static int instream_start_dummy(SoundIoPrivate *si, SoundIoInStreamPrivate *is) {
    return instream_pause_dummy(si, is, false);
}

static int instream_begin_read_dummy(SoundIoPrivate *si,
        SoundIoInStreamPrivate *is, SoundIoChannelArea **out_areas, int *out_frame_count)
{
    SoundIoInStream *instream = &is->pub;
    SoundIoInStreamDummy *isd = &is->backend_data.dummy;

    char *read_ptr = soundio_ring_buffer_read_ptr(&isd->ring_buffer);
    for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
        isd->areas[ch].ptr = read_ptr + instream->bytes_per_sample * ch;
        isd->areas[ch].step = instream->bytes_per_frame;
    }

    *out_frame_count = isd->frames_left;
    *out_areas = isd->areas;

    return 0;
}

static int instream_end_read_dummy(SoundIoPrivate *si, SoundIoInStreamPrivate *is) {
    SoundIoInStreamDummy *isd = &is->backend_data.dummy;
    SoundIoInStream *instream = &is->pub;
    int byte_count = isd->frames_left * instream->bytes_per_frame;
    soundio_ring_buffer_advance_read_ptr(&isd->ring_buffer, byte_count);
    isd->frames_left = 0;
    return 0;
}

static int set_all_device_formats(SoundIoDevice *device) {
    device->format_count = 18;
    device->formats = allocate<SoundIoFormat>(device->format_count);
    if (!device->formats)
        return SoundIoErrorNoMem;

    device->formats[0] = SoundIoFormatFloat32NE;
    device->formats[1] = SoundIoFormatFloat32FE;
    device->formats[2] = SoundIoFormatS32NE;
    device->formats[3] = SoundIoFormatS32FE;
    device->formats[4] = SoundIoFormatU32NE;
    device->formats[5] = SoundIoFormatU32FE;
    device->formats[6] = SoundIoFormatS24NE;
    device->formats[7] = SoundIoFormatS24FE;
    device->formats[8] = SoundIoFormatU24NE;
    device->formats[9] = SoundIoFormatU24FE;
    device->formats[10] = SoundIoFormatFloat64NE;
    device->formats[11] = SoundIoFormatFloat64FE;
    device->formats[12] = SoundIoFormatS16NE;
    device->formats[13] = SoundIoFormatS16FE;
    device->formats[14] = SoundIoFormatU16NE;
    device->formats[15] = SoundIoFormatU16FE;
    device->formats[16] = SoundIoFormatS8;
    device->formats[17] = SoundIoFormatU8;

    return 0;
}

static int set_all_device_channel_layouts(SoundIoDevice *device) {
    device->layout_count = soundio_channel_layout_builtin_count();
    device->layouts = allocate<SoundIoChannelLayout>(device->layout_count);
    if (!device->layouts)
        return SoundIoErrorNoMem;
    for (int i = 0; i < device->layout_count; i += 1)
        device->layouts[i] = *soundio_channel_layout_get_builtin(i);
    return 0;
}

int soundio_dummy_init(SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;
    SoundIoDummy *sid = &si->backend_data.dummy;

    sid->mutex = soundio_os_mutex_create();
    if (!sid->mutex) {
        destroy_dummy(si);
        return SoundIoErrorNoMem;
    }

    sid->cond = soundio_os_cond_create();
    if (!sid->cond) {
        destroy_dummy(si);
        return SoundIoErrorNoMem;
    }

    assert(!si->safe_devices_info);
    si->safe_devices_info = create<SoundIoDevicesInfo>();
    if (!si->safe_devices_info) {
        destroy_dummy(si);
        return SoundIoErrorNoMem;
    }

    si->safe_devices_info->default_input_index = 0;
    si->safe_devices_info->default_output_index = 0;

    // create output device
    {
        SoundIoDevicePrivate *dev = create<SoundIoDevicePrivate>();
        if (!dev) {
            destroy_dummy(si);
            return SoundIoErrorNoMem;
        }
        SoundIoDevice *device = &dev->pub;

        device->ref_count = 1;
        device->soundio = soundio;
        device->id = strdup("dummy-out");
        device->name = strdup("Dummy Output Device");
        if (!device->id || !device->name) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return SoundIoErrorNoMem;
        }

        int err;
        if ((err = set_all_device_channel_layouts(device))) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return err;
        }
        if ((err = set_all_device_formats(device))) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return err;
        }

        device->buffer_duration_min = 0.01;
        device->buffer_duration_max = 4;
        device->buffer_duration_current = 0.1;
        device->sample_rate_min = 2;
        device->sample_rate_max = 5644800;
        device->sample_rate_current = 48000;
        device->period_duration_min = 0.01;
        device->period_duration_max = 2;
        device->period_duration_current = 0.05;
        device->aim = SoundIoDeviceAimOutput;

        if (si->safe_devices_info->output_devices.append(device)) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return SoundIoErrorNoMem;
        }
    }

    // create input device
    {
        SoundIoDevicePrivate *dev = create<SoundIoDevicePrivate>();
        if (!dev) {
            destroy_dummy(si);
            return SoundIoErrorNoMem;
        }
        SoundIoDevice *device = &dev->pub;

        device->ref_count = 1;
        device->soundio = soundio;
        device->id = strdup("dummy-in");
        device->name = strdup("Dummy Input Device");
        if (!device->id || !device->name) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return SoundIoErrorNoMem;
        }

        int err;
        if ((err = set_all_device_channel_layouts(device))) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return err;
        }

        if ((err = set_all_device_formats(device))) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return err;
        }
        device->buffer_duration_min = 0.01;
        device->buffer_duration_max = 4;
        device->buffer_duration_current = 0.1;
        device->sample_rate_min = 2;
        device->sample_rate_max = 5644800;
        device->sample_rate_current = 48000;
        device->period_duration_min = 0.01;
        device->period_duration_max = 2;
        device->period_duration_current = 0.05;
        device->aim = SoundIoDeviceAimInput;

        if (si->safe_devices_info->input_devices.append(device)) {
            soundio_device_unref(device);
            destroy_dummy(si);
            return SoundIoErrorNoMem;
        }
    }


    si->destroy = destroy_dummy;
    si->flush_events = flush_events;
    si->wait_events = wait_events;
    si->wakeup = wakeup;

    si->outstream_open = outstream_open_dummy;
    si->outstream_destroy = outstream_destroy_dummy;
    si->outstream_start = outstream_start_dummy;
    si->outstream_begin_write = outstream_begin_write_dummy;
    si->outstream_end_write = outstream_end_write_dummy;
    si->outstream_clear_buffer = outstream_clear_buffer_dummy;
    si->outstream_pause = outstream_pause_dummy;

    si->instream_open = instream_open_dummy;
    si->instream_destroy = instream_destroy_dummy;
    si->instream_start = instream_start_dummy;
    si->instream_begin_read = instream_begin_read_dummy;
    si->instream_end_read = instream_end_read_dummy;
    si->instream_pause = instream_pause_dummy;

    return 0;
}
