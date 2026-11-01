# Roadmap

This page tracks remaining engineering work, not a historical development diary. For supported workflows, start with the [README](../README.md) and [Getting started](getting-started.md).

## Implemented

- PipeWire input → ordered VST effect chain → named virtual microphone, with an SDK-free headless mode.
- Qt 6 plugin browser, search, device selection, drag-to-reorder chain, native editors, Host Controls, live per-effect bypass, and audio buffer diagnostics.
- Native Linux VST3 and optional licensed VST2; Wine VST2/VST3 through vstbridge or yabridge; format labels and Windows directory filtering.
- Pinned microphone reconnection without falling back to another input.
- Per-instance plugin-state persistence, GUI settings, portable `.vsthost` files, and automated tests.

## Priorities

1. **Audio meters:** per-channel input/output peak/RMS and clipping indication, with bounded nonblocking transfer to the GUI.
2. **Reliability:** better Wine bridge/plugin crash diagnostics, recovery paths, and device/session-manager edge cases.
3. **Performance validation:** measure end-to-end latency, buffer behavior, CPU use, and plugin-induced latency on representative hardware.
4. **Controls:** consider live output-gain adjustment and further parameter automation without disrupting the audio thread.
5. **Compatibility:** expand testing across Linux distributions, plugins, and desktop environments; improve editor focus/resizing.

## Out of scope for now

MIDI instruments, dedicated multichannel/sidechain routing, and fully native Wayland plugin embedding are not supported. VST2 licensing restrictions remain in force; the SDK is not distributed with this project.

See [Architecture](architecture.md) for implementation boundaries and [Troubleshooting](troubleshooting.md) for current limitations.
