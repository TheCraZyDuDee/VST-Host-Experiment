# VST Host Experiment

A lightweight Linux-first real-time audio host for chaining audio plugins and exposing the processed signal as a named virtual microphone.

## Current status

**Prototype 0.7 development:** the command-line host supports ordered native Linux VST3 chains, portable configuration files, recursive plugin-directory scanning, and optional Windows VST3 discovery through Wine/vstbridge (with yabridge fallback). The optional Qt 6 frontend provides chain editing, PipeWire input selection, audio buffer diagnostics, and portable configuration import/export. The CLI can load the same configuration headlessly without Qt. Real-device audio validation and bridge crash isolation remain open.

## Goals

- Keep idle/background CPU and memory use low.
- Support native Linux VST3 plugins first; evaluate VST2 support separately after checking SDK and distribution constraints.
- Let users choose an input device, arrange a plugin chain, and select an output/virtual microphone.
- Keep the audio engine independent from the user interface so a headless mode remains possible.
- Support Windows-only VST3 plugins using an optional Wine/vstbridge compatibility path (or yabridge fallback); native Linux plugins remain the simplest and lightest option.

## Technical direction

- **Language:** C++20.
- **Build:** CMake.
- **Audio:** PipeWire-native API, using a capture stream and a named virtual source.
- **UI:** optional and separate from the engine. Qt 6 is a candidate, but it should not be a dependency of a headless build.
- **Plugin hosting:** native Linux formats first, behind a format-neutral processor interface.

The real-time path uses a bounded single-producer/single-consumer ring buffer between PipeWire callbacks. The callbacks do not take locks or allocate memory. Buffer underruns are filled with silence; overflow drops incoming frames rather than blocking the audio thread. The diagnostics distinguish missing input frames (underruns), producer overflow, and stale audio trimmed when the capture side gets far ahead. All three counters are cumulative and should be investigated if they keep increasing steadily after startup.

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

The GUI separates the available-plugin browser from the active processing chain. Add an individual `.vst3` bundle to the browser, or configure Linux and Windows VST3 search directories in one directory list and scan them to populate one available-plugin browser. Windows plugins appear with a `(WINE)` suffix; discovery prepares the chosen bridge's Linux-side wrappers on a background worker and requires Wine and vstbridge (preferred) or yabridge already installed. Select individual available plugins to add them to the processing chain explicitly, without automatically loading all scanned bundles on startup. The active chain can then be reordered or removed independently. The chain entries can be dragged to reorder them (or use the Move up/Move down controls), and the selected plugin's editor can be opened. It also provides Load configuration and Save configuration for portable `.vsthost` files. The chain and audio routing/gain/latency settings are additionally saved between launches using the desktop settings store. It enumerates available PipeWire capture sources at startup and offers a Refresh control plus editable manual node-name entry (leave empty for the default microphone). The selected source is saved by its PipeWire node name, while the displayed label includes its current node id. Configure gain and requested latency, then start/stop the virtual microphone. With audio running, the active processing-chain list stays selectable; double-click an effect or select it and click **Edit selected effect** to open its editor. The editor itself also has an **Edit effect** dropdown to switch directly between running instances without destroying the Qt editor window. You can switch between active instances without stopping the microphone, while adding, removing, and reordering effects remain disabled until audio stops. Editor parameter changes are transferred to the audio processor using a bounded non-blocking queue; preset changes that notify the host are also forwarded. Use **Edit selected effect** to embed the plugin's own editor. Native editor embedding currently requires the Qt `xcb` platform; on a Wayland desktop with XWayland installed, try `QT_QPA_PLATFORM=xcb ./build-vst3-gui/vst-host-gui`. Plug-ins that do not expose an X11-compatible editor will report an error. Native and Wine bridge editor input and focus depend on X11/XWayland and the plugin's own windowing implementation; test mouse, keyboard, and parameter response with representative plugins. **If a Wine plugin displays correctly but its controls do not move**, open the **Parameters (host controls)** tab in the editor. It exposes up to 512 standard VST3 parameters as sliders (including stepped parameters); changes are passed to the running DSP on the audio callback through the non-blocking parameter queue. Plug-in-specific graphical controls that are not exported as VST3 parameters cannot be represented here. For the native embedded editor itself, be aware of upstream Wine/yabridge window-position and mouse-input issues ([yabridge #409](https://github.com/robbert-vdh/yabridge/issues/409), [#506](https://github.com/robbert-vdh/yabridge/issues/506) and [Wine bug 59822](https://bugs.winehq.org/show_bug.cgi?id=59822)). Test the same plugin under an established yabridge-compatible host to distinguish plugin/Wine bugs from issues in this host. Wine's virtual desktop option may restore mouse clicks for affected setups but can misplace plug-in popups. The yabridge documentation also recommends DXVK for certain plug-ins whose GUI fails to repaint when interacted with. Runtime behavior is not verified by headless CI. The command-line `vst-host` target is still built alongside the GUI.


The first configure downloads the SDK and its required source submodules; later builds reuse them in the CMake build directory. VSTGUI and the SDK's example plugins are disabled because this headless host does not need them. To use an SDK checkout you already have instead, pass `-DVST3_SDK_ROOT=/path/to/vst3sdk` with `-DVST_HOST_ENABLE_VST3=ON`. VST3 hosting stays optional, so the normal build does not need a network connection or the SDK. The SDK is distributed under the terms in the [official SDK repository](https://github.com/steinbergmedia/vst3sdk).

The application must run inside a logged-in desktop/user session with PipeWire available. By default it asks PipeWire to connect to the default microphone. To target a specific PipeWire node:

```sh
./build/vst-host --input NODE_ID_OR_NAME --name vst-host-mic --gain 1.0
```

Use `--config my-setup.vsthost` to load a configuration saved by the GUI and run it headlessly (no Qt process or desktop window is started). CLI options supplied after `--config` override scalar settings; repeated `--plugin` entries append to the configured chain. Use `--save-config my-setup.vsthost` to write the effective CLI settings before starting. Use `--gain 0.5` to attenuate, `--gain 2.0` to amplify, or `--bypass` to bypass the gain stage. With a VST3-enabled build, repeat `--plugin` to load individual native Linux VST3 audio effects, use `--plugin-dir PATH` to scan a directory recursively for Linux `.vst3` bundles, or use `--windows-plugin-dir PATH` to register a Windows VST3 directory with Wine and the selected bridge. Windows directories are synchronized using vstbridge when installed (or yabridge as a fallback) before the audio stream starts, and the generated Linux-side wrappers are loaded into the same chain. Multiple plugin directories can be supplied; discovered plugins are appended in sorted path order after individually specified plugins. For example:

```sh
./build-vst3/vst-host --input NODE_ID_OR_NAME --plugin /path/to/Effect.vst3 --name vst-host-mic
```

Each plugin instance currently supports its first main audio input/output bus with mono/stereo negotiation; the chain passes stereo audio between instances, adapting mono plugin layouts at each plugin boundary. `--latency-frames 128` is the default and requests a 128-frame PipeWire interval (about 2.67 ms at 48 kHz). If you hear dropouts, try `256` (5.33 ms), `512` (10.67 ms), or `1024` (21.33 ms). On shutdown, the host reports dropped and underrun frame counts as a basic troubleshooting diagnostic. This is a request, not a guarantee: actual latency depends on the PipeWire graph and the receiving application. The host also discards stale queued audio if the virtual microphone was suspended, rather than replaying a large backlog. Gain and latency are set at startup, not changed interactively. Stop with Ctrl+C.

The output source is intended to appear in applications' microphone/input-device lists as **VST Host Virtual Microphone** (or the name supplied with `--name`). This needs verification on the target distribution and PipeWire version; if the node is exposed as an application stream instead of a selectable source, the virtual-source publication strategy will need adjustment.

### Duplicate Windows plugin entries

If a Windows VST3 appears twice after scanning, check whether vstbridge
registered both a parent directory and its `VST3` subdirectory. Each
registration can produce a separate wrapper path for the same Windows module
(for example `~/.vst3/vstbridge/rnnoise.vst3` and
`~/.vst3/vstbridge/VST3/rnnoise.vst3`). The host now identifies these
equivalent wrappers by their **actual original Windows module symlink**, not
by bundle path or name. It shows one available effect per original module;
separately chosen instances in the active chain are unaffected.

The bridge registry is shared with other DAWs, so the host will not
delete wrappers or unregister a directory automatically. It checks for
overlapping registered roots before adding any new directories and warns if
overlaps already exist. To inspect or manually clean old registrations,
see [Windows bridge duplicate-wrapper troubleshooting](docs/windows-vst3-bridge.md#duplicate-windows-vst3-plugins-and-overlapping-search-roots).
Start with `vstbridgectl list`; only use `rm` and `sync --prune` after
checking the effects on other hosts.

## Pinned microphone and hot-plug recovery

Selecting a specific microphone by its stable PipeWire **node name** now
pins the capture stream to **that device only**. Unlike the **Default
microphone** option, a selected input must not silently fall back to another
microphone when it is unplugged, suspended or temporarily unavailable.

On WirePlumber 0.5, the host sets `target.object`, `node.dont-fallback=true`,
`node.linger=true`, and `node.dont-move=true` on the PipeWire **capture
stream**. The `linger` property keeps its stream alive while the target is
missing; `dont-fallback` prevents WirePlumber from linking a different source.
The host also watches PipeWire source node registration by stable `node.name`
(the numeric object ID normally changes on reconnect). While missing or paused,
the **virtual microphone remains present but emits silence**, including when
plugins could otherwise produce audio from silence. Queued pre-disconnect audio
is discarded, and processing resumes automatically when the same node appears
again. The GUI reports when it is waiting and when the source has returned.

The **Default microphone** choice intentionally retains PipeWire's normal
fallback behavior. The input target cannot be changed during a running session;
stop and restart audio to select another source.

To test, select a USB microphone, start the virtual mic, unplug it while
another physical microphone is available, and confirm that the virtual mic is
silent and its node remains present. Reconnect the selected microphone and
confirm that audio resumes without restarting the host. A stable `node.name`
must be used for automatic reconnection; numeric PipeWire node IDs are not
persistent across unplug/replug.

This behavior depends on a WirePlumber linking policy that honors
`node.linger` (for example WirePlumber 0.5). Other PipeWire session managers
or outdated WirePlumber configurations may behave differently and require
separate testing.

## Optional VST2 effects (Linux and Windows via Wine)

The host can now process a mixed ordered chain of **VST2** and **VST3**
effects. Linux VST2 effects are loadable shared libraries (`.so` files).
Windows VST2 effects are `.dll` files which **vstbridge or yabridge**
convert to native Linux VST2 `.so` wrappers. vstbridge typically places
them under `~/.vst/vstbridge/`; VST3 wrappers remain under
`~/.vst3/vstbridge/`. The GUI shows `(VST2)` or `(VST3)`, followed by
`(WINE)` for bridged effects, in **one plugin library and chain**.

**VST2 hosting is explicitly opt-in because of its legacy licensing.**
Steinberg retired the VST2 SDK in October 2018; new VST2 host
distribution without the required pre-existing agreement is not allowed.
The VST2 SDK is *not* included, downloaded, or copied into this repository.
Only build or distribute a VST2-enabled executable where your legal rights
permit it. If you have a lawfully licensed local VST2.4 SDK containing
`aeffectx.h`, enable it with:

```sh
cmake -S . -B build-vst2-vst3 -DVST_HOST_ENABLE_VST2=ON \
  -DVST2_SDK_ROOT=/path/to/your/licensed/vst2sdk \
  -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON
cmake --build build-vst2-vst3 -j
QT_QPA_PLATFORM=xcb ./build-vst2-vst3/vst-host-gui
```

The default CMake configuration keeps VST2 disabled. If VST2 is disabled,
`.so` effects are not advertised as loadable in the GUI scan, and trying
to load one explicitly reports the missing VST2 support. The VST3-only
build is unaffected.

**Legacy SDK compile compatibility:** Some unmodified VST2.4 SDK releases
define `VSTCALLBACK` as the MSVC keyword `__cdecl` even when compiling on
Linux. GCC/Clang do not recognize that keyword natively, producing errors
in `aeffect.h` such as `expected ')' before '*'` and follow-up errors
about missing `AEffect::dispatcher`. The host provides a compatibility
definition before including `aeffectx.h` on Linux; do **not** modify the
licensed SDK headers. If you encountered this issue, pull the latest
PR #21 branch and rebuild the `vst2-processor-test` target before
attempting the full GUI build.

When VST2 is enabled, **Add VST2 .so…** picks a native Linux VST2 library;
scanning a Linux plugin directory also finds `.so` effects while excluding
`.so` files inside VST3 bundles. Add a Windows VST2 `.dll` directory via
**Add Windows directory…**; syncing generates `.so` wrappers which appear
in the same library as the VST3 bundles. Centralized and inline VST2 bridge
wrappers are supported. Load the generated `.so`, not the Windows `.dll`,
into the host chain. The headless CLI also supports `--plugin /path/plugin.so`.

The VST2 adapter handles 32-bit floating-point replacing effects with mono
or stereo inputs/outputs, a basic embedded editor through X11 and
host-parameter sliders. Editor behavior depends on the plugin and Wine
bridge; classic VST2 plugins may require periodic `effEditIdle` callbacks.
Plugin state is saved using VST2 bank chunks when supported, otherwise
a bounded parameter snapshot. Saved states follow each plugin instance
individually through order changes and portable `.vsthost` files.

**Limitations:** VST2 synthesizers without audio input, sidechains, more
than two audio channels, 32-bit native Linux .so modules inside a 64-bit
process, plugin-specific offline processing, and reliable GUI embedding
on native Wayland are not supported. Run real desktop and Wine regression
tests before relying on a VST2 plugin in production. Automated CI without
the legacy SDK exercises the VST2-disabled path and format discovery only.

## Per-plugin VST3 settings and preset persistence

After you change a VST3 effect (including an effect hosted through
vstbridge/Wine), stop audio normally with **Stop audio**. The host now
serializes the effect's **component/DSP state** and, when the plugin supports
it, its **editor/controller state**. Each instance in the processing chain
gets its own snapshot, even when you load two copies of the same effect.

On the next start, the host restores these states **before enabling audio
processing**. Snapshots are saved in the Qt desktop settings after each
orderly shutdown; the active-chain label marks effects with
`[saved settings]`. Dragging effects to reorder them keeps each state with
its own chain entry. **Clear saved settings for selected effect** resets that
instance to the plugin's defaults on its next start.

To create a portable setup containing current plugin settings: stop audio
first, then use **Save configuration…**. The `.vsthost` format now accepts
`plugin_component_state = "…"` and `plugin_controller_state = "…"`
(base64 binary snapshots) after each corresponding `plugin = "..."` entry.
Older `.vsthost` files with only plugin paths and bypass values remain
compatible. Loading a portable configuration restores its per-instance
snapshots; the CLI also loads them with `--config`. Saving config from the
CLI **before** processing cannot capture subsequent changes made during
that session, but will preserve any snapshots already loaded.

**The VST3 specification has separate component and controller state.**
The host captures snapshots only after PipeWire processing callbacks have
stopped, never on the real-time audio thread. If a plugin rejects or cannot
provide a snapshot, the GUI reports a warning and preserves any previously
saved state for that instance. An incompatible saved snapshot can prevent
that plugin from starting; use **Clear saved settings** to load it with
defaults again.

Snapshots are limited to 16 MiB per component and per controller. Some
plugins store licensing, paths, or personally identifying configuration in
their state. Base64 is **not encryption**; treat exported `.vsthost` files as
potentially sensitive and avoid publishing them publicly. Snapshot support
must still be validated against real native and Wine VST3 plugins, especially
ones with extensive presets or asynchronous editor state.

## Live per-effect bypass

Select an effect in the **Processing chain** and check **Bypass selected
effect (can change while audio runs)** to temporarily skip that effect.
The effect remains loaded and prepared, so it can be re-enabled immediately
without restarting the microphone or changing the processing order.
Bypassed effects are marked `[BYPASSED]` in the chain. The bypass toggle
uses an atomic flag read by the audio callback: no reallocation, file I/O,
or blocking synchronization occurs in the real-time processing path.

The selected bypass states are saved in the desktop settings and portable
`.vsthost` files. They follow individual plugin instances if you reorder
the chain; different instances of the same VST3 can be bypassed independently.
Configuration files add an optional `plugin_bypassed = true` line after the
corresponding `plugin = "..."` line. Older files without this key work
unchanged, with every effect enabled by default. The CLI can start a
disabled instance explicitly:

```sh
./build-vst3/vst-host --plugin /path/to/Effect.vst3 --plugin-bypassed /path/to/Another.vst3
```

Bypassing a latency-producing effect may cause an audible timing change.
It does **not** reset the plugin's internal state or compensate for latency,
and the host does not currently support changing the chain order or plugin
loading while audio is running.

## Windows VST3 bridging (vstbridge preferred)

Install Wine and [vstbridge](https://github.com/rations/vstbridge) separately.
For modern Wine, use a compatible [vstbridge release](https://github.com/rations/vstbridge/releases)
(for example v0.0.4, released October 28, 2026). It includes a fix for embedded
Windows VST3 editors that draw correctly but do not respond to mouse clicks.
This is a candidate fix for RNNoise and Supertone Clear under Wine 11.20,
not a guarantee until those plugins are tested on a real desktop.

The host searches for `~/.local/share/vstbridge/vstbridgectl` and
`vstbridgectl` on `PATH`; it **prefers vstbridge** whenever both bridges are
installed. If vstbridge is absent, the existing yabridge integration still
works. You can select a particular executable explicitly:

```sh
VSTBRIDGECTL="$HOME/.local/share/vstbridge/vstbridgectl" ./build-vst3-gui/vst-host-gui
# To deliberately keep using legacy yabridge:
YABRIDGECTL="$HOME/.local/share/yabridge/yabridgectl" ./build-vst3-gui/vst-host-gui
```

The GUI keeps the same **Add Windows directory** and **Scan all** controls.
The selected bridge registers each directory, runs `sync`, and scans the
resulting native wrappers. vstbridge writes VST3 wrappers to
`~/.vst3/vstbridge/`; yabridge uses `~/.vst3/yabridge/`. You do not need to
rename or manually symlink either directory. When vstbridge's equivalent
wrapper exists, the GUI automatically updates saved processing-chain entries
that still point to yabridge, preserving their order and repeated instances.
If a wrapper has not been synchronized, the old path is retained instead of
silently replacing it with a nonexistent one. Rescan or re-add that plugin
from the available browser.

Existing `.vsthost` files and Windows search-directory settings remain
compatible, and the Host Controls fallback is still available. The bridge
is not downloaded or installed automatically. For details, see
[Windows VST3 bridge architecture and migration](docs/windows-vst3-bridge.md).

### Fixing Windows plugin GUI rendering with DXVK

If a Windows VST3 effect **accepts mouse input and audibly changes its
processing, but the graphical sliders only visually refresh after switching
tabs or reopening the editor**, check Wine's graphics backend before
modifying the host. Installing and configuring **DXVK** in the plugin's
Wine prefix resolved this behavior in desktop testing with **RNNoise and
Supertone Clear under Wine 11.20 and vstbridge**.

DXVK requires working Vulkan drivers and is an external Wine dependency.
Back up the correct Wine prefix before installing it. With `winetricks`,
a typical command for a **specific** prefix is:

```sh
WINEPREFIX="/path/to/plugin-wine-prefix" winetricks -q dxvk
```

Do not assume every plugin uses `~/.wine`: the bridge can launch plugins
under different prefixes. DXVK is not installed or changed automatically
by VST Host Experiment. The experimental periodic-redraw workaround was
**not merged** because the underlying rendering issue was resolved by
configuring Wine correctly.

### Plugin browser and startup feedback

The GUI displays readable plugin names instead of full bundle paths (hover
to inspect a path), includes a name/path filter in the available-plugin
browser, and numbers the active chain in processing order. Drag-and-drop
reordering is saved immediately, not just on the next audio start.
If plugin loading or PipeWire startup fails, the status area shows the
actual error, including the failed plugin path when available.

## Known limitations

- Windows VST3 support is optional and requires Wine plus a separately installed bridge: [vstbridge](https://github.com/rations/vstbridge) (preferred) or yabridge (fallback). The host runs the selected tool's `add`, `sync`, and `status` commands; it does not install either bridge. Each bridge's plugin-directory configuration is shared with other hosts, so removing a directory from this app does not unregister it from the bridge.
- PipeWire device enumeration and selection are implemented in the GUI, but hotplug updates require pressing Refresh. The CLI still accepts `--input` as a PipeWire target id/name.
- Stereo F32 input is requested at 48 kHz. Compatibility with mono-only devices and other negotiated formats still needs testing.
- Plug-in-internal state persistence and audio meters are not implemented. Generic host parameter controls are available while a VST3 editor is attached, but do not substitute for plug-in-specific controls. Portable host configuration files store plugin paths and host audio settings, but not plugin-internal state. Embedded editor hosting currently supports X11/XWayland only; Wayland-native editor embedding is future work.
- The chain is prepared before the PipeWire real-time callback starts; its process path performs ordered in-place processing without chain allocation or locking. Real-world CPU use, cumulative plugin latency, and device compatibility still need measurement on a PipeWire system.

See [architecture notes](docs/architecture.md) and [roadmap](docs/roadmap.md).
