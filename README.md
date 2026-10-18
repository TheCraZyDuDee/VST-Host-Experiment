# VST Host Experiment

A lightweight Linux-first real-time audio host for chaining audio plugins and exposing the processed signal as a named virtual microphone.

## Goals

- Keep idle/background CPU and memory use low.
- Support native Linux VST3 plugins first; evaluate VST2 support separately after checking SDK and distribution constraints.
- Let users choose an input device, arrange a plugin chain, and select an output/virtual microphone.
- Keep the audio engine independent from the user interface so a headless mode remains possible.
- Treat Windows-only plugins as a later, optional compatibility project. Native Linux plugins come first.

## Current status

**Early scaffold only.** This repository establishes project direction and a minimal C++ entry point. Audio capture, plugin loading, device routing, and virtual-microphone creation are not implemented yet.

## Technical direction

- **Language:** C++20.
- **Build:** CMake.
- **Audio:** investigate PipeWire-native integration and/or the JACK API (which can run over PipeWire); choose based on latency, virtual-device routing, and packaging tests.
- **UI:** keep optional and separate from the engine. Qt 6 is the leading candidate, but it should not be a dependency of a headless build.
- **Plugin hosting:** start with native Linux formats and isolate plugin-format details behind an engine interface.

The priority is a predictable, low-overhead audio path, not a large feature set. The real-time callback must avoid blocking operations, allocation, filesystem access, and UI work.

## Build the scaffold

Requirements: CMake 3.20+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
./build/vst-host
```

The current executable only prints a status message; it does not process audio yet.

## Development notes

See [the architecture notes](docs/architecture.md) and [the roadmap](docs/roadmap.md).
