# Architecture

## Design constraints

1. The audio engine must be usable without a GUI.
2. The real-time processing path must not allocate memory, take contended locks, log synchronously, access files, or wait on UI/plugin-management work.
3. Device changes, plugin scanning, plugin loading, and configuration parsing happen outside the audio callback.
4. The engine should report failures clearly and recover without leaving a partially configured signal chain.
5. Idle operation should not poll aggressively. Prefer event-driven device/session APIs where available.

## Current audio prototype

The current prototype uses PipeWire's native C API:

- A capture stream requests interleaved 32-bit float stereo audio at 48 kHz.
- A fixed-capacity single-producer/single-consumer ring buffer separates the capture callback from the virtual-source callback.
- The source callback reads into two pre-created mono output ports, applies a fixed gain (or bypasses it), and emits silence on underrun.
- The output node is configured with `media.class = Audio/Source/Virtual` and a configurable node name.

The ring buffer avoids locks and dynamic allocation in both callbacks. On overflow it drops newest incoming frames; on underrun it emits silence. Counters are maintained for later diagnostics.

## Limitations to verify

PipeWire format negotiation, mono devices, node exposure in different desktop environments, and reconnect behavior need testing on actual Linux/PipeWire systems. In particular, verify that the output appears as a selectable microphone source, rather than only as an application stream. If necessary, use a supported PipeWire virtual-source/loopback topology while retaining the processing engine.

## Component boundaries

- **Application shell:** command-line options, configuration path, lifecycle and diagnostics.
- **Audio backend:** input/output stream setup, channel layout, sample-rate and buffer negotiation.
- **Processing graph:** ordered chain of processors, with explicit channel and latency handling.
- **Plugin adapter:** plugin discovery, loading, parameter/state management, and format-specific details.
- **Virtual output:** publish processed audio to a named PipeWire source.
- **Optional UI:** device selection, chain ordering, bypass, meters and configuration. The engine must not depend on Qt or any other UI library.

## Plugin strategy

Implement and test native Linux plugins first. Keep a format-neutral processor interface so plugin hosting can be replaced or extended without changing device routing. Evaluate VST3 hosting options and licensing before committing to a framework or vendored SDK. VST2 support requires a separate licensing/distribution review.

Windows-only plugins are out of scope for the first working prototype. A later Wine-based compatibility layer would require its own helper process, diagnostics, and per-plugin compatibility tests; it must not be a prerequisite for native Linux use.
