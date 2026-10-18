# VST Host Experiment

A lightweight Linux-first real-time audio host for chaining audio plugins and exposing the processed signal as a named virtual microphone.

## Current status

**Prototype 0.2:** a first PipeWire pass-through engine with a fixed gain processor. Plugin loading, a GUI, device enumeration, and runtime gain controls are not implemented yet. This prototype has not yet been built or tested against a live PipeWire session in this development environment.

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

Requirements: CMake 3.20+, a C++20 compiler, PipeWire development headers and pkg-config.

On Debian/Ubuntu, the relevant packages are typically:

```sh
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev
```

Build and run:

```sh
cmake -S . -B build
cmake --build build
./build/vst-host --help
./build/vst-host
```

The application must run inside a logged-in desktop/user session with PipeWire available. By default it asks PipeWire to connect to the default microphone. To target a specific PipeWire node:

```sh
./build/vst-host --input NODE_ID_OR_NAME --name vst-host-mic --gain 1.0
```

Use `--gain 0.5` to attenuate, `--gain 2.0` to amplify, or `--bypass` to bypass the gain stage. Gain is currently set at startup, not changed interactively. Stop with Ctrl+C.

The output source is intended to appear in applications' microphone/input-device lists as **VST Host Virtual Microphone** (or the name supplied with `--name`). This needs verification on the target distribution and PipeWire version; if the node is exposed as an application stream instead of a selectable source, the virtual-source publication strategy will need adjustment.

## Known limitations

- No device picker yet; `--input` accepts a PipeWire target id/name, and the default source is used otherwise.
- Stereo F32 input is requested at 48 kHz. Compatibility with mono-only devices and other negotiated formats still needs testing.
- No plugin hosting, chain editor, meters, persistence, or GUI yet.
- The prototype has not been compiled or measured on a real PipeWire system, so latency, CPU use, and device compatibility are not yet established.

See [architecture notes](docs/architecture.md) and [roadmap](docs/roadmap.md).
