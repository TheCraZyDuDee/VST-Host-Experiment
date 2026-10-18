# VST Host Experiment

A lightweight Linux-first real-time audio host for chaining audio plugins and exposing the processed signal as a named virtual microphone.

## Current status

**Prototype 0.4 development:** the PipeWire path now supports an optional single native Linux VST3 audio effect, behind the format-neutral processor interface. The default build remains SDK-free. The VST3 path is an initial integration and still needs a real plugin build/load/process test on Linux. A GUI, device enumeration, multi-plugin chains, plugin state, and runtime parameter controls are not implemented yet.

## Goals

- Keep idle/background CPU and memory use low.
- Support native Linux VST3 plugins first; evaluate VST2 support separately after checking SDK and distribution constraints.
- Let users choose an input device, arrange a plugin chain, and select an output/virtual microphone.
- Keep the audio engine independent from the user interface so a headless mode remains possible.
- Treat Windows-only plugins as a later, optional compatibility project. Native Linux plugins come first.

## Technical direction

- **Language:** C++20.
- **Build:** CMake.
- **Audio:** PipeWire-native API, using a capture stream and a named virtual source.
- **UI:** optional and separate from the engine. Qt 6 is a candidate, but it should not be a dependency of a headless build.
- **Plugin hosting:** native Linux formats first, behind a format-neutral processor interface.

The real-time path uses a bounded single-producer/single-consumer ring buffer between PipeWire callbacks. The callbacks do not take locks or allocate memory. Buffer underruns are filled with silence; overflow drops incoming frames rather than blocking the audio thread.

## Build

Requirements for the default build: CMake 3.20+, a C++20 compiler, PipeWire development headers and pkg-config. Enabling VST3 hosting requires CMake 3.25+ and a network connection on the first configure unless you already have the SDK locally.

On Debian/Ubuntu, the relevant packages are typically:

```sh
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev
```

Build and run without downloading the VST3 SDK:

```sh
cmake -S . -B build
cmake --build build
./build/vst-host --help
./build/vst-host
```

To enable VST3 hosting, let CMake fetch the pinned Steinberg SDK and build the required hosting components as part of the same build:

```sh
cmake -S . -B build-vst3 -DVST_HOST_ENABLE_VST3=ON
cmake --build build-vst3
ctest --test-dir build-vst3 --output-on-failure
```

The first configure downloads the SDK and its required source submodules; later builds reuse them in the CMake build directory. VSTGUI and the SDK's example plugins are disabled because this headless host does not need them. To use an SDK checkout you already have instead, pass `-DVST3_SDK_ROOT=/path/to/vst3sdk` with `-DVST_HOST_ENABLE_VST3=ON`. VST3 hosting stays optional, so the normal build does not need a network connection or the SDK. The SDK is distributed under the terms in the [official SDK repository](https://github.com/steinbergmedia/vst3sdk).

The application must run inside a logged-in desktop/user session with PipeWire available. By default it asks PipeWire to connect to the default microphone. To target a specific PipeWire node:

```sh
./build/vst-host --input NODE_ID_OR_NAME --name vst-host-mic --gain 1.0
```

Use `--gain 0.5` to attenuate, `--gain 2.0` to amplify, or `--bypass` to bypass the gain stage. With a VST3-enabled build, `--plugin /path/to/Effect.vst3` loads one native Linux VST3 audio effect before the gain stage. For example:

```sh
./build-vst3/vst-host --input NODE_ID_OR_NAME --plugin /path/to/Effect.vst3 --name vst-host-mic
```

The first integration supports the plug-in’s first stereo audio input/output bus only. `--latency-frames 128` is the default and requests a 128-frame PipeWire interval (about 2.67 ms at 48 kHz). If you hear dropouts, try `256` (5.33 ms), `512` (10.67 ms), or `1024` (21.33 ms). On shutdown, the host reports dropped and underrun frame counts as a basic troubleshooting diagnostic. This is a request, not a guarantee: actual latency depends on the PipeWire graph and the receiving application. The host also discards stale queued audio if the virtual microphone was suspended, rather than replaying a large backlog. Gain and latency are set at startup, not changed interactively. Stop with Ctrl+C.

The output source is intended to appear in applications' microphone/input-device lists as **VST Host Virtual Microphone** (or the name supplied with `--name`). This needs verification on the target distribution and PipeWire version; if the node is exposed as an application stream instead of a selectable source, the virtual-source publication strategy will need adjustment.

## Known limitations

- No device picker yet; `--input` accepts a PipeWire target id/name, and the default source is used otherwise.
- Stereo F32 input is requested at 48 kHz. Compatibility with mono-only devices and other negotiated formats still needs testing.
- Multi-plugin chains, plug-in state persistence, plug-in editor UI, parameter controls, meters, and GUI are not implemented. The initial VST3 path has not yet been validated against a real third-party effect on the target Linux system.
- The prototype has not been compiled or measured on a real PipeWire system, so latency, CPU use, and device compatibility are not yet established.

See [architecture notes](docs/architecture.md) and [roadmap](docs/roadmap.md).
