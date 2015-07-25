# libsoundio

C library which provides cross-platform audio input and output. The API is
suitable for real-time software such as digital audio workstations as well
as consumer software such as music players.

This library is an abstraction; however it prioritizes performance and power
over API convenience. Features that only exist in some sound backends are
exposed.

**This library is a work-in-progress.**

## Features

 * Supported backends:
   - [PulseAudio](http://www.freedesktop.org/wiki/Software/PulseAudio/)
   - [ALSA](http://www.alsa-project.org/)
   - Dummy (silence)
   - (planned) [JACK](http://jackaudio.org/)
   - (planned) [CoreAudio](https://developer.apple.com/library/mac/documentation/MusicAudio/Conceptual/CoreAudioOverview/Introduction/Introduction.html)
   - (planned) [WASAPI](https://msdn.microsoft.com/en-us/library/windows/desktop/dd371455%28v=vs.85%29.aspx)
   - (planned) [ASIO](http://www.asio4all.com/)
 * C library. Depends only on the respective backend API libraries and libc.
   Does *not* depend on libstdc++, and does *not* have exceptions, run-time type
   information, or [setjmp](http://latentcontent.net/2007/12/05/libpng-worst-api-ever/).
 * Does not write anything to stdio. I'm looking at you,
  [PortAudio](http://www.portaudio.com/).
 * Supports channel layouts (also known as channel maps), important for
   surround sound applications.
 * Ability to monitor devices and get an event when available devices change.
 * Detects which input device is default and which output device is default.
 * Ability to connect to multiple backends at once. For example you could have
   an ALSA device open and a JACK device open at the same time.
 * Meticulously checks all return codes and memory allocations and uses
   meaningful error codes.

## Synopsis

Complete program to emit a sine wave over the default device using the best
backend:

```c
#include <soundio/soundio.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

static const float PI = 3.1415926535f;
static float seconds_offset = 0.0f;
static void write_callback(struct SoundIoOutStream *outstream, int requested_frame_count) {
    float float_sample_rate = outstream->sample_rate;
    float seconds_per_frame = 1.0f / float_sample_rate;
    int err;

    for (;;) {
        int frame_count = requested_frame_count;

        struct SoundIoChannelArea *areas;
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

        if ((err = soundio_outstream_end_write(outstream, frame_count)))
            panic("%s", soundio_strerror(err));

        requested_frame_count -= frame_count;
        if (requested_frame_count <= 0)
            break;
    }
}

int main(int argc, char **argv) {
    struct SoundIo *soundio = soundio_create();
    if (!soundio)
        panic("out of memory");

    if ((err = soundio_connect(soundio)))
        panic("error connecting: %s", soundio_strerror(err));

    int default_out_device_index = soundio_get_default_output_device_index(soundio);
    if (default_out_device_index < 0)
        panic("no output device found");

    struct SoundIoDevice *device = soundio_get_output_device(soundio, default_out_device_index);
    if (!device)
        panic("out of memory");

    fprintf(stderr, "Output device: %s: %s\n", device->name, device->description);

    struct SoundIoOutStream *outstream = soundio_outstream_create(device);
    outstream->format = SoundIoFormatFloat32NE;
    outstream->write_callback = write_callback;

    if ((err = soundio_outstream_open(outstream)))
        panic("unable to open device: %s", soundio_strerror(err));

    if (outstream->layout_error)
        fprintf(stderr, "unable to set channel layout: %s\n", soundio_strerror(outstream->layout_error));

    if ((err = soundio_outstream_start(outstream)))
        panic("unable to start device: %s", soundio_strerror(err));

    for (;;)
        soundio_wait_events(soundio);

    soundio_outstream_destroy(outstream);
    soundio_device_unref(device);
    soundio_destroy(soundio);
    return 0;
}
```

### Backend Priority

When you use `soundio_connect`, libsoundio tries these backends in order.
If unable to connect to that backend, due to the backend not being installed,
or the server not running, or the platform is wrong, the next backend is tried.

 0. JACK
 0. PulseAudio
 0. ALSA (Linux)
 0. CoreAudio (OSX)
 0. WASAPI (Windows)
 0. ASIO (Windows)
 0. Dummy

If you don't like this order, you can use `soundio_connect_backend` to
explicitly choose a backend to connect to. You can use `soundio_backend_count`
and `soundio_get_backend` to get the list of available backends.

For complete API documentation, see `src/soundio.h`.

## Contributing

libsoundio is programmed in a tiny subset of C++11:

 * No STL.
 * No `new` or `delete`.
 * No `class`. All fields in structs are `public`.
 * No exceptions or run-time type information.
 * No references.
 * No linking against libstdc++.

Do not be fooled - this is a *C library*, not a C++ library. We just take
advantage of a select few C++11 compiler features such as templates, and then
link against libc.

### Building

Install the dependencies:

 * cmake
 * ALSA library (optional)
 * libjack2 (optional)
 * libpulseaudio (optional)

```
mkdir build
cd build
cmake ..
make
sudo make install
```

### Building for Windows

You can build libsoundio with [mxe](http://mxe.cc/). Follow the
[requirements](http://mxe.cc/#requirements) section to install the
packages necessary on your system. Then somewhere on your file system:

```
git clone https://github.com/mxe/mxe
cd mxe
make gcc
```

Then in the libsoundio source directory (replace "/path/to/mxe" with the
appropriate path):

```
mkdir build-win
cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/mxe/usr/i686-w64-mingw32.static/share/cmake/mxe-conf.cmake
make
```

#### Running the Tests

```
make test
```

For more detailed output:

```
make
./unit_tests
```

To see test coverage, install lcov, run `make coverage` and then
view `coverage/index.html` in a browser.

## Roadmap

 0. implement JACK backend, get examples working
 0. why does pulseaudio microphone use up all the CPU?
 0. merge in/out stream structures and functions?
 0. implement CoreAudio (OSX) backend, get examples working
 0. implement WASAPI (Windows) backend, get examples working
 0. implement ASIO (Windows) backend, get examples working
 0. Avoid calling `panic` in PulseAudio.
 0. Figure out a way to test prebuf. I suspect prebuf not working for ALSA
    which is why we have to pre-fill the ring buffer with silence for
    the microphone example.
 0. PulseAudio: when prebuf gets set to 0 need to pass `PA_STREAM_START_CORKED`.
 0. In ALSA do we need to wake up the poll when destroying the in or out stream?
 0. Create a test for clearing the playback buffer.
 0. Create a test for pausing and resuming input and output streams.
 0. Create a test for the latency / synchronization API.
    - Input is an audio file and some events indexed at particular frame - when
      listening the events should line up exactly with a beat or visual
      indicator, even when the latency is large.
    - Play the audio file, have the user press an input right at the beat. Find
      out what the frame index it thinks the user pressed it at and make sure
      that is correct.
 0. Expose JACK options in `jack_client_open`
 0. Allow calling functions from outside the callbacks as long as they first
    call lock and then unlock when done.
 0. Should pause/resume be callable from outside the callbacks?
 0. clean up API and improve documentation
    - make sure every function which can return an error documents which errors
      it can return
 0. use a documentation generator and host the docs somewhere
 0. -fvisibility=hidden and then explicitly export stuff, or
    explicitly make the unexported stuff private
 0. Integrate into libgroove and test with Groove Basin
 0. look at microphone example and determine if fewer memcpys can be done
    with the audio data
    - test that sending the frame count to begin read works with PulseAudio
 0. add len arguments to APIs that have char *
 0. Test in an app that needs to synchronize video to test the
    latency/synchronization API.
 0. Support PulseAudio proplist properties for main context and streams
 0. custom allocator support
 0. mlock memory which is accessed in the real time path
 0. instead of `void *backend_data` use a union for better cache locality
    and smaller mlock requirements
 0. Consider testing on FreeBSD
 0. make rtprio warning a callback and have existing behavior be the default callback
 0. write detailed docs on buffer underflows explaining when they occur, what state
    changes are related to them, and how to recover from them.

## Planned Uses for libsoundio

 * [Genesis](https://github.com/andrewrk/genesis)
 * [libgroove](https://github.com/andrewrk/libgroove) ([Groove Basin](https://github.com/andrewrk/groovebasin))
