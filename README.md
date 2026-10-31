# VST Host Experiment

A lightweight Linux-first real-time audio host for chaining audio plugins and exposing the processed signal as a named virtual microphone.

## Current status

**Prototype 0.6 development:** the command-line host supports optional native Linux VST3 effects in an ordered chain. The optional Qt 6 frontend provides chain editing, plugin editors on X11, and portable configuration import/export. The CLI can load the same configuration in headless mode without Qt. Device enumeration and Windows-plugin bridging remain in progress.

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

Requirements for the default build: CMake 3.20+, a C++20 compiler, PipeWire development headers and pkg-config. Enabling VST3 hosting requires CMake 3.25+ and a network connection on the first configure unless you already have the SDK locally. The optional GUI additionally requires Qt 6 Widgets development files.

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

For single-configuration generators such as Unix Makefiles, CMake defaults the VST3-enabled build to Release when no build type is specified, as required by the Steinberg SDK. You can still choose Debug explicitly with `-DCMAKE_BUILD_TYPE=Debug`.

To build the optional Qt frontend (in addition to native VST3 support), install Qt 6 Widgets development files and configure with:

```sh
cmake -S . -B build-vst3-gui -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON
cmake --build build-vst3-gui
./build-vst3-gui/vst-host-gui
```

The GUI lets you add individual `.vst3` bundles or add separate plugin search directories. Search directories can be scanned again; discovered bundles are added to the ordered chain and can be reordered or removed independently. The chain entries can be dragged to reorder them (or use the Move up/Move down controls), and the selected plugin's editor can be opened. It also provides Load configuration and Save configuration for portable `.vsthost` files. The chain and audio routing/gain/latency settings are additionally saved between launches using the desktop settings store. It enumerates available PipeWire capture sources at startup and offers a Refresh control plus editable manual node-name entry (leave empty for the default microphone). The selected source is saved by its PipeWire node name, while the displayed label includes its current node id. Configure gain and requested latency, then start/stop the virtual microphone. Use **Open plugin editor** to embed the plugin's own editor. Native editor embedding currently requires the Qt `xcb` platform; on a Wayland desktop with XWayland installed, try `QT_QPA_PLATFORM=xcb ./build-vst3-gui/vst-host-gui`. Plug-ins that do not expose an X11-compatible editor will report an error. The command-line `vst-host` target is still built alongside the GUI.


The first configure downloads the SDK and its required source submodules; later builds reuse them in the CMake build directory. VSTGUI and the SDK's example plugins are disabled because this headless host does not need them. To use an SDK checkout you already have instead, pass `-DVST3_SDK_ROOT=/path/to/vst3sdk` with `-DVST_HOST_ENABLE_VST3=ON`. VST3 hosting stays optional, so the normal build does not need a network connection or the SDK. The SDK is distributed under the terms in the [official SDK repository](https://github.com/steinbergmedia/vst3sdk).

The application must run inside a logged-in desktop/user session with PipeWire available. By default it asks PipeWire to connect to the default microphone. To target a specific PipeWire node:

```sh
./build/vst-host --input NODE_ID_OR_NAME --name vst-host-mic --gain 1.0
```

Use `--config my-setup.vsthost` to load a configuration saved by the GUI and run it headlessly (no Qt process or desktop window is started). CLI options supplied after `--config` override scalar settings; repeated `--plugin` entries append to the configured chain. Use `--save-config my-setup.vsthost` to write the effective CLI settings before starting. Use `--gain 0.5` to attenuate, `--gain 2.0` to amplify, or `--bypass` to bypass the gain stage. With a VST3-enabled build, repeat `--plugin` to load individual native Linux VST3 audio effects, or use `--plugin-dir PATH` to scan a directory recursively for `.vst3` bundles. Multiple plugin directories can be supplied; discovered plugins are appended in sorted path order after individually specified plugins. For example:

```sh
./build-vst3/vst-host --input NODE_ID_OR_NAME --plugin /path/to/Effect.vst3 --name vst-host-mic
```

Each plugin instance currently supports its first main audio input/output bus with mono/stereo negotiation; the chain passes stereo audio between instances, adapting mono plugin layouts at each plugin boundary. `--latency-frames 128` is the default and requests a 128-frame PipeWire interval (about 2.67 ms at 48 kHz). If you hear dropouts, try `256` (5.33 ms), `512` (10.67 ms), or `1024` (21.33 ms). On shutdown, the host reports dropped and underrun frame counts as a basic troubleshooting diagnostic. This is a request, not a guarantee: actual latency depends on the PipeWire graph and the receiving application. The host also discards stale queued audio if the virtual microphone was suspended, rather than replaying a large backlog. Gain and latency are set at startup, not changed interactively. Stop with Ctrl+C.

The output source is intended to appear in applications' microphone/input-device lists as **VST Host Virtual Microphone** (or the name supplied with `--name`). This needs verification on the target distribution and PipeWire version; if the node is exposed as an application stream instead of a selectable source, the virtual-source publication strategy will need adjustment.

## Known limitations

- PipeWire device enumeration and selection are implemented in the GUI, but hotplug updates require pressing Refresh. The CLI still accepts `--input` as a PipeWire target id/name.
- Stereo F32 input is requested at 48 kHz. Compatibility with mono-only devices and other negotiated formats still needs testing.
- Plug-in-internal state persistence, generic parameter controls, meters, device enumeration is not implemented. Portable host configuration files store plugin paths and host audio settings, but not plugin-internal state. Embedded editor hosting currently supports X11/XWayland only; Wayland-native editor embedding is future work.
- The chain is prepared before the PipeWire real-time callback starts; its process path performs ordered in-place processing without chain allocation or locking. Real-world CPU use, cumulative plugin latency, and device compatibility still need measurement on a PipeWire system.

See [architecture notes](docs/architecture.md) and [roadmap](docs/roadmap.md).
