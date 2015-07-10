/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <soundio.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// list or keep a watch on audio devices

static int usage(char *exe) {
    fprintf(stderr, "Usage: %s [--watch]\n", exe);
    return 1;
}

static void print_channel_layout(const struct SoundIoChannelLayout *layout) {
    if (layout->name) {
        fprintf(stderr, "%s", layout->name);
    } else {
        fprintf(stderr, "%s", soundio_get_channel_name(layout->channels[0]));
        for (int i = 1; i < layout->channel_count; i += 1) {
            fprintf(stderr, ", %s", soundio_get_channel_name(layout->channels[i]));
        }
    }

}

static void print_device(struct SoundIoDevice *device, bool is_default) {
    const char *default_str = is_default ? " (default)" : "";
    const char *raw_str = device->is_raw ? " (raw)" : "";
    fprintf(stderr, "%s%s%s\n", device->description, default_str, raw_str);
    fprintf(stderr, "  name: %s\n", device->name);

    if (device->probe_error) {
        fprintf(stderr, "  probe error: %s\n", soundio_strerror(device->probe_error));
    } else {
        fprintf(stderr, "  channel layout: ");
        print_channel_layout(&device->channel_layout);
        fprintf(stderr, "\n");
        fprintf(stderr, "  min sample rate: %d\n", device->sample_rate_min);
        fprintf(stderr, "  max sample rate: %d\n", device->sample_rate_max);
        if (device->sample_rate_current)
            fprintf(stderr, "  current sample rate: %d\n", device->sample_rate_current);
        fprintf(stderr, "  formats: ");
        for (int i = 0; i < device->format_count; i += 1) {
            const char *comma = (i == device->format_count - 1) ? "" : ", ";
            fprintf(stderr, "%s%s", soundio_format_string(device->formats[i]), comma);
        }
        fprintf(stderr, "\n");
        if (device->current_format != SoundIoFormatInvalid)
            fprintf(stderr, "  current format: %s\n", soundio_format_string(device->current_format));
    }
    fprintf(stderr, "\n");
}

static int list_devices(struct SoundIo *soundio) {
    int output_count = soundio_get_output_device_count(soundio);
    int input_count = soundio_get_input_device_count(soundio);

    int default_output = soundio_get_default_output_device_index(soundio);
    int default_input = soundio_get_default_input_device_index(soundio);

    fprintf(stderr, "--------Input Devices--------\n\n");
    for (int i = 0; i < input_count; i += 1) {
        struct SoundIoDevice *device = soundio_get_input_device(soundio, i);
        print_device(device, default_input == i);
        soundio_device_unref(device);
    }
    fprintf(stderr, "\n--------Output Devices--------\n\n");
    for (int i = 0; i < output_count; i += 1) {
        struct SoundIoDevice *device = soundio_get_output_device(soundio, i);
        print_device(device, default_output == i);
        soundio_device_unref(device);
    }

    fprintf(stderr, "\n%d devices found\n", input_count + output_count);
    return 0;
}

static void on_devices_change(struct SoundIo *soundio) {
    fprintf(stderr, "devices changed\n");
    list_devices(soundio);
}

int main(int argc, char **argv) {
    char *exe = argv[0];
    bool watch = false;

    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (strcmp("--watch", arg) == 0) {
            watch = true;
        } else {
            return usage(exe);
        }
    }

    struct SoundIo *soundio = soundio_create();
    if (!soundio) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    int err;
    if ((err = soundio_connect(soundio))) {
        fprintf(stderr, "%s\n", soundio_strerror(err));
        return err;
    }

    if (watch) {
        soundio->on_devices_change = on_devices_change;
        for (;;) {
            soundio_wait_events(soundio);
        }
    } else {
        int err = list_devices(soundio);
        soundio_destroy(soundio);
        return err;
    }
}
