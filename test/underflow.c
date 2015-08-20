/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <soundio/soundio.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

__attribute__ ((cold))
__attribute__ ((noreturn))
__attribute__ ((format (printf, 1, 2)))
static void panic(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

static int usage(char *exe) {
    fprintf(stderr, "Usage: %s [--dummy] [--alsa] [--pulseaudio] [--jack]\n", exe);
    return 1;
}

static const float PI = 3.1415926535f;
static float seconds_offset = 0.0f;
static bool caused_underflow = false;
static struct SoundIo *soundio = NULL;
static float seconds_end = 9.0f;

static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max) {
    float float_sample_rate = outstream->sample_rate;
    float seconds_per_frame = 1.0f / float_sample_rate;
    struct SoundIoChannelArea *areas;
    int err;

    if (!caused_underflow && seconds_offset >= 3.0f) {
        caused_underflow = true;
        sleep(3);
    }

    if (seconds_offset >= seconds_end) {
        soundio_wakeup(soundio);
        return;
    }

    int frames_left = frame_count_max;

    for (;;) {
        int frame_count = frames_left;
        if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count)))
            panic("%s", soundio_strerror(err));

        if (!frame_count)
            break;

        const struct SoundIoChannelLayout *layout = &outstream->layout;

        float pitch = 440.0f;
        float radians_per_second = pitch * 2.0f * PI;
        for (int frame = 0; frame < frame_count; frame += 1) {
            float sample = sinf((seconds_offset + frame * seconds_per_frame) * radians_per_second);
            for (int channel = 0; channel < layout->channel_count; channel += 1) {
                float *ptr = (float*)(areas[channel].ptr + areas[channel].step * frame);
                *ptr = sample;
            }
        }
        seconds_offset += seconds_per_frame * frame_count;

        if ((err = soundio_outstream_end_write(outstream))) {
            if (err == SoundIoErrorUnderflow)
                return;
            panic("%s", soundio_strerror(err));
        }

        frames_left -= frame_count;
        if (frames_left <= 0)
            break;
    }
}

static void underflow_callback(struct SoundIoOutStream *outstream) {
    static int count = 0;
    fprintf(stderr, "underflow %d\n", count++);
}

int main(int argc, char **argv) {
    char *exe = argv[0];
    enum SoundIoBackend backend = SoundIoBackendNone;
    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (strcmp("--dummy", arg) == 0) {
            backend = SoundIoBackendDummy;
        } else if (strcmp("--alsa", arg) == 0) {
            backend = SoundIoBackendAlsa;
        } else if (strcmp("--pulseaudio", arg) == 0) {
            backend = SoundIoBackendPulseAudio;
        } else if (strcmp("--jack", arg) == 0) {
            backend = SoundIoBackendJack;
        } else {
            return usage(exe);
        }
    }

    fprintf(stderr, "You should hear a sine wave for 3 seconds, then some period of silence or glitches,\n"
                    "then you should see at least one buffer underflow message, then hear a sine\n"
                    "wave for 3 seconds, then the program should exit successfully.\n");

    if (!(soundio = soundio_create()))
        panic("out of memory");

    int err = (backend == SoundIoBackendNone) ?
        soundio_connect(soundio) : soundio_connect_backend(soundio, backend);

    if (err)
        panic("error connecting: %s", soundio_strerror(err));

    soundio_flush_events(soundio);

    int default_out_device_index = soundio_default_output_device_index(soundio);
    if (default_out_device_index < 0)
        panic("no output device found");

    struct SoundIoDevice *device = soundio_get_output_device(soundio, default_out_device_index);
    if (!device)
        panic("out of memory");

    fprintf(stderr, "Output device: %s\n", device->name);

    struct SoundIoOutStream *outstream = soundio_outstream_create(device);
    outstream->format = SoundIoFormatFloat32NE;
    outstream->write_callback = write_callback;
    outstream->underflow_callback = underflow_callback;

    if ((err = soundio_outstream_open(outstream)))
        panic("unable to open device: %s", soundio_strerror(err));

    if (outstream->layout_error)
        fprintf(stderr, "unable to set channel layout: %s\n", soundio_strerror(outstream->layout_error));

    if ((err = soundio_outstream_start(outstream)))
        panic("unable to start device: %s", soundio_strerror(err));

    while (seconds_offset < seconds_end)
        soundio_wait_events(soundio);

    soundio_outstream_destroy(outstream);
    soundio_device_unref(device);
    soundio_destroy(soundio);
    return 0;
}
