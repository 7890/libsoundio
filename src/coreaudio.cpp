#include "coreaudio.hpp"
#include "soundio.hpp"

#include <assert.h>

static SoundIoDeviceAim aims[] = {
    SoundIoDeviceAimInput,
    SoundIoDeviceAimOutput,
};

static OSStatus on_devices_changed(AudioObjectID in_object_id, UInt32 in_number_addresses,
    const AudioObjectPropertyAddress in_addresses[], void *in_client_data)
{
    SoundIoPrivate *si = (SoundIoPrivate*)in_client_data;
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;

    sica->device_scan_queued.store(true);
    soundio_os_cond_signal(sica->scan_devices_cond, nullptr);

    return noErr;
}

static OSStatus on_service_restarted(AudioObjectID in_object_id, UInt32 in_number_addresses,
    const AudioObjectPropertyAddress in_addresses[], void *in_client_data)
{
    SoundIoPrivate *si = (SoundIoPrivate*)in_client_data;
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;

    sica->service_restarted.store(true);
    soundio_os_cond_signal(sica->scan_devices_cond, nullptr);

    return noErr;
}

static void destroy_ca(struct SoundIoPrivate *si) {
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;

    int err;
    AudioObjectPropertyAddress prop_address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };
    err = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &prop_address,
        on_devices_changed, si);
    assert(!err);

    prop_address.mSelector = kAudioHardwarePropertyServiceRestarted;
    err = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &prop_address,
        on_service_restarted, si);
    assert(!err);

    if (sica->thread) {
        sica->abort_flag.clear();
        soundio_os_cond_signal(sica->scan_devices_cond, nullptr);
        soundio_os_thread_destroy(sica->thread);
    }

    if (sica->cond)
        soundio_os_cond_destroy(sica->cond);

    if (sica->have_devices_cond)
        soundio_os_cond_destroy(sica->have_devices_cond);

    if (sica->scan_devices_cond)
        soundio_os_cond_destroy(sica->scan_devices_cond);

    if (sica->mutex)
        soundio_os_mutex_destroy(sica->mutex);

    soundio_destroy_devices_info(sica->ready_devices_info);
}

/* TODO
static CFStringRef to_cf_string(const char *str) {
    return CFStringCreateWithCString(kCFAllocatorDefault, str, kCFStringEncodingUTF8);
}
*/

// Possible errors:
//  * SoundIoErrorNoMem
//  * SoundIoErrorEncodingString
static int from_cf_string(CFStringRef string_ref, char **out_str, int *out_str_len) {
    assert(string_ref);

    CFIndex length = CFStringGetLength(string_ref);
    CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    char *buf = allocate_nonzero<char>(max_size);
    if (!buf)
        return SoundIoErrorNoMem;

    if (!CFStringGetCString(string_ref, buf, max_size, kCFStringEncodingUTF8)) {
        deallocate(buf, max_size);
        return SoundIoErrorEncodingString;
    }

    *out_str = buf;
    *out_str_len = strlen(buf);
    return 0;
}

static int aim_to_scope(SoundIoDeviceAim aim) {
    return (aim == SoundIoDeviceAimInput) ?
        kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
}
// TODO subscribe to device property changes for every property we read and
// trigger a rescan if anything changes. test with changing the preferredchannellayout
// for the device

// TODO
/*
 *
    @constant       kAudioDevicePropertyDeviceHasChanged
                        The type of this property is a UInt32, but its value has no meaning. This
                        property exists so that clients can listen to it and be told when the
                        configuration of the AudioDevice has changed in ways that cannot otherwise
                        be conveyed through other notifications. In response to this notification,
                        clients should re-evaluate everything they need to know about the device,
                        particularly the layout and values of the controls.
*/
/*
    @constant       kAudioDevicePropertyLatency
                        A UInt32 containing the number of frames of latency in the AudioDevice. Note
                        that input and output latency may differ. Further, the AudioDevice's
                        AudioStreams may have additional latency so they should be queried as well.
                        If both the device and the stream say they have latency, then the total
                        latency for the stream is the device latency summed with the stream latency.
*/
/*
    @constant       kAudioDevicePropertyIcon
                        A CFURLRef that indicates an image file that can be used to represent the
                        device visually. The caller is responsible for releasing the returned
                        CFObject.
*/
/*
    @constant       kAudioDevicePropertyPreferredChannelsForStereo
                        An array of two UInt32s, the first for the left channel, the second for the
                        right channel, that indicate the channel numbers to use for stereo IO on the
                        device. The value of this property can be different for input and output and
                        there are no restrictions on the channel numbers that can be used.
*/

static SoundIoChannelId from_channel_descr(const AudioChannelDescription *descr) {
    switch (descr->mChannelLabel) {
        default:                                        return SoundIoChannelIdInvalid;
        case kAudioChannelLabel_Left:                   return SoundIoChannelIdFrontLeft;
        case kAudioChannelLabel_Right:                  return SoundIoChannelIdFrontRight;
        case kAudioChannelLabel_Center:                 return SoundIoChannelIdFrontCenter;
        case kAudioChannelLabel_LFEScreen:              return SoundIoChannelIdLfe;
        case kAudioChannelLabel_LeftSurround:           return SoundIoChannelIdBackLeft;
        case kAudioChannelLabel_RightSurround:          return SoundIoChannelIdBackRight;
        case kAudioChannelLabel_LeftCenter:             return SoundIoChannelIdFrontLeftCenter;
        case kAudioChannelLabel_RightCenter:            return SoundIoChannelIdFrontRightCenter;
        case kAudioChannelLabel_CenterSurround:         return SoundIoChannelIdBackCenter;
        case kAudioChannelLabel_LeftSurroundDirect:     return SoundIoChannelIdSideLeft;
        case kAudioChannelLabel_RightSurroundDirect:    return SoundIoChannelIdSideRight;
        case kAudioChannelLabel_TopCenterSurround:      return SoundIoChannelIdTopCenter;
        case kAudioChannelLabel_VerticalHeightLeft:     return SoundIoChannelIdTopFrontLeft;
        case kAudioChannelLabel_VerticalHeightCenter:   return SoundIoChannelIdTopFrontCenter;
        case kAudioChannelLabel_VerticalHeightRight:    return SoundIoChannelIdTopFrontRight;
        case kAudioChannelLabel_TopBackLeft:            return SoundIoChannelIdTopBackLeft;
        case kAudioChannelLabel_TopBackCenter:          return SoundIoChannelIdTopBackCenter;
        case kAudioChannelLabel_TopBackRight:           return SoundIoChannelIdTopBackRight;
        case kAudioChannelLabel_RearSurroundLeft:       return SoundIoChannelIdBackLeft;
        case kAudioChannelLabel_RearSurroundRight:      return SoundIoChannelIdBackRight;
        case kAudioChannelLabel_LeftWide:               return SoundIoChannelIdFrontLeftWide;
        case kAudioChannelLabel_RightWide:              return SoundIoChannelIdFrontRightWide;
        case kAudioChannelLabel_LFE2:                   return SoundIoChannelIdLfe2;
        case kAudioChannelLabel_LeftTotal:              return SoundIoChannelIdFrontLeft;
        case kAudioChannelLabel_RightTotal:             return SoundIoChannelIdFrontRight;
        case kAudioChannelLabel_HearingImpaired:        return SoundIoChannelIdHearingImpaired;
        case kAudioChannelLabel_Narration:              return SoundIoChannelIdNarration;
        case kAudioChannelLabel_Mono:                   return SoundIoChannelIdFrontCenter;
        case kAudioChannelLabel_DialogCentricMix:       return SoundIoChannelIdDialogCentricMix;
        case kAudioChannelLabel_CenterSurroundDirect:   return SoundIoChannelIdBackCenter;
        case kAudioChannelLabel_Haptic:                 return SoundIoChannelIdHaptic;

        case kAudioChannelLabel_Ambisonic_W:            return SoundIoChannelIdAmbisonicW;
        case kAudioChannelLabel_Ambisonic_X:            return SoundIoChannelIdAmbisonicX;
        case kAudioChannelLabel_Ambisonic_Y:            return SoundIoChannelIdAmbisonicY;
        case kAudioChannelLabel_Ambisonic_Z:            return SoundIoChannelIdAmbisonicZ;

        case kAudioChannelLabel_MS_Mid:                 return SoundIoChannelIdMsMid;
        case kAudioChannelLabel_MS_Side:                return SoundIoChannelIdMsSide;

        case kAudioChannelLabel_XY_X:                   return SoundIoChannelIdXyX;
        case kAudioChannelLabel_XY_Y:                   return SoundIoChannelIdXyY;

        case kAudioChannelLabel_HeadphonesLeft:         return SoundIoChannelIdHeadphonesLeft;
        case kAudioChannelLabel_HeadphonesRight:        return SoundIoChannelIdHeadphonesRight;
        case kAudioChannelLabel_ClickTrack:             return SoundIoChannelIdClickTrack;
        case kAudioChannelLabel_ForeignLanguage:        return SoundIoChannelIdForeignLanguage;

        case kAudioChannelLabel_Discrete:               return SoundIoChannelIdAux;

        case kAudioChannelLabel_Discrete_0:             return SoundIoChannelIdAux0;
        case kAudioChannelLabel_Discrete_1:             return SoundIoChannelIdAux1;
        case kAudioChannelLabel_Discrete_2:             return SoundIoChannelIdAux2;
        case kAudioChannelLabel_Discrete_3:             return SoundIoChannelIdAux3;
        case kAudioChannelLabel_Discrete_4:             return SoundIoChannelIdAux4;
        case kAudioChannelLabel_Discrete_5:             return SoundIoChannelIdAux5;
        case kAudioChannelLabel_Discrete_6:             return SoundIoChannelIdAux6;
        case kAudioChannelLabel_Discrete_7:             return SoundIoChannelIdAux7;
        case kAudioChannelLabel_Discrete_8:             return SoundIoChannelIdAux8;
        case kAudioChannelLabel_Discrete_9:             return SoundIoChannelIdAux9;
        case kAudioChannelLabel_Discrete_10:            return SoundIoChannelIdAux10;
        case kAudioChannelLabel_Discrete_11:            return SoundIoChannelIdAux11;
        case kAudioChannelLabel_Discrete_12:            return SoundIoChannelIdAux12;
        case kAudioChannelLabel_Discrete_13:            return SoundIoChannelIdAux13;
        case kAudioChannelLabel_Discrete_14:            return SoundIoChannelIdAux14;
        case kAudioChannelLabel_Discrete_15:            return SoundIoChannelIdAux15;
    }
}

// See https://developer.apple.com/library/mac/documentation/MusicAudio/Reference/CoreAudioDataTypesRef/#//apple_ref/doc/constant_group/Audio_Channel_Layout_Tags
// Possible Errors:
// * SoundIoErrorIncompatibleDevice
static int from_coreaudio_layout(const AudioChannelLayout *ca_layout, SoundIoChannelLayout *layout) {
    switch (ca_layout->mChannelLayoutTag) {
    case kAudioChannelLayoutTag_UseChannelDescriptions:
    {
        layout->channel_count = ca_layout->mNumberChannelDescriptions;
        for (int i = 0; i < layout->channel_count; i += 1) {
            layout->channels[i] = from_channel_descr(&ca_layout->mChannelDescriptions[i]);
        }
        break;
    }
    case kAudioChannelLayoutTag_UseChannelBitmap:
        soundio_panic("TODO how the f to parse this");
        return SoundIoErrorIncompatibleDevice;
    case kAudioChannelLayoutTag_Mono:
        layout->channel_count = 1;
        layout->channels[0] = SoundIoChannelIdFrontCenter;
        break;
    case kAudioChannelLayoutTag_Stereo:
    case kAudioChannelLayoutTag_StereoHeadphones:
    case kAudioChannelLayoutTag_MatrixStereo:
    case kAudioChannelLayoutTag_Binaural:
        layout->channel_count = 2;
        layout->channels[0] = SoundIoChannelIdFrontLeft;
        layout->channels[1] = SoundIoChannelIdFrontRight;
        break;
    case kAudioChannelLayoutTag_XY:
        layout->channel_count = 2;
        layout->channels[0] = SoundIoChannelIdXyX;
        layout->channels[1] = SoundIoChannelIdXyY;
        break;
    case kAudioChannelLayoutTag_MidSide:
        layout->channel_count = 2;
        layout->channels[0] = SoundIoChannelIdMsMid;
        layout->channels[1] = SoundIoChannelIdMsSide;
        break;
    case kAudioChannelLayoutTag_Ambisonic_B_Format:
        layout->channel_count = 4;
        layout->channels[0] = SoundIoChannelIdAmbisonicW;
        layout->channels[1] = SoundIoChannelIdAmbisonicX;
        layout->channels[2] = SoundIoChannelIdAmbisonicY;
        layout->channels[3] = SoundIoChannelIdAmbisonicZ;
        break;
    case kAudioChannelLayoutTag_Quadraphonic:
        layout->channel_count = 4;
        layout->channels[0] = SoundIoChannelIdFrontLeft;
        layout->channels[1] = SoundIoChannelIdFrontRight;
        layout->channels[2] = SoundIoChannelIdBackLeft;
        layout->channels[3] = SoundIoChannelIdBackRight;
        break;
    case kAudioChannelLayoutTag_Pentagonal:
        layout->channel_count = 5;
        layout->channels[0] = SoundIoChannelIdSideLeft;
        layout->channels[1] = SoundIoChannelIdSideRight;
        layout->channels[2] = SoundIoChannelIdBackLeft;
        layout->channels[3] = SoundIoChannelIdBackRight;
        layout->channels[4] = SoundIoChannelIdFrontCenter;
        break;
    case kAudioChannelLayoutTag_Hexagonal:
        layout->channel_count = 6;
        layout->channels[0] = SoundIoChannelIdFrontLeft;
        layout->channels[1] = SoundIoChannelIdFrontRight;
        layout->channels[2] = SoundIoChannelIdBackLeft;
        layout->channels[3] = SoundIoChannelIdBackRight;
        layout->channels[4] = SoundIoChannelIdFrontCenter;
        layout->channels[5] = SoundIoChannelIdBackCenter;
        break;
    case kAudioChannelLayoutTag_Octagonal:
        layout->channel_count = 8;
        layout->channels[0] = SoundIoChannelIdFrontLeft;
        layout->channels[1] = SoundIoChannelIdFrontRight;
        layout->channels[2] = SoundIoChannelIdBackLeft;
        layout->channels[3] = SoundIoChannelIdBackRight;
        layout->channels[4] = SoundIoChannelIdFrontCenter;
        layout->channels[5] = SoundIoChannelIdBackCenter;
        layout->channels[6] = SoundIoChannelIdSideLeft;
        layout->channels[7] = SoundIoChannelIdSideRight;
        break;
    case kAudioChannelLayoutTag_Cube:
        layout->channel_count = 8;
        layout->channels[0] = SoundIoChannelIdFrontLeft;
        layout->channels[1] = SoundIoChannelIdFrontRight;
        layout->channels[2] = SoundIoChannelIdBackLeft;
        layout->channels[3] = SoundIoChannelIdBackRight;
        layout->channels[4] = SoundIoChannelIdTopFrontLeft;
        layout->channels[5] = SoundIoChannelIdTopFrontRight;
        layout->channels[6] = SoundIoChannelIdTopBackLeft;
        layout->channels[7] = SoundIoChannelIdTopBackRight;
        break;
// TODO more hardcoded channel layouts
    default:
        return SoundIoErrorIncompatibleDevice;
    }
    soundio_channel_layout_detect_builtin(layout);
    return 0;
}

static bool all_channels_invalid(const struct SoundIoChannelLayout *layout) {
    for (int i = 0; i < layout->channel_count; i += 1) {
        if (layout->channels[i] != SoundIoChannelIdInvalid)
            return false;
    }
    return true;
}

struct RefreshDevices {
    SoundIoDevicesInfo *devices_info;
    int devices_size;
    AudioObjectID *devices;
    CFStringRef string_ref;
    char *device_name;
    int device_name_len;
    AudioBufferList *buffer_list;
    SoundIoDevice *device;
    AudioChannelLayout *audio_channel_layout;
    char *device_uid;
    int device_uid_len;
    AudioValueRange *avr_array;
};

static void deinit_refresh_devices(RefreshDevices *rd) {
    destroy(rd->devices_info);
    deallocate((char*)rd->devices, rd->devices_size);
    if (rd->string_ref)
        CFRelease(rd->string_ref);
    free(rd->device_name);
    free(rd->buffer_list);
    soundio_device_unref(rd->device);
    free(rd->audio_channel_layout);
    free(rd->device_uid);
    free(rd->avr_array);
}

// TODO get the device UID which persists between unplug/plug
static int refresh_devices(struct SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;

    UInt32 io_size;
    OSStatus os_err;
    int err;

    RefreshDevices rd = {0};

    if (!(rd.devices_info = create<SoundIoDevicesInfo>())) {
        deinit_refresh_devices(&rd);
        return SoundIoErrorNoMem;
    }

    AudioObjectPropertyAddress prop_address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };

    if ((os_err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
        &prop_address, 0, nullptr, &io_size)))
    {
        deinit_refresh_devices(&rd);
        return SoundIoErrorOpeningDevice;
    }

    AudioObjectID default_input_id;
    AudioObjectID default_output_id;

    int device_count = io_size / (UInt32)sizeof(AudioObjectID);
    if (device_count >= 1) {
        rd.devices_size = io_size;
        rd.devices = (AudioObjectID *)allocate<char>(rd.devices_size);
        if (!rd.devices) {
            deinit_refresh_devices(&rd);
            return SoundIoErrorNoMem;
        }

        if ((os_err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_address, 0, nullptr,
            &io_size, rd.devices)))
        {
            deinit_refresh_devices(&rd);
            return SoundIoErrorOpeningDevice;
        }


        io_size = sizeof(AudioObjectID);
        prop_address.mSelector = kAudioHardwarePropertyDefaultInputDevice;
        if ((os_err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_address,
            0, nullptr, &io_size, &default_input_id)))
        {
            deinit_refresh_devices(&rd);
            return SoundIoErrorOpeningDevice;
        }

        io_size = sizeof(AudioObjectID);
        prop_address.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        if ((os_err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_address,
            0, nullptr, &io_size, &default_output_id)))
        {
            deinit_refresh_devices(&rd);
            return SoundIoErrorOpeningDevice;
        }
    }

    for (int device_i = 0; device_i < device_count; device_i += 1) {
        AudioObjectID device_id = rd.devices[device_i];

        prop_address.mSelector = kAudioObjectPropertyName;
        prop_address.mScope = kAudioObjectPropertyScopeGlobal;
        prop_address.mElement = kAudioObjectPropertyElementMaster;
        io_size = sizeof(CFStringRef);
        if (rd.string_ref) {
            CFRelease(rd.string_ref);
            rd.string_ref = nullptr;
        }
        if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address,
            0, nullptr, &io_size, &rd.string_ref)))
        {
            deinit_refresh_devices(&rd);
            return SoundIoErrorOpeningDevice;
        }

        free(rd.device_name);
        rd.device_name = nullptr;
        if ((err = from_cf_string(rd.string_ref, &rd.device_name, &rd.device_name_len))) {
            deinit_refresh_devices(&rd);
            return err;
        }

        prop_address.mSelector = kAudioDevicePropertyDeviceUID;
        prop_address.mScope = kAudioObjectPropertyScopeGlobal;
        prop_address.mElement = kAudioObjectPropertyElementMaster;
        io_size = sizeof(CFStringRef);
        if (rd.string_ref) {
            CFRelease(rd.string_ref);
            rd.string_ref = nullptr;
        }
        if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address,
            0, nullptr, &io_size, &rd.string_ref)))
        {
            deinit_refresh_devices(&rd);
            return SoundIoErrorOpeningDevice;
        }

        free(rd.device_uid);
        rd.device_uid = nullptr;
        if ((err = from_cf_string(rd.string_ref, &rd.device_uid, &rd.device_uid_len))) {
            deinit_refresh_devices(&rd);
            return err;
        }


        for (int aim_i = 0; aim_i < array_length(aims); aim_i += 1) {
            SoundIoDeviceAim aim = aims[aim_i];

            io_size = 0;
            prop_address.mSelector = kAudioDevicePropertyStreamConfiguration;
            prop_address.mScope = aim_to_scope(aim);
            prop_address.mElement = kAudioObjectPropertyElementMaster;
            if ((os_err = AudioObjectGetPropertyDataSize(device_id, &prop_address, 0, nullptr, &io_size))) {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }

            free(rd.buffer_list);
            rd.buffer_list = (AudioBufferList*)allocate_nonzero<char>(io_size);
            if (!rd.buffer_list) {
                deinit_refresh_devices(&rd);
                return SoundIoErrorNoMem;
            }

            if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address, 0, nullptr,
                &io_size, rd.buffer_list)))
            {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }

            int channel_count = 0;
            for (int i = 0; i < rd.buffer_list->mNumberBuffers; i += 1) {
                channel_count += rd.buffer_list->mBuffers[i].mNumberChannels;
            }

            if (channel_count <= 0)
                continue;

            SoundIoDevicePrivate *dev = create<SoundIoDevicePrivate>();
            if (!dev) {
                deinit_refresh_devices(&rd);
                return SoundIoErrorNoMem;
            }
            SoundIoDeviceCoreAudio *dca = &dev->backend_data.coreaudio;
            dca->device_id = device_id;
            assert(!rd.device);
            rd.device = &dev->pub;
            rd.device->ref_count = 1;
            rd.device->soundio = soundio;
            rd.device->is_raw = false; // TODO
            rd.device->aim = aim;
            rd.device->id = soundio_str_dupe(rd.device_uid, rd.device_uid_len);
            rd.device->name = soundio_str_dupe(rd.device_name, rd.device_name_len);
            rd.device->layout_count = 1;
            rd.device->layouts = create<SoundIoChannelLayout>();
            rd.device->format_count = 1;
            rd.device->formats = create<SoundIoFormat>();

            if (!rd.device->id || !rd.device->name || !rd.device->layouts || !rd.device->formats) {
                deinit_refresh_devices(&rd);
                return SoundIoErrorNoMem;
            }

            prop_address.mSelector = kAudioDevicePropertyPreferredChannelLayout;
            prop_address.mScope = aim_to_scope(aim);
            prop_address.mElement = kAudioObjectPropertyElementMaster;
            if (!(os_err = AudioObjectGetPropertyDataSize(device_id, &prop_address,
                0, nullptr, &io_size)))
            {
                rd.audio_channel_layout = (AudioChannelLayout *)allocate<char>(io_size);
                if (!rd.audio_channel_layout) {
                    deinit_refresh_devices(&rd);
                    return SoundIoErrorNoMem;
                }
                if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address, 0, nullptr,
                    &io_size, rd.audio_channel_layout)))
                {
                    deinit_refresh_devices(&rd);
                    return SoundIoErrorOpeningDevice;
                }
                if ((err = from_coreaudio_layout(rd.audio_channel_layout, &rd.device->current_layout))) {
                    rd.device->current_layout.channel_count = channel_count;
                }
            }
            if (all_channels_invalid(&rd.device->current_layout)) {
                const struct SoundIoChannelLayout *guessed_layout =
                    soundio_channel_layout_get_default(channel_count);
                if (guessed_layout)
                    rd.device->current_layout = *guessed_layout;
            }

            rd.device->layouts[0] = rd.device->current_layout;
            // in CoreAudio, format is always 32-bit native endian float
            rd.device->formats[0] = SoundIoFormatFloat32NE;

            prop_address.mSelector = kAudioDevicePropertyNominalSampleRate;
            prop_address.mScope = aim_to_scope(aim);
            prop_address.mElement = kAudioObjectPropertyElementMaster;
            io_size = sizeof(double);
            if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address, 0, nullptr,
                &io_size, &rd.device->sample_rate_current)))
            {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }

            prop_address.mSelector = kAudioDevicePropertyAvailableNominalSampleRates;
            prop_address.mScope = aim_to_scope(aim);
            prop_address.mElement = kAudioObjectPropertyElementMaster;
            if ((os_err = AudioObjectGetPropertyDataSize(device_id, &prop_address, 0, nullptr,
                &io_size)))
            {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }
            int avr_array_len = io_size / sizeof(AudioValueRange);
            rd.avr_array = (AudioValueRange*)allocate<char>(io_size);

            if (!rd.avr_array) {
                deinit_refresh_devices(&rd);
                return SoundIoErrorNoMem;
            }

            if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address, 0, nullptr,
                &io_size, rd.avr_array)))
            {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }

            for (int i = 0; i < avr_array_len; i += 1) {
                AudioValueRange *avr = &rd.avr_array[i];
                int min_val = ceil(avr->mMinimum);
                int max_val = floor(avr->mMaximum);
                if (rd.device->sample_rate_min == 0 || min_val < rd.device->sample_rate_min)
                    rd.device->sample_rate_min = min_val;
                if (rd.device->sample_rate_max == 0 || max_val > rd.device->sample_rate_max)
                    rd.device->sample_rate_max = max_val;
            }

            prop_address.mSelector = kAudioDevicePropertyBufferFrameSize;
            prop_address.mScope = aim_to_scope(aim);
            prop_address.mElement = kAudioObjectPropertyElementMaster;
            io_size = sizeof(UInt32);
            UInt32 buffer_frame_size;
            if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address, 0, nullptr,
                &io_size, &buffer_frame_size)))
            {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }
            double use_sample_rate = clamp(rd.device->sample_rate_min, rd.device->sample_rate_current,
                rd.device->sample_rate_max);
            rd.device->buffer_duration_current = buffer_frame_size / use_sample_rate;

            prop_address.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
            prop_address.mScope = aim_to_scope(aim);
            prop_address.mElement = kAudioObjectPropertyElementMaster;
            io_size = sizeof(AudioValueRange);
            AudioValueRange avr;
            if ((os_err = AudioObjectGetPropertyData(device_id, &prop_address, 0, nullptr,
                &io_size, &avr)))
            {
                deinit_refresh_devices(&rd);
                return SoundIoErrorOpeningDevice;
            }
            rd.device->buffer_duration_min = avr.mMinimum / use_sample_rate;
            rd.device->buffer_duration_max = avr.mMaximum / use_sample_rate;



            SoundIoList<SoundIoDevice *> *device_list;
            if (rd.device->aim == SoundIoDeviceAimOutput) {
                device_list = &rd.devices_info->output_devices;
                if (device_id == default_output_id)
                    rd.devices_info->default_output_index = device_list->length;
            } else {
                assert(rd.device->aim == SoundIoDeviceAimInput);
                device_list = &rd.devices_info->input_devices;
                if (device_id == default_input_id)
                    rd.devices_info->default_input_index = device_list->length;
            }

            if ((err = device_list->append(rd.device))) {
                deinit_refresh_devices(&rd);
                return err;
            }
            rd.device = nullptr;
        }
    }


    soundio_os_mutex_lock(sica->mutex);
    soundio_destroy_devices_info(sica->ready_devices_info);
    sica->ready_devices_info = rd.devices_info;
    soundio->on_events_signal(soundio);
    soundio_os_mutex_unlock(sica->mutex);

    rd.devices_info = nullptr;
    deinit_refresh_devices(&rd);

    return 0;
}

static void shutdown_backend(SoundIoPrivate *si, int err) {
    SoundIo *soundio = &si->pub;
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;
    soundio_os_mutex_lock(sica->mutex);
    sica->shutdown_err = err;
    soundio->on_events_signal(soundio);
    soundio_os_mutex_unlock(sica->mutex);
}

static void block_until_have_devices(SoundIoCoreAudio *sica) {
    if (sica->have_devices_flag.load())
        return;
    while (!sica->have_devices_flag.load())
        soundio_os_cond_wait(sica->have_devices_cond, nullptr);
}

static void flush_events_ca(struct SoundIoPrivate *si) {
    SoundIo *soundio = &si->pub;
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;
    block_until_have_devices(sica);

    bool change = false;
    bool cb_shutdown = false;
    SoundIoDevicesInfo *old_devices_info = nullptr;

    soundio_os_mutex_lock(sica->mutex);

    if (sica->shutdown_err && !sica->emitted_shutdown_cb) {
        sica->emitted_shutdown_cb = true;
        cb_shutdown = true;
    } else if (sica->ready_devices_info) {
        old_devices_info = si->safe_devices_info;
        si->safe_devices_info = sica->ready_devices_info;
        sica->ready_devices_info = nullptr;
        change = true;
    }

    soundio_os_mutex_unlock(sica->mutex);

    if (cb_shutdown)
        soundio->on_backend_disconnect(soundio, sica->shutdown_err);
    else if (change)
        soundio->on_devices_change(soundio);

    soundio_destroy_devices_info(old_devices_info);
}

static void wait_events_ca(struct SoundIoPrivate *si) {
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;
    flush_events_ca(si);
    soundio_os_cond_wait(sica->cond, nullptr);
}

static void wakeup_ca(struct SoundIoPrivate *si) {
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;
    soundio_os_cond_signal(sica->cond, nullptr);
}

static void device_thread_run(void *arg) {
    SoundIoPrivate *si = (SoundIoPrivate *)arg;
    SoundIo *soundio = &si->pub;
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;
    int err;

    for (;;) {
        if (!sica->abort_flag.test_and_set())
            break;
        if (sica->service_restarted.load()) {
            shutdown_backend(si, SoundIoErrorBackendDisconnected);
            return;
        }
        if (sica->device_scan_queued.exchange(false)) {
            err = refresh_devices(si);
            if (err)
                shutdown_backend(si, err);
            if (!sica->have_devices_flag.exchange(true)) {
                soundio_os_cond_signal(sica->have_devices_cond, nullptr);
                soundio->on_events_signal(soundio);
            }
            if (err)
                return;
            soundio_os_cond_signal(sica->cond, nullptr);
        }
        soundio_os_cond_wait(sica->scan_devices_cond, nullptr);
    }
}

static void outstream_destroy_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamCoreAudio *osca = &os->backend_data.coreaudio;
    if (osca->output_instance)
        AudioComponentInstanceDispose(osca->output_instance);
}

static OSStatus write_callback_ca(void *userdata, AudioUnitRenderActionFlags *io_action_flags,
    const AudioTimeStamp *in_time_stamp, UInt32 in_bus_number, UInt32 in_number_frames,
    AudioBufferList *io_data)
{
    SoundIoOutStreamPrivate *os = (SoundIoOutStreamPrivate *) userdata;
    SoundIoOutStream *outstream = &os->pub;
    SoundIoOutStreamCoreAudio *osca = &os->backend_data.coreaudio;

    osca->io_data = io_data;
    osca->buffer_index = 0;
    outstream->write_callback(outstream, in_number_frames);
    osca->io_data = nullptr;

    return noErr;
}

static int outstream_open_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamCoreAudio *osca = &os->backend_data.coreaudio;
    SoundIoOutStream *outstream = &os->pub;
    SoundIoDevice *device = outstream->device;
    SoundIoDevicePrivate *dev = (SoundIoDevicePrivate *)device;
    SoundIoDeviceCoreAudio *dca = &dev->backend_data.coreaudio;

    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent output_comp = AudioComponentFindNext(nullptr, &desc);
    if (!output_comp) {
        soundio_panic("find next comp");
    }

    OSStatus os_err;
    if ((os_err = AudioComponentInstanceNew(output_comp, &osca->output_instance))) {
        soundio_panic("AudioComponentInstanceNew");
    }

    if ((os_err = AudioUnitInitialize(osca->output_instance))) {
        soundio_panic("AudioUnitInitialize");
    }

    AudioStreamBasicDescription format = {0};
    format.mSampleRate = outstream->sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat;
    format.mBytesPerPacket = outstream->bytes_per_frame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = outstream->bytes_per_frame;
    format.mChannelsPerFrame = outstream->layout.channel_count;
    format.mBitsPerChannel = 32;

    if ((os_err = AudioUnitSetProperty(osca->output_instance, kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Input, 0, &dca->device_id, sizeof(AudioDeviceID))))
    {
        soundio_panic("set device");
    }

    if ((os_err = AudioUnitSetProperty(osca->output_instance, kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &format, sizeof(AudioStreamBasicDescription))))
    {
        soundio_panic("AudioUnitSetProperty format %d", os_err);
    }

    AURenderCallbackStruct render_callback = {write_callback_ca, os};
    if ((os_err = AudioUnitSetProperty(osca->output_instance, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &render_callback, sizeof(AURenderCallbackStruct))))
    {
        soundio_panic("AudioUnitSetProperty callback");
    }

    return 0;
}

static int outstream_start_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    SoundIoOutStreamCoreAudio *osca = &os->backend_data.coreaudio;
    OSStatus os_err;
    if ((os_err = AudioOutputUnitStart(osca->output_instance))) {
        soundio_panic("audio outut unit start");
    }

    return 0;
}

static int outstream_begin_write_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os,
        SoundIoChannelArea **out_areas, int *out_frame_count)
{
    SoundIoOutStream *outstream = &os->pub;
    SoundIoOutStreamCoreAudio *osca = &os->backend_data.coreaudio;

    if (osca->buffer_index < osca->io_data->mNumberBuffers) {
        AudioBuffer *audio_buffer = &osca->io_data->mBuffers[osca->buffer_index];
        assert(audio_buffer->mNumberChannels == outstream->layout.channel_count);
        *out_frame_count = audio_buffer->mDataByteSize / outstream->bytes_per_frame;
        assert((audio_buffer->mDataByteSize % outstream->bytes_per_frame) == 0);
        for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
            osca->areas[ch].ptr = ((char*)audio_buffer->mData) + outstream->bytes_per_sample * ch;
            osca->areas[ch].step = outstream->bytes_per_frame;
        }
        *out_areas = osca->areas;
    } else {
        *out_areas = nullptr;
        *out_frame_count = 0;
    }
    return 0;
}

static int outstream_end_write_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os, int)
{
    SoundIoOutStreamCoreAudio *osca = &os->backend_data.coreaudio;
    osca->buffer_index += 1;
    return 0;
}

static int outstream_clear_buffer_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os) {
    soundio_panic("TODO clear buffer");
}

static int outstream_pause_ca(struct SoundIoPrivate *si, struct SoundIoOutStreamPrivate *os, bool pause) {
    soundio_panic("TODO pause");
}



static int instream_open_ca(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    soundio_panic("TODO open");
}

static void instream_destroy_ca(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    soundio_panic("TODO destroy");
}

static int instream_start_ca(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    soundio_panic("TODO start");
}

static int instream_begin_read_ca(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is,
        SoundIoChannelArea **out_areas, int *frame_count)
{
    soundio_panic("TODO begin read");
}

static int instream_end_read_ca(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is) {
    soundio_panic("TODO end read");
}

static int instream_pause_ca(struct SoundIoPrivate *si, struct SoundIoInStreamPrivate *is, bool pause) {
    soundio_panic("TODO pause");
}


// Possible errors:
// * SoundIoErrorNoMem
int soundio_coreaudio_init(SoundIoPrivate *si) {
    SoundIoCoreAudio *sica = &si->backend_data.coreaudio;
    int err;

    sica->have_devices_flag.store(false);
    sica->device_scan_queued.store(true);
    sica->service_restarted.store(false);
    sica->abort_flag.test_and_set();

    sica->mutex = soundio_os_mutex_create();
    if (!sica->mutex) {
        destroy_ca(si);
        return SoundIoErrorNoMem;
    }

    sica->cond = soundio_os_cond_create();
    if (!sica->cond) {
        destroy_ca(si);
        return SoundIoErrorNoMem;
    }

    sica->have_devices_cond = soundio_os_cond_create();
    if (!sica->have_devices_cond) {
        destroy_ca(si);
        return SoundIoErrorNoMem;
    }

    sica->scan_devices_cond = soundio_os_cond_create();
    if (!sica->scan_devices_cond) {
        destroy_ca(si);
        return SoundIoErrorNoMem;
    }

    AudioObjectPropertyAddress prop_address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };
    if ((err = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &prop_address,
        on_devices_changed, si)))
    {
        soundio_panic("add prop listener");
    }

    prop_address.mSelector = kAudioHardwarePropertyServiceRestarted;
    if ((err = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &prop_address,
        on_service_restarted, si)))
    {
        soundio_panic("add prop listener 2");
    }

    if ((err = soundio_os_thread_create(device_thread_run, si, false, &sica->thread))) {
        destroy_ca(si);
        return err;
    }

    si->destroy = destroy_ca;
    si->flush_events = flush_events_ca;
    si->wait_events = wait_events_ca;
    si->wakeup = wakeup_ca;

    si->outstream_open = outstream_open_ca;
    si->outstream_destroy = outstream_destroy_ca;
    si->outstream_start = outstream_start_ca;
    si->outstream_begin_write = outstream_begin_write_ca;
    si->outstream_end_write = outstream_end_write_ca;
    si->outstream_clear_buffer = outstream_clear_buffer_ca;
    si->outstream_pause = outstream_pause_ca;

    si->instream_open = instream_open_ca;
    si->instream_destroy = instream_destroy_ca;
    si->instream_start = instream_start_ca;
    si->instream_begin_read = instream_begin_read_ca;
    si->instream_end_read = instream_end_read_ca;
    si->instream_pause = instream_pause_ca;

    return 0;
}
