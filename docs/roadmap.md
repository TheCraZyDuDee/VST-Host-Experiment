# Roadmap

This page tracks remaining engineering work, not a historical development diary. For supported workflows, start with the [README](../README.md) and [Getting started](getting-started.md).

## Implemented

- PipeWire input → ordered VST effect chain → named virtual microphone, with an SDK-free headless mode.
- Qt 6 plugin browser, search, device selection, drag-to-reorder chain, native editors, Host Controls, live per-effect bypass, stereo input/output peak and RMS meters, clipping indicators, and audio buffer diagnostics.
- Native Linux VST3 and optional licensed VST2; Wine VST2/VST3 through vstbridge or yabridge; format labels and Windows directory filtering.
- Pinned microphone reconnection without falling back to another input, and nonfinite sample sanitization with separate input/output repair diagnostics.
- Per-instance plugin-state persistence, GUI settings, portable `.vsthost` files, and automated tests.

## Priorities

1. **Reliability:** better Wine bridge/plugin crash diagnostics, recovery paths, and device/session-manager edge cases.
2. **Performance validation:** measure end-to-end latency, buffer behavior, CPU use, and plugin-induced latency on representative hardware; consider UI-side graph timing.
3. **Controls:** consider live output-gain adjustment and further parameter automation without disrupting the audio thread.
4. **Compatibility:** expand testing across Linux distributions, plugins, and desktop environments; improve editor focus/resizing. Validate floating XWayland vendor editors under native Qt Wayland on real desktops.
5. **Genuine native VST3 Wayland editors:** implement the Steinberg VST3 3.8 `IWaylandHost` / `IWaylandFrame` protocol and host-side compositor integration for plug-ins supporting `kPlatformTypeWaylandSurfaceID` (tracked in [issue #30](https://github.com/TheCraZyDuDee/VST-Host-Experiment/issues/30)). Keep XEmbed/XWayland fallback for older plug-ins.

## Out of scope for now

MIDI instruments and dedicated multichannel/sidechain routing are not supported. Fully native Wayland plugin editor embedding is not implemented yet (see the compatibility priority above). VST2 licensing restrictions remain in force; the SDK is not distributed with this project.

See [Architecture](architecture.md) for implementation boundaries and [Troubleshooting](troubleshooting.md) for current limitations.
