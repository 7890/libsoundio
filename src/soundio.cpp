/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "soundio.hpp"
#include "util.hpp"
#include "dummy.hpp"
#include "os.hpp"
#include "config.h"

#ifdef SOUNDIO_HAVE_PULSEAUDIO
#include "pulseaudio.hpp"
#endif

#ifdef SOUNDIO_HAVE_ALSA
#include "alsa.hpp"
#endif

#include <string.h>
#include <assert.h>

static const SoundIoBackend available_backends[] = {
#ifdef SOUNDIO_HAVE_PULSEAUDIO
    SoundIoBackendPulseAudio,
#endif
#ifdef SOUNDIO_HAVE_ALSA
    SoundIoBackendAlsa,
#endif
    SoundIoBackendDummy,
};

const char *soundio_strerror(int error) {
    switch ((enum SoundIoError)error) {
        case SoundIoErrorNone: return "(no error)";
        case SoundIoErrorNoMem: return "out of memory";
        case SoundIoErrorInitAudioBackend: return "unable to initialize audio backend";
        case SoundIoErrorSystemResources: return "system resource not available";
        case SoundIoErrorOpeningDevice: return "unable to open device";
        case SoundIoErrorInvalid: return "invalid value";
        case SoundIoErrorBackendUnavailable: return "backend unavailable";
    }
    soundio_panic("invalid error enum value: %d", error);
}

int soundio_get_bytes_per_sample(enum SoundIoFormat format) {
    switch (format) {
    case SoundIoFormatU8:         return 1;
    case SoundIoFormatS8:         return 1;
    case SoundIoFormatS16LE:      return 2;
    case SoundIoFormatS16BE:      return 2;
    case SoundIoFormatU16LE:      return 2;
    case SoundIoFormatU16BE:      return 2;
    case SoundIoFormatS24LE:      return 4;
    case SoundIoFormatS24BE:      return 4;
    case SoundIoFormatU24LE:      return 4;
    case SoundIoFormatU24BE:      return 4;
    case SoundIoFormatS32LE:      return 4;
    case SoundIoFormatS32BE:      return 4;
    case SoundIoFormatU32LE:      return 4;
    case SoundIoFormatU32BE:      return 4;
    case SoundIoFormatFloat32LE:  return 4;
    case SoundIoFormatFloat32BE:  return 4;
    case SoundIoFormatFloat64LE:  return 8;
    case SoundIoFormatFloat64BE:  return 8;

    case SoundIoFormatInvalid:
        soundio_panic("invalid sample format");
    }
    soundio_panic("invalid sample format");
}

const char * soundio_format_string(enum SoundIoFormat format) {
    switch (format) {
    case SoundIoFormatU8:         return "signed 8-bit";
    case SoundIoFormatS8:         return "unsigned 8-bit";
    case SoundIoFormatS16LE:      return "signed 16-bit LE";
    case SoundIoFormatS16BE:      return "signed 16-bit BE";
    case SoundIoFormatU16LE:      return "unsigned 16-bit LE";
    case SoundIoFormatU16BE:      return "unsigned 16-bit LE";
    case SoundIoFormatS24LE:      return "signed 24-bit LE";
    case SoundIoFormatS24BE:      return "signed 24-bit BE";
    case SoundIoFormatU24LE:      return "unsigned 24-bit LE";
    case SoundIoFormatU24BE:      return "unsigned 24-bit BE";
    case SoundIoFormatS32LE:      return "signed 32-bit LE";
    case SoundIoFormatS32BE:      return "signed 32-bit BE";
    case SoundIoFormatU32LE:      return "unsigned 32-bit LE";
    case SoundIoFormatU32BE:      return "unsigned 32-bit BE";
    case SoundIoFormatFloat32LE:  return "float 32-bit LE";
    case SoundIoFormatFloat32BE:  return "float 32-bit BE";
    case SoundIoFormatFloat64LE:  return "float 64-bit LE";
    case SoundIoFormatFloat64BE:  return "float 64-bit BE";

    case SoundIoFormatInvalid:
        return "(invalid sample format)";
    }
    return "(invalid sample format)";
}


const char *soundio_backend_name(enum SoundIoBackend backend) {
    switch (backend) {
        case SoundIoBackendNone: return "(none)";
        case SoundIoBackendPulseAudio: return "PulseAudio";
        case SoundIoBackendAlsa: return "ALSA";
        case SoundIoBackendDummy: return "Dummy";
    }
    soundio_panic("invalid backend enum value: %d", (int)backend);
}

void soundio_destroy(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    if (!si)
        return;

    soundio_disconnect(soundio);

    destroy(si);
}

static void default_on_devices_change(struct SoundIo *) { }
static void default_on_events_signal(struct SoundIo *) { }

struct SoundIo * soundio_create(void) {
    soundio_os_init();
    struct SoundIoPrivate *si = create<SoundIoPrivate>();
    if (!si)
        return NULL;
    SoundIo *soundio = &si->pub;
    soundio->on_devices_change = default_on_devices_change;
    soundio->on_events_signal = default_on_events_signal;
    return soundio;
}

int soundio_connect(struct SoundIo *soundio) {
    int err = 0;

    for (int i = 0; i < array_length(available_backends); i += 1) {
        SoundIoBackend backend = available_backends[i];
        err = soundio_connect_backend(soundio, backend);
        if (!err)
            return 0;
        if (err != SoundIoErrorInitAudioBackend)
            return err;
    }

    return err;
}

int soundio_connect_backend(SoundIo *soundio, SoundIoBackend backend) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;

    if (si->current_backend)
        return SoundIoErrorInvalid;

    int err;
    switch (backend) {
    case SoundIoBackendPulseAudio:
#ifdef SOUNDIO_HAVE_PULSEAUDIO
        si->current_backend = SoundIoBackendPulseAudio;
        if ((err = soundio_pulseaudio_init(si))) {
            soundio_disconnect(soundio);
            return err;
        }
        return 0;
#else
        return SoundIoErrorBackendUnavailable;
#endif
    case SoundIoBackendAlsa:
#ifdef SOUNDIO_HAVE_ALSA
        si->current_backend = SoundIoBackendAlsa;
        if ((err = soundio_alsa_init(si))) {
            soundio_disconnect(soundio);
            return err;
        }
        return 0;
#else
        return SoundIoErrorBackendUnavailable;
#endif
    case SoundIoBackendDummy:
        si->current_backend = SoundIoBackendDummy;
        err = soundio_dummy_init(si);
        if (err)
            soundio_disconnect(soundio);
        return err;
    case SoundIoBackendNone:
        return SoundIoErrorInvalid;
    }
    return SoundIoErrorInvalid;
}

void soundio_disconnect(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;

    if (si->destroy)
        si->destroy(si);
    assert(!si->backend_data);

    si->current_backend = SoundIoBackendNone;

    soundio_destroy_devices_info(si->safe_devices_info);
    si->safe_devices_info = nullptr;

    si->destroy = nullptr;
    si->flush_events = nullptr;
    si->wait_events = nullptr;
    si->wakeup = nullptr;

    si->outstream_init = nullptr;
    si->outstream_destroy = nullptr;
    si->outstream_start = nullptr;
    si->outstream_free_count = nullptr;
    si->outstream_begin_write = nullptr;
    si->outstream_write = nullptr;
    si->outstream_clear_buffer = nullptr;

    si->instream_init = nullptr;
    si->instream_destroy = nullptr;
    si->instream_start = nullptr;
    si->instream_peek = nullptr;
    si->instream_drop = nullptr;
    si->instream_clear_buffer = nullptr;
}

void soundio_flush_events(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    assert(si->flush_events);
    if (si->flush_events)
        si->flush_events(si);
}

int soundio_get_input_device_count(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    soundio_flush_events(soundio);
    assert(si->safe_devices_info);
    return si->safe_devices_info->input_devices.length;
}

int soundio_get_output_device_count(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    soundio_flush_events(soundio);
    assert(si->safe_devices_info);
    return si->safe_devices_info->output_devices.length;
}

int soundio_get_default_input_device_index(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    soundio_flush_events(soundio);
    assert(si->safe_devices_info);
    return si->safe_devices_info->default_input_index;
}

int soundio_get_default_output_device_index(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    soundio_flush_events(soundio);
    assert(si->safe_devices_info);
    return si->safe_devices_info->default_output_index;
}

struct SoundIoDevice *soundio_get_input_device(struct SoundIo *soundio, int index) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    assert(si->safe_devices_info);
    assert(index >= 0);
    assert(index < si->safe_devices_info->input_devices.length);
    SoundIoDevice *device = si->safe_devices_info->input_devices.at(index);
    soundio_device_ref(device);
    return device;
}

struct SoundIoDevice *soundio_get_output_device(struct SoundIo *soundio, int index) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    assert(si->safe_devices_info);
    assert(index >= 0);
    assert(index < si->safe_devices_info->output_devices.length);
    SoundIoDevice *device = si->safe_devices_info->output_devices.at(index);
    soundio_device_ref(device);
    return device;
}

// the name is the identifier for the device. UTF-8 encoded
const char *soundio_device_name(const struct SoundIoDevice *device) {
    return device->name;
}

// UTF-8 encoded
const char *soundio_device_description(const struct SoundIoDevice *device) {
    return device->description;
}

enum SoundIoDevicePurpose soundio_device_purpose(const struct SoundIoDevice *device) {
    return device->purpose;
}

void soundio_device_unref(struct SoundIoDevice *device) {
    if (!device)
        return;

    device->ref_count -= 1;
    assert(device->ref_count >= 0);

    if (device->ref_count == 0) {
        free(device->name);
        free(device->description);
        deallocate(device->formats, device->format_count);
        destroy(device);
    }
}

void soundio_device_ref(struct SoundIoDevice *device) {
    device->ref_count += 1;
}

void soundio_wait_events(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    si->wait_events(si);
}

void soundio_wakeup(struct SoundIo *soundio) {
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    si->wakeup(si);
}

void soundio_outstream_fill_with_silence(struct SoundIoOutStream *outstream) {
    char *buffer;
    int requested_frame_count = soundio_outstream_free_count(outstream);
    while (requested_frame_count > 0) {
        int frame_count = requested_frame_count;
        soundio_outstream_begin_write(outstream, &buffer, &frame_count);
        memset(buffer, 0, frame_count * outstream->bytes_per_frame);
        soundio_outstream_write(outstream, buffer, frame_count);
        requested_frame_count -= frame_count;
    }
}

int soundio_outstream_free_count(struct SoundIoOutStream *outstream) {
    SoundIo *soundio = outstream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)outstream;
    return si->outstream_free_count(si, os);
}

void soundio_outstream_begin_write(struct SoundIoOutStream *outstream,
        char **data, int *frame_count)
{
    SoundIo *soundio = outstream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)outstream;
    si->outstream_begin_write(si, os, data, frame_count);
}

void soundio_outstream_write(struct SoundIoOutStream *outstream,
        char *data, int frame_count)
{
    SoundIo *soundio = outstream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)outstream;
    si->outstream_write(si, os, data, frame_count);
}


struct SoundIoOutStream *soundio_outstream_create(struct SoundIoDevice *device) {
    SoundIoOutStreamPrivate *os = create<SoundIoOutStreamPrivate>();
    if (!os)
        return nullptr;
    SoundIoOutStream *outstream = &os->pub;

    outstream->device = device;
    soundio_device_ref(device);

    // TODO set defaults

    return outstream;
}

int soundio_outstream_open(struct SoundIoOutStream *outstream) {
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)outstream;
    outstream->bytes_per_frame = soundio_get_bytes_per_frame(outstream->format, outstream->layout.channel_count);

    SoundIo *soundio = outstream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    return si->outstream_init(si, os);
}

void soundio_outstream_destroy(SoundIoOutStream *outstream) {
    if (!outstream)
        return;

    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)outstream;
    SoundIo *soundio = outstream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;

    if (si->outstream_destroy)
        si->outstream_destroy(si, os);

    soundio_device_unref(outstream->device);
    destroy(os);
}

int soundio_outstream_start(struct SoundIoOutStream *outstream) {
    SoundIo *soundio = outstream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *)outstream;
    return si->outstream_start(si, os);
}

struct SoundIoInStream *soundio_instream_create(struct SoundIoDevice *device) {
    SoundIoInStreamPrivate *is = create<SoundIoInStreamPrivate>();
    if (!is)
        return nullptr;
    SoundIoInStream *instream = &is->pub;

    instream->device = device;
    soundio_device_ref(device);

    // TODO set defaults

    return instream;
}

int soundio_instream_open(struct SoundIoInStream *instream) {
    instream->bytes_per_frame = soundio_get_bytes_per_frame(instream->format, instream->layout.channel_count);
    SoundIo *soundio = instream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    SoundIoInStreamPrivate *is = (SoundIoInStreamPrivate *)instream;
    return si->instream_init(si, is);
}

int soundio_instream_start(struct SoundIoInStream *instream) {
    SoundIo *soundio = instream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;
    SoundIoInStreamPrivate *is = (SoundIoInStreamPrivate *)instream;
    return si->instream_start(si, is);
}

void soundio_instream_destroy(struct SoundIoInStream *instream) {
    if (!instream)
        return;

    SoundIoInStreamPrivate *is = (SoundIoInStreamPrivate *)instream;
    SoundIo *soundio = instream->device->soundio;
    SoundIoPrivate *si = (SoundIoPrivate *)soundio;

    if (si->instream_destroy)
        si->instream_destroy(si, is);

    soundio_device_unref(instream->device);
    destroy(is);
}

void soundio_destroy_devices_info(SoundIoDevicesInfo *devices_info) {
    if (!devices_info)
        return;

    for (int i = 0; i < devices_info->input_devices.length; i += 1)
        soundio_device_unref(devices_info->input_devices.at(i));
    for (int i = 0; i < devices_info->output_devices.length; i += 1)
        soundio_device_unref(devices_info->output_devices.at(i));

    destroy(devices_info);
}

bool soundio_have_backend(SoundIoBackend backend) {
    switch (backend) {
    case SoundIoBackendPulseAudio:
#ifdef SOUNDIO_HAVE_PULSEAUDIO
        return true;
#else
        return false;
#endif
    case SoundIoBackendAlsa:
#ifdef SOUNDIO_HAVE_ALSA
        return true;
#else
        return false;
#endif
    case SoundIoBackendDummy:
        return true;
    case SoundIoBackendNone:
        return false;
    }
    return false;
}

int soundio_backend_count(struct SoundIo *soundio) {
    return array_length(available_backends);
}

SoundIoBackend soundio_get_backend(struct SoundIo *soundio, int index) {
    return available_backends[index];
}

static bool layout_contains(const SoundIoChannelLayout *available_layouts, int available_layouts_count,
        const SoundIoChannelLayout *target_layout)
{
    for (int i = 0; i < available_layouts_count; i += 1) {
        const SoundIoChannelLayout *available_layout = &available_layouts[i];
        if (soundio_channel_layout_equal(target_layout, available_layout))
            return true;
    }
    return false;
}

const struct SoundIoChannelLayout *soundio_best_matching_channel_layout(
        const struct SoundIoChannelLayout *preferred_layouts, int preferred_layouts_count,
        const struct SoundIoChannelLayout *available_layouts, int available_layouts_count)
{
    for (int i = 0; i < preferred_layouts_count; i += 1) {
        const SoundIoChannelLayout *preferred_layout = &preferred_layouts[i];
        if (layout_contains(available_layouts, available_layouts_count, preferred_layout))
            return preferred_layout;
    }
    return nullptr;
}

static int compare_layouts(const void *a, const void *b) {
    const SoundIoChannelLayout *layout_a = *((SoundIoChannelLayout **)a);
    const SoundIoChannelLayout *layout_b = *((SoundIoChannelLayout **)b);
    if (layout_a->channel_count > layout_b->channel_count)
        return -1;
    else if (layout_a->channel_count < layout_b->channel_count)
        return 1;
    else
        return 0;
}

void soundio_sort_channel_layouts(struct SoundIoChannelLayout *layouts, int layouts_count) {
    if (!layouts)
        return;

    qsort(layouts, layouts_count, sizeof(SoundIoChannelLayout), compare_layouts);
}

void soundio_device_sort_channel_layouts(struct SoundIoDevice *device) {
    soundio_sort_channel_layouts(device->layouts, device->layout_count);
}
