# Architecture

VST Host Experiment is a C++20 PipeWire audio engine with an optional Qt 6 frontend. The CLI and GUI use the same processing, discovery and portable configuration code.

## Audio flow

```text
Selected microphone (PipeWire capture stream)
    ↓
Preallocated stereo single-producer/single-consumer ring buffer
    ↓
Ordered effect chain: VST2 / VST3 (per-instance live bypass)
    ↓
Output gain or gain bypass
    ↓
Named PipeWire virtual microphone (Audio/Source/Virtual)
```

The engine processes **stereo 32-bit float audio at 48 kHz**. Capture and virtual-source callbacks exchange audio through a fixed-capacity, nonblocking ring buffer. An underrun outputs silence; overflow drops incoming frames; stale frames can be skipped after a long delay. The GUI reports cumulative underrun, dropped and stale frame counts. Dedicated stereo level meters observe valid capture data before effects and output samples after the effect chain and gain (including generated silence on pinned-input disconnects). Peak release and RMS smoothing are block-size aware; cross-thread readings are independent atomic values, read on the existing Qt timer. No allocations or locks are introduced in realtime callbacks.

The host allocates plugin scratch buffers during preparation, not during each callback. It does not lock, scan files, load plugins, or update Qt widgets in the real-time callbacks. **Third-party plugins may have their own real-time safety limitations.**

## Input and device lifetime

With the default input, PipeWire selects the system's capture source. With an explicitly selected PipeWire `node.name`, the capture stream uses WirePlumber's `node.dont-fallback`, `node.linger` and `node.dont-move` properties. A PipeWire registry listener tracks that source across temporary node-ID changes.

When the named source disappears or capture is inactive, the output stream stays alive but produces silence; buffered pre-disconnect audio is discarded. On reconnection to the same named source, processing resumes. This requires compatible session-manager behavior.

## Processor and plugin boundaries

| Code | Responsibility |
| --- | --- |
| `src/audio/pipewire_engine.*` | Streams, lifecycle, input pinning, chain setup, diagnostics |
| `src/audio/stereo_ring_buffer.hpp` | Bounded lock-free capture/output transfer |
| `src/audio/stereo_level_meter.hpp` | Stereo peak/RMS levels and per-channel clipping event counts (atomic GUI snapshots) |
| `src/audio/processor_chain.hpp` | Ordered effects and atomic per-effect bypass |
| `src/audio/audio_processor.hpp` / `plugin_processor.hpp` | Common real-time DSP and plugin/editor contracts |
| `src/audio/vst3_processor.*` | VST3 loading, bus adaptation, editor, parameter/state handling |
| `src/audio/vst2_processor.*` | Optional legacy VST2 effect ABI, channel mapping, state |
| `src/audio/plugin_discovery.hpp` | Native plugin scanning and wrapper identity |
| `src/audio/windows_bridge_manager.hpp` | External Wine bridge registration/sync |
| `src/audio/windows_plugin_scope.hpp` | Restrict global bridge discoveries to selected Windows roots |
| `src/audio/config.hpp` / `plugin_state.hpp` | Portable settings and bounded binary state encoding |
| `src/gui/main.cpp` | Qt plugin browser, chain controls, device UI, native Qt Wayland/X11 host controls, optional X11 editor surface |

The GUI scans devices and plugin directories off the UI thread. It retains an explicitly selected processing chain. The headless CLI restores that same chain from `--config`, without auto-loading other effects from saved library directories; `--plugin-dir` and `--windows-plugin-dir` explicitly opt in to additional automatic effects. Wine bridge synchronization for explicit Windows CLI directory scans runs before audio starts, outside audio callbacks.

## Parameters and state

Plugin editor changes are sent toward DSP processing through bounded parameter-event queues. Per-effect bypass is read through an atomic flag, so toggling it does not rebuild the chain.

On orderly shutdown, the host disconnects PipeWire streams before requesting VST2/VST3 plugin-state snapshots. Each ordered instance owns a distinct snapshot. VST3 state uses component and controller streams; VST2 uses plugin chunks where supported or saved parameter values. The Qt settings store and the shared `.vsthost` format can restore these states on the next start.

## Build boundaries

`vst-host` is the headless executable. `vst-host-gui` adds Qt Widgets with native Wayland or X11 windows. Embedded plug-in-specific XEmbed editors require optional X11 headers at build time. On Qt `xcb`, they appear inside the host dialog. Under native Qt Wayland with a working XWayland `DISPLAY`, legacy XEmbed editors can appear in separate X11 companion windows; standard host parameter controls remain native Wayland. True native VST3 Wayland editor embedding requires the Steinberg 3.8 `IWaylandHost`/`IWaylandFrame` protocol and is not yet implemented. VST3 uses the pinned Steinberg SDK or a supplied checkout; VST2 is **disabled by default** and requires a user-provided, appropriately licensed legacy SDK. Neither Wine nor a bridge is bundled.

For setup and user workflows see [Getting started](getting-started.md); for format and configuration details see [Plugins](plugins.md).
