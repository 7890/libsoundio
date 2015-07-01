/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef SOUNDIO_SOUNDIO_H
#define SOUNDIO_SOUNDIO_H

#include "config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

struct SoundIo;
struct SoundIoDevicesInfo;

enum SoundIoError {
    SoundIoErrorNone,
    SoundIoErrorNoMem,
    SoundIoErrorInitAudioBackend,
    SoundIoErrorSystemResources,
    SoundIoErrorOpeningDevice,
};

enum SoundIoChannelId {
    SoundIoChannelIdInvalid,
    SoundIoChannelIdFrontLeft,
    SoundIoChannelIdFrontRight,
    SoundIoChannelIdFrontCenter,
    SoundIoChannelIdLowFrequency,
    SoundIoChannelIdBackLeft,
    SoundIoChannelIdBackRight,
    SoundIoChannelIdFrontLeftOfCenter,
    SoundIoChannelIdFrontRightOfCenter,
    SoundIoChannelIdBackCenter,
    SoundIoChannelIdSideLeft,
    SoundIoChannelIdSideRight,
    SoundIoChannelIdTopCenter,
    SoundIoChannelIdTopFrontLeft,
    SoundIoChannelIdTopFrontCenter,
    SoundIoChannelIdTopFrontRight,
    SoundIoChannelIdTopBackLeft,
    SoundIoChannelIdTopBackCenter,
    SoundIoChannelIdTopBackRight,

    SoundIoChannelIdCount,
};

#define SOUNDIO_MAX_CHANNELS 32
struct SoundIoChannelLayout {
    const char *name;
    int channel_count;
    enum SoundIoChannelId channels[SOUNDIO_MAX_CHANNELS];
};

enum SoundIoChannelLayoutId {
    SoundIoChannelLayoutIdMono,
    SoundIoChannelLayoutIdStereo,
    SoundIoChannelLayoutId2Point1,
    SoundIoChannelLayoutId3Point0,
    SoundIoChannelLayoutId3Point0Back,
    SoundIoChannelLayoutId3Point1,
    SoundIoChannelLayoutId4Point0,
    SoundIoChannelLayoutId4Point1,
    SoundIoChannelLayoutIdQuad,
    SoundIoChannelLayoutIdQuadSide,
    SoundIoChannelLayoutId5Point0,
    SoundIoChannelLayoutId5Point0Back,
    SoundIoChannelLayoutId5Point1,
    SoundIoChannelLayoutId5Point1Back,
    SoundIoChannelLayoutId6Point0,
    SoundIoChannelLayoutId6Point0Front,
    SoundIoChannelLayoutIdHexagonal,
    SoundIoChannelLayoutId6Point1,
    SoundIoChannelLayoutId6Point1Back,
    SoundIoChannelLayoutId6Point1Front,
    SoundIoChannelLayoutId7Point0,
    SoundIoChannelLayoutId7Point0Front,
    SoundIoChannelLayoutId7Point1,
    SoundIoChannelLayoutId7Point1Wide,
    SoundIoChannelLayoutId7Point1WideBack,
    SoundIoChannelLayoutIdOctagonal,
};

enum SoundIoBackend {
    SoundIoBackendPulseAudio,
    SoundIoBackendDummy,
};

enum SoundIoDevicePurpose {
    SoundIoDevicePurposeInput,
    SoundIoDevicePurposeOutput,
};

// always native-endian
enum SoundIoSampleFormat {
    SoundIoSampleFormatUInt8,
    SoundIoSampleFormatInt16,
    SoundIoSampleFormatInt24,
    SoundIoSampleFormatInt32,
    SoundIoSampleFormatFloat,
    SoundIoSampleFormatDouble,
    SoundIoSampleFormatInvalid,
};

struct SoundIoDevice {
    struct SoundIo *soundio;
    char *name;
    char *description;
    struct SoundIoChannelLayout channel_layout;
    enum SoundIoSampleFormat default_sample_format;
    double default_latency;
    int default_sample_rate;
    enum SoundIoDevicePurpose purpose;
    int ref_count;
};

struct SoundIoOutputDevice {
    void *backend_data;
    struct SoundIoDevice *device;
    enum SoundIoSampleFormat sample_format;
    double latency;
    int bytes_per_frame;

    void *userdata;
    void (*underrun_callback)(struct SoundIoOutputDevice *);
    void (*write_callback)(struct SoundIoOutputDevice *, int frame_count);
};

struct SoundIoInputDevice {
    void *backend_data;
    struct SoundIoDevice *device;
    enum SoundIoSampleFormat sample_format;
    double latency;
    int bytes_per_frame;

    void *userdata;
    void (*read_callback)(struct SoundIoInputDevice *);
};

struct SoundIo {
    enum SoundIoBackend current_backend;
    void *backend_data;

    // safe to read without a mutex from a single thread
    struct SoundIoDevicesInfo *safe_devices_info;

    void *userdata;
    void (*on_devices_change)(struct SoundIo *);
    void (*on_events_signal)(struct SoundIo *);

    void (*destroy)(struct SoundIo *);
    void (*flush_events)(struct SoundIo *);
    void (*refresh_audio_devices)(struct SoundIo *);

    int (*output_device_init)(struct SoundIo *, struct SoundIoOutputDevice *);
    void (*output_device_destroy)(struct SoundIo *, struct SoundIoOutputDevice *);
    int (*output_device_start)(struct SoundIo *, struct SoundIoOutputDevice *);
    int (*output_device_free_count)(struct SoundIo *, struct SoundIoOutputDevice *);
    void (*output_device_begin_write)(struct SoundIo *, struct SoundIoOutputDevice *,
            char **data, int *frame_count);
    void (*output_device_write)(struct SoundIo *, struct SoundIoOutputDevice *,
            char *data, int frame_count);
    void (*output_device_clear_buffer)(struct SoundIo *, struct SoundIoOutputDevice *);


    int (*input_device_init)(struct SoundIo *, struct SoundIoInputDevice *);
    void (*input_device_destroy)(struct SoundIo *, struct SoundIoInputDevice *);
    int (*input_device_start)(struct SoundIo *, struct SoundIoInputDevice *);
    void (*input_device_peek)(struct SoundIo *, struct SoundIoInputDevice *,
            const char **data, int *frame_count);
    void (*input_device_drop)(struct SoundIo *, struct SoundIoInputDevice *);
    void (*input_device_clear_buffer)(struct SoundIo *, struct SoundIoInputDevice *);
};

// Main Context

// Create a SoundIo context.
// Returns an error code.
int soundio_create(struct SoundIo **out_soundio);

void soundio_destroy(struct SoundIo *soundio);

const char *soundio_error_string(int error);
const char *soundio_backend_name(enum SoundIoBackend backend);

// when you call this, the on_devices_change and on_events_signal callbacks
// might be called. This is the only time those functions will be called.
void soundio_flush_events(struct SoundIo *soundio);

// flushes events as they occur, blocks until you call soundio_wakeup
// be ready for spurious wakeups
void soundio_wait_events(struct SoundIo *soundio);

// makes soundio_wait_events stop blocking
void soundio_wakeup(struct SoundIo *soundio);



// Channel Layouts

bool soundio_channel_layout_equal(const struct SoundIoChannelLayout *a,
        const struct SoundIoChannelLayout *b);

const char *soundio_get_channel_name(enum SoundIoChannelId id);

int soundio_channel_layout_builtin_count(void);
const struct SoundIoChannelLayout *soundio_channel_layout_get_builtin(int index);

void soundio_debug_print_channel_layout(const struct SoundIoChannelLayout *layout);

int soundio_channel_layout_find_channel(
        const struct SoundIoChannelLayout *layout, enum SoundIoChannelId channel);



// Sample Formats

int soundio_get_bytes_per_sample(enum SoundIoSampleFormat sample_format);

static inline int soundio_get_bytes_per_frame(enum SoundIoSampleFormat sample_format, int channel_count) {
    return soundio_get_bytes_per_sample(sample_format) * channel_count;
}

static inline int soundio_get_bytes_per_second(enum SoundIoSampleFormat sample_format,
        int channel_count, int sample_rate)
{
    return soundio_get_bytes_per_frame(sample_format, channel_count) * sample_rate;
}

const char * soundio_sample_format_string(enum SoundIoSampleFormat sample_format);



// Devices

// returns -1 on error
int soundio_get_input_device_count(struct SoundIo *soundio);
int soundio_get_output_device_count(struct SoundIo *soundio);

// returns NULL on error
// call soundio_audio_device_unref when you no longer have a reference to the pointer.
struct SoundIoDevice *soundio_get_input_device(struct SoundIo *soundio, int index);
struct SoundIoDevice *soundio_get_output_device(struct SoundIo *soundio, int index);

// returns the index of the default input device, or -1 on error
int soundio_get_default_input_device_index(struct SoundIo *soundio);

// returns the index of the default output device, or -1 on error
int soundio_get_default_output_device_index(struct SoundIo *soundio);

void soundio_audio_device_ref(struct SoundIoDevice *device);
void soundio_audio_device_unref(struct SoundIoDevice *device);

// the name is the identifier for the device. UTF-8 encoded
const char *soundio_audio_device_name(const struct SoundIoDevice *device);

// UTF-8 encoded
const char *soundio_audio_device_description(const struct SoundIoDevice *device);

const struct SoundIoChannelLayout *soundio_audio_device_channel_layout(const struct SoundIoDevice *device);
int soundio_audio_device_sample_rate(const struct SoundIoDevice *device);

bool soundio_audio_device_equal(
        const struct SoundIoDevice *a,
        const struct SoundIoDevice *b);
enum SoundIoDevicePurpose soundio_device_purpose(const struct SoundIoDevice *device);



// Output Devices

int soundio_output_device_create(struct SoundIoDevice *audio_device,
        enum SoundIoSampleFormat sample_format,
        double latency, void *userdata,
        void (*write_callback)(struct SoundIoOutputDevice *, int),
        void (*underrun_callback)(struct SoundIoOutputDevice *),
        struct SoundIoOutputDevice **out_output_device);
void soundio_output_device_destroy(struct SoundIoOutputDevice *device);

int soundio_output_device_start(struct SoundIoOutputDevice *device);

void soundio_output_device_fill_with_silence(struct SoundIoOutputDevice *device);


// number of frames available to write
int soundio_output_device_free_count(struct SoundIoOutputDevice *device);
void soundio_output_device_begin_write(struct SoundIoOutputDevice *device,
        char **data, int *frame_count);
void soundio_output_device_write(struct SoundIoOutputDevice *device,
        char *data, int frame_count);

void soundio_output_device_clear_buffer(struct SoundIoOutputDevice *device);



// Input Devices

int soundio_input_device_create(struct SoundIoDevice *audio_device,
        enum SoundIoSampleFormat sample_format, double latency, void *userdata,
        void (*read_callback)(struct SoundIoOutputDevice *),
        struct SoundIoOutputDevice **out_input_device);
void soundio_input_device_destroy(struct SoundIoOutputDevice *device);

int soundio_input_device_start(struct SoundIoOutputDevice *device);
void soundio_input_device_peek(struct SoundIoOutputDevice *device,
        const char **data, int *frame_count);
void soundio_input_device_drop(struct SoundIoOutputDevice *device);

void soundio_input_device_clear_buffer(struct SoundIoOutputDevice *device);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
