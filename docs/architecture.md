# Architecture

## Design constraints

1. The audio engine must be usable without a GUI.
2. The real-time processing path must not allocate memory, take contended locks, log synchronously, access files, or wait on UI/plugin-management work.
3. Device changes, plugin scanning, plugin loading, and configuration parsing happen outside the audio callback.
4. The engine should report failures clearly and recover without leaving a partially configured signal chain.
5. Idle operation should not poll aggressively. Prefer event-driven device/session APIs where available.

## Current audio prototype

The current prototype uses PipeWire's native C API and a pre-prepared ordered processor chain:

- A capture stream requests interleaved 32-bit float stereo audio at 48 kHz.
- A fixed-capacity single-producer/single-consumer ring buffer separates the capture callback from the virtual-source callback.
- Both streams request a configurable PipeWire node latency (256 frames by default at 48 kHz); PipeWire and client graph settings may override the effective interval.
- The source callback runs each VST3 effect in chain order, then applies fixed gain (or bypasses it), emits silence on underrun, and discards excessive stale queue backlog to avoid long delayed playback after suspension.
- The output node is configured with `media.class = Audio/Source/Virtual` and a configurable node name.

The ring buffer avoids locks and dynamic allocation in both callbacks. Producer overflow drops incoming frames; if the consumer falls far behind, it skips old queued frames and keeps a small scheduling cushion. Underruns are filled with silence. Latency still needs measurement on the target system.

## Limitations to verify

PipeWire format negotiation, mono devices, node exposure in different desktop environments, and reconnect behavior need testing on actual Linux/PipeWire systems. In particular, verify that the output appears as a selectable microphone source, rather than only as an application stream. If necessary, use a supported PipeWire virtual-source/loopback topology while retaining the processing engine.

## Processor interface\n\n`AudioProcessor` defines `prepare()` for non-real-time setup, `process()` for in-place interleaved float audio, `reset()`, and optional latency reporting. The callback-facing `process()` contract forbids blocking, allocation, logging, I/O, and exceptions. The current `GainProcessor` implements this interface and is prepared before PipeWire streams start. Processor configuration is currently startup-only; live parameter updates need a bounded control-transfer mechanism before they are exposed. Native plugin adapters must follow the same contract and prepare all plugin buffers/state outside the callback.\n\n## Component boundaries

- **Application shell:** command-line options, configuration path, lifecycle and diagnostics.
- **Audio backend:** input/output stream setup, channel layout, sample-rate and buffer negotiation.
- **Processing graph:** ordered chain of processors, with explicit channel and latency handling.
- **Plugin adapter:** plugin discovery, loading, parameter/state management, and format-specific details.
- **Virtual output:** publish processed audio to a named PipeWire source.
- **Optional UI:** device selection, chain ordering, bypass, meters and configuration. The engine must not depend on Qt or any other UI library.

## Plugin strategy

Implement and test native Linux plugins first. The initial VST3 adapter uses the Steinberg hosting SDK when `VST3_SDK_ROOT` is supplied at configure time; the SDK is optional and the default build remains SDK-free. It loads one audio-effect class from a selected bundle and currently supports the first stereo input/output bus. Plug-in discovery, multi-effect chains, controller/editor support, state persistence, and robust bus negotiation remain future work. The audio callback uses preallocated planar scratch buffers to adapt the host’s interleaved stereo format to VST3’s process API. Third-party plug-ins are not guaranteed to be real-time safe, and this initial adapter still requires build and runtime validation with real Linux effects. VST2 support requires a separate licensing/distribution review.

Windows-only plugins are out of scope for the first working prototype. A later Wine-based compatibility layer would require its own helper process, diagnostics, and per-plugin compatibility tests; it must not be a prerequisite for native Linux use.


## Portable host configurations

The CLI and optional Qt frontend share the line-oriented `.vsthost` format implemented in `src/audio/config.hpp`. It stores the PipeWire input target, virtual-source name, gain, requested latency, gain bypass, and an ordered list of native VST3 bundle paths. String values are quoted and escaped; repeated `plugin` keys preserve chain order. Unknown keys and invalid values fail loading rather than being silently ignored.

The GUI's Load/Save configuration actions import and export this format. The CLI accepts `--config FILE` to start the same setup without Qt, and `--save-config FILE` to export the effective options. This is the supported lightweight background/headless path; it still runs inside the user's PipeWire session and does not require a GUI process. The file does not yet include plugin-internal controller/component state or plugin directory search paths.


## Plugin search directories

The plugin scanner in `src/audio/plugin_discovery.hpp` recursively searches configured directories for Linux VST3 bundle directories (`*.vst3`). It stops recursion when a bundle is found so bundle-internal resources are not mistaken for separate plugins. Results are sorted for deterministic startup order. Explicit plugin paths remain first; directory discoveries are appended after them. Scanning and plugin loading happen before PipeWire streams start, never in a real-time callback. The Qt UI keeps a separate list of search directories and offers an explicit rescan action; discovered bundles are inserted into the editable processing chain. Search directories are stored in `.vsthost` files and desktop settings. The CLI uses repeated `--plugin-dir PATH` options.
