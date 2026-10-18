# Architecture (proposed)

## Design constraints

1. The audio engine must be usable without a GUI.
2. The real-time processing path must not allocate memory, take contended locks, log synchronously, access files, or wait on UI/plugin-management work.
3. Device changes, plugin scanning, plugin loading, and configuration parsing happen outside the audio callback.
4. The engine should report failures clearly and recover without leaving a partially configured signal chain.
5. Idle operation should not poll aggressively. Prefer event-driven device/session APIs where available.

## Initial component boundaries

- **Application shell:** command-line options, configuration path, lifecycle and diagnostics.
- **Audio backend:** input/output stream setup, channel layout, sample-rate and buffer negotiation.
- **Processing graph:** ordered chain of processors, with explicit channel and latency handling.
- **Plugin adapter:** plugin discovery, loading, parameter/state management, and format-specific details.
- **Virtual output:** publish processed audio to a named PipeWire node that can be selected as a microphone in other applications.
- **Optional UI:** device selection, chain ordering, bypass, meters and configuration. The engine must not depend on Qt or any other UI library.

## Backend decision

PipeWire is the primary target because the required output is a named virtual audio source in a modern Linux desktop. The implementation should compare direct PipeWire integration with the JACK API running on PipeWire. Choose based on actual routing behavior, latency, supported distributions, and ease of creating a persistent named source—not assumption alone.

Avoid running separate capture and playback loops if one graph can handle the signal path. Minimize copies and use preallocated audio buffers. Any control-to-audio updates should be transferred through bounded, real-time-safe mechanisms.

## Plugin strategy

Implement and test native Linux plugins first. Keep a format-neutral processor interface so plugin hosting can be replaced or extended without changing device routing. Evaluate VST3 hosting options and licensing before committing to a framework or vendored SDK. VST2 support requires a separate licensing/distribution review.

Windows-only plugins are explicitly out of scope for the first working prototype. A later Wine-based compatibility layer would require its own helper process, diagnostics, and per-plugin compatibility tests; it must not be a prerequisite for native Linux use.

## UI and headless mode

The executable and audio engine should expose the same control/configuration interface regardless of whether a GUI is present. Qt 6 is a candidate for the optional desktop UI, but should not be linked into a headless build. Avoid adding the UI framework until a real engine slice exists and can be tested independently.
