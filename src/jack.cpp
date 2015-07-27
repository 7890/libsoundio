/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "jack.hpp"
#include "soundio.hpp"
#include "atomics.hpp"
#include "os.hpp"
#include "list.hpp"

#include <jack/jack.h>
#include <stdio.h>

static atomic_flag global_msg_callback_flag = ATOMIC_FLAG_INIT;

struct SoundIoJack {
    jack_client_t *client;
    SoundIoOsMutex *mutex;
    SoundIoOsCond *cond;
    // this one is ready to be read with flush_events. protected by mutex
    struct SoundIoDevicesInfo *ready_devices_info;
    bool initialized;
    int sample_rate;
    int buffer_size;
};

struct SoundIoJackPort {
    const char *name;
    int name_len;
};

struct SoundIoJackClient {
    const char *name;
    int name_len;
    bool is_physical;
    SoundIoDevicePurpose purpose;
    SoundIoList<SoundIoJackPort> ports;
};

static void flush_events_jack(struct SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;

    bool change = false;
    SoundIoDevicesInfo *old_devices_info = nullptr;

    soundio_os_mutex_lock(sij->mutex);

    if (sij->ready_devices_info) {
        old_devices_info = si->safe_devices_info;
        si->safe_devices_info = sij->ready_devices_info;
        sij->ready_devices_info = nullptr;
        change = true;
    }

    soundio_os_mutex_unlock(sij->mutex);

    if (change)
        soundio->on_devices_change(soundio);

    soundio_destroy_devices_info(old_devices_info);
}

static void wait_events_jack(struct SoundIoPrivate *si) {
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    flush_events_jack(si);
    soundio_os_mutex_lock(sij->mutex);
    soundio_os_cond_wait(sij->cond, sij->mutex);
    soundio_os_mutex_unlock(sij->mutex);
}

static void wakeup_jack(struct SoundIoPrivate *si) {
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    soundio_os_mutex_lock(sij->mutex);
    soundio_os_cond_signal(sij->cond, sij->mutex);
    soundio_os_mutex_unlock(sij->mutex);
}


static int outstream_open_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *) {
    soundio_panic("TODO");
}

static void outstream_destroy_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *) {
    soundio_panic("TODO");
}

static int outstream_start_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *) {
    soundio_panic("TODO");
}

static int outstream_begin_write_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *,
        SoundIoChannelArea **out_areas, int *frame_count)
{
    soundio_panic("TODO");
}

static int outstream_end_write_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *, int frame_count) {
    soundio_panic("TODO");
}

static int outstream_clear_buffer_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *) {
    soundio_panic("TODO");
}

static int outstream_pause_jack(struct SoundIoPrivate *, struct SoundIoOutStreamPrivate *, bool pause) {
    soundio_panic("TODO");
}



static int instream_open_jack(struct SoundIoPrivate *, struct SoundIoInStreamPrivate *) {
    soundio_panic("TODO");
}

static void instream_destroy_jack(struct SoundIoPrivate *, struct SoundIoInStreamPrivate *) {
    soundio_panic("TODO");
}

static int instream_start_jack(struct SoundIoPrivate *, struct SoundIoInStreamPrivate *) {
    soundio_panic("TODO");
}

static int instream_begin_read_jack(struct SoundIoPrivate *, struct SoundIoInStreamPrivate *,
        SoundIoChannelArea **out_areas, int *frame_count)
{
    soundio_panic("TODO");
}

static int instream_end_read_jack(struct SoundIoPrivate *, struct SoundIoInStreamPrivate *) {
    soundio_panic("TODO");
}

static int instream_pause_jack(struct SoundIoPrivate *, struct SoundIoInStreamPrivate *, bool pause) {
    soundio_panic("TODO");
}

static void split_str(const char *input_str, int input_str_len, char c,
        const char **out_1, int *out_len_1, const char **out_2, int *out_len_2)
{
    *out_1 = input_str;
    while (*input_str) {
        if (*input_str == c) {
            *out_len_1 = input_str - *out_1;
            *out_2 = input_str + 1;
            *out_len_2 = input_str_len - 1 - *out_len_1;
            return;
        }
        input_str += 1;
    }
}

static bool eql_str(const char *str1, int str1_len, const char *str2, int str2_len) {
    if (str1_len != str2_len)
        return false;
    return memcmp(str1, str2, str1_len) == 0;
}

static SoundIoJackClient *find_or_create_client(SoundIoList<SoundIoJackClient> *clients,
        SoundIoDevicePurpose purpose, bool is_physical, const char *client_name, int client_name_len)
{
    for (int i = 0; i < clients->length; i += 1) {
        SoundIoJackClient *client = &clients->at(i);
        if (client->is_physical == is_physical &&
            client->purpose == purpose &&
            eql_str(client->name, client->name_len, client_name, client_name_len))
        {
            return client;
        }
    }
    int err;
    if ((err = clients->add_one()))
        return nullptr;
    SoundIoJackClient *client = &clients->last();
    client->is_physical = is_physical;
    client->purpose = purpose;
    client->name = client_name;
    client->name_len = client_name_len;
    return client;
}

static char *dupe_str(const char *str, int str_len) {
    char *out = allocate_nonzero<char>(str_len + 1);
    if (!out)
        return nullptr;
    memcpy(out, str, str_len);
    out[str_len] = 0;
    return out;
}

static int refresh_devices(SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;

    SoundIoDevicesInfo *devices_info = create<SoundIoDevicesInfo>();
    if (!devices_info)
        return SoundIoErrorNoMem;

    devices_info->default_output_index = -1;
    devices_info->default_input_index = -1;
    const char **port_names = jack_get_ports(sij->client, nullptr, nullptr, 0);
    if (!port_names) {
        destroy(devices_info);
        return SoundIoErrorNoMem;
    }

    SoundIoList<SoundIoJackClient> clients;
    int err;

    const char **port_name_ptr = port_names;
    while (*port_name_ptr) {
        const char *client_and_port_name = *port_name_ptr;
        jack_port_t *jport = jack_port_by_name(sij->client, client_and_port_name);
        int flags = jack_port_flags(jport);
        SoundIoDevicePurpose purpose = (flags & JackPortIsInput) ?
            SoundIoDevicePurposeOutput : SoundIoDevicePurposeInput;
        bool is_physical = flags & JackPortIsPhysical;

        const char *client_name = nullptr;
        const char *port_name = nullptr;
        int client_name_len;
        int port_name_len;
        split_str(client_and_port_name, strlen(client_and_port_name), ':',
                &client_name, &client_name_len, &port_name, &port_name_len);
        if (!client_name || !port_name) {
            // device does not have colon, skip it
            continue;
        }
        SoundIoJackClient *client = find_or_create_client(&clients, purpose, is_physical,
                client_name, client_name_len);
        if (!client) {
            jack_free(port_names);
            destroy(devices_info);
            return SoundIoErrorNoMem;
        }
        if ((err = client->ports.add_one())) {
            jack_free(port_names);
            destroy(devices_info);
            return SoundIoErrorNoMem;
        }
        SoundIoJackPort *port = &client->ports.last();
        port->name = port_name;
        port->name_len = port_name_len;


        port_name_ptr += 1;
    }

    for (int i = 0; i < clients.length; i += 1) {
        SoundIoJackClient *client = &clients.at(i);
        if (client->ports.length <= 0)
            continue;

        SoundIoDevice *device = create<SoundIoDevice>();
        if (!device) {
            jack_free(port_names);
            destroy(devices_info);
            return SoundIoErrorNoMem;
        }
        int description_len = client->name_len + 3 + 2 * client->ports.length;
        for (int port_index = 0; port_index < client->ports.length; port_index += 1) {
            SoundIoJackPort *port = &client->ports.at(port_index);
            description_len += port->name_len;
        }

        device->ref_count = 1;
        device->soundio = soundio;
        device->is_raw = false;
        device->purpose = client->purpose;
        device->name = dupe_str(client->name, client->name_len);
        device->description = allocate<char>(description_len);
        device->layout_count = 1;
        device->layouts = create<SoundIoChannelLayout>();
        device->format_count = 1;
        device->formats = create<SoundIoFormat>();
        device->current_format = SoundIoFormatFloat32NE;
        device->sample_rate_min = sij->sample_rate;
        device->sample_rate_max = sij->sample_rate;
        device->sample_rate_current = sij->sample_rate;
        device->buffer_duration_min = sij->buffer_size / (double) sij->sample_rate;
        device->buffer_duration_max = device->buffer_duration_min;
        device->buffer_duration_current = device->buffer_duration_min;

        if (!device->name || !device->description || !device->layouts || !device->formats) {
            jack_free(port_names);
            soundio_device_unref(device);
            destroy(devices_info);
            return SoundIoErrorNoMem;
        }

        memcpy(device->description, client->name, client->name_len);
        memcpy(&device->description[client->name_len], ": ", 2);
        int index = client->name_len + 2;
        for (int port_index = 0; port_index < client->ports.length; port_index += 1) {
            SoundIoJackPort *port = &client->ports.at(port_index);
            memcpy(&device->description[index], port->name, port->name_len);
            index += port->name_len;
            if (port_index + 1 < client->ports.length) {
                memcpy(&device->description[index], ", ", 2);
                index += 2;
            }
        }

        const struct SoundIoChannelLayout *layout = soundio_channel_layout_get_default(client->ports.length);
        if (layout) {
            device->current_layout = *layout;
        } else {
            for (int port_index = 0; port_index < client->ports.length; port_index += 1)
                device->current_layout.channels[port_index] = SoundIoChannelIdInvalid;
        }

        device->layouts[0] = device->current_layout;
        device->formats[0] = device->current_format;

        SoundIoList<SoundIoDevice *> *device_list;
        if (device->purpose == SoundIoDevicePurposeOutput) {
            device_list = &devices_info->output_devices;
            if (devices_info->default_output_index < 0 && client->is_physical)
                devices_info->default_output_index = device_list->length;
        } else {
            assert(device->purpose == SoundIoDevicePurposeInput);
            device_list = &devices_info->input_devices;
            if (devices_info->default_input_index < 0 && client->is_physical)
                devices_info->default_input_index = device_list->length;
        }

        if (device_list->append(device)) {
            soundio_device_unref(device);
            destroy(devices_info);
            return SoundIoErrorNoMem;
        }

    }
    jack_free(port_names);


    soundio_os_mutex_lock(sij->mutex);
    soundio_destroy_devices_info(sij->ready_devices_info);
    sij->ready_devices_info = devices_info;
    soundio_os_cond_signal(sij->cond, sij->mutex);
    soundio->on_events_signal(soundio);
    soundio_os_mutex_unlock(sij->mutex);

    return 0;
}

static int process_callback(jack_nframes_t nframes, void *arg) {
    ////SoundIoPrivate *si = (SoundIoPrivate *)arg;
    //soundio_panic("TODO process callback");
    return 0;
}

static int buffer_size_callback(jack_nframes_t nframes, void *arg) {
    SoundIoPrivate *si = (SoundIoPrivate *)arg;
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    sij->buffer_size = nframes;
    if (sij->initialized)
        refresh_devices(si);
    return 0;
}

static int sample_rate_callback(jack_nframes_t nframes, void *arg) {
    SoundIoPrivate *si = (SoundIoPrivate *)arg;
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    sij->sample_rate = nframes;
    if (sij->initialized)
        refresh_devices(si);
    return 0;
}

static int xrun_callback(void *arg) {
    //SoundIoPrivate *si = (SoundIoPrivate *)arg;
    soundio_panic("TODO xrun callback");
    return 0;
}

static void port_registration_callback(jack_port_id_t port_id, int reg, void *arg) {
    SoundIoPrivate *si = (SoundIoPrivate *)arg;
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    if (sij->initialized)
        refresh_devices(si);
}

static int port_rename_calllback(jack_port_id_t port_id,
        const char *old_name, const char *new_name, void *arg)
{
    SoundIoPrivate *si = (SoundIoPrivate *)arg;
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    if (sij->initialized)
        refresh_devices(si);
    return 0;
}

static void shutdown_callback(void *arg) {
    //SoundIoPrivate *si = (SoundIoPrivate *)arg;
    soundio_panic("TODO shutdown callback");
}

static void destroy_jack(SoundIoPrivate *si) {
    SoundIoJack *sij = (SoundIoJack *)si->backend_data;
    if (!sij)
        return;

    if (sij->client)
        jack_client_close(sij->client);

    if (sij->cond)
        soundio_os_cond_destroy(sij->cond);

    if (sij->mutex)
        soundio_os_mutex_destroy(sij->mutex);

    soundio_destroy_devices_info(sij->ready_devices_info);

    destroy(sij);
    si->backend_data = nullptr;
}

int soundio_jack_init(struct SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;

    if (!global_msg_callback_flag.test_and_set()) {
        if (soundio->jack_error_callback)
            jack_set_error_function(soundio->jack_error_callback);
        if (soundio->jack_info_callback)
            jack_set_info_function(soundio->jack_info_callback);
        global_msg_callback_flag.clear();
    }

    assert(!si->backend_data);
    SoundIoJack *sij = create<SoundIoJack>();
    if (!sij) {
        destroy_jack(si);
        return SoundIoErrorNoMem;
    }
    si->backend_data = sij;

    sij->mutex = soundio_os_mutex_create();
    if (!sij->mutex) {
        destroy_jack(si);
        return SoundIoErrorNoMem;
    }

    sij->cond = soundio_os_cond_create();
    if (!sij->cond) {
        destroy_jack(si);
        return SoundIoErrorNoMem;
    }

    // We pass JackNoStartServer due to
    // https://github.com/jackaudio/jack2/issues/138
    jack_status_t status;
    sij->client = jack_client_open(soundio->app_name, JackNoStartServer, &status);
    if (!sij->client) {
        destroy_jack(si);
        assert(!(status & JackInvalidOption));
        if (status & JackShmFailure)
            return SoundIoErrorSystemResources;
        if (status & JackNoSuchClient)
            return SoundIoErrorNoSuchClient;

        return SoundIoErrorInitAudioBackend;
    }

    int err;
    if ((err = jack_set_process_callback(sij->client, process_callback, si))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }
    if ((err = jack_set_buffer_size_callback(sij->client, buffer_size_callback, si))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }
    if ((err = jack_set_sample_rate_callback(sij->client, sample_rate_callback, si))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }
    if ((err = jack_set_xrun_callback(sij->client, xrun_callback, si))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }
    if ((err = jack_set_port_registration_callback(sij->client, port_registration_callback, si))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }
    if ((err = jack_set_port_rename_callback(sij->client, port_rename_calllback, si))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }
    jack_on_shutdown(sij->client, shutdown_callback, si);

    if ((err = jack_activate(sij->client))) {
        destroy_jack(si);
        return SoundIoErrorInitAudioBackend;
    }

    sij->initialized = true;
    if ((err = refresh_devices(si))) {
        destroy_jack(si);
        return err;
    }

    si->destroy = destroy_jack;
    si->flush_events = flush_events_jack;
    si->wait_events = wait_events_jack;
    si->wakeup = wakeup_jack;

    si->outstream_open = outstream_open_jack;
    si->outstream_destroy = outstream_destroy_jack;
    si->outstream_start = outstream_start_jack;
    si->outstream_begin_write = outstream_begin_write_jack;
    si->outstream_end_write = outstream_end_write_jack;
    si->outstream_clear_buffer = outstream_clear_buffer_jack;
    si->outstream_pause = outstream_pause_jack;

    si->instream_open = instream_open_jack;
    si->instream_destroy = instream_destroy_jack;
    si->instream_start = instream_start_jack;
    si->instream_begin_read = instream_begin_read_jack;
    si->instream_end_read = instream_end_read_jack;
    si->instream_pause = instream_pause_jack;

    return 0;
}
