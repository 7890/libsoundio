# libsoundio

C library which provides cross-platform audio input and output. The API is
suitable for real-time software such as digital audio workstations as well
as consumer software such as music players.

This library is an abstraction; however it prioritizes performance and power
over API convenience. Features that only exist in some sound backends are
exposed.

This library is a work-in-progress.

## How It Works

libsoundio tries these backends in order. If unable to connect to that backend,
due to the backend not being installed, or the server not running, or the
platform is wrong, the next backend is tried.

 0. JACK
 0. PulseAudio
 0. ALSA (Linux)
 0. CoreAudio (OSX)
 0. ASIO (Windows)
 0. DirectSound (Windows)
 0. OSS (BSD)
 0. Dummy

## Contributing

libsoundio is programmed in a tiny subset of C++11:

 * No STL.
 * No `new` or `delete`.
 * No `class`. All fields in structs are `public`.
 * No exceptions or run-time type information.
 * No references.
 * No linking against libstdc++.

Don't get tricked - this is a *C library*, not a C++ library. We just take
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

### Building With MXE

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

## Roadmap

 0. sine example working with dummy backend linux, osx, windows
 0. sine example working with pulseaudio backend linux
 0. pipe record to playback example working with dummy linux, osx, windows
 0. pipe record to playback example working with pulseaudio linux
 0. implement CoreAudio (OSX) backend, get examples working
 0. implement DirectSound (Windows) backend, get examples working
 0. implement ALSA (Linux) backend, get examples working
 0. implement JACK backend, get examples working
 0. Avoid calling `panic` in PulseAudio.
 0. implement ASIO (Windows) backend, get examples working
 0. clean up API and improve documentation
 0. use a documentation generator and host the docs somewhere
 0. -fvisibility=hidden and then explicitly export stuff
 0. Integrate into libgroove and test with Groove Basin
 0. Consider testing on FreeBSD

## Planned Uses for libsoundio

 * [Genesis](https://github.com/andrewrk/genesis)
 * [libgroove](https://github.com/andrewrk/libgroove) ([Groove Basin](https://github.com/andrewrk/groovebasin))
