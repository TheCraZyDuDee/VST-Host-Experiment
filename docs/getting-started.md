# Getting started

## Requirements

- Linux with PipeWire in a logged-in user session (WirePlumber is recommended for pinned-mic reconnection).
- C++20 compiler, `pkg-config`, PipeWire 0.3 development headers, CMake **3.20+** for the SDK-free CLI or **3.25+** for VST3.
- For the GUI: Qt 6 Widgets and a Qt platform plugin (such as `qt6-wayland` for native Wayland). X11 development files are optional; they enable embedded XEmbed plugin editors when running with `xcb`.
- VST3 support: Steinberg SDK (fetched automatically at first configure, or supplied via `VST3_SDK_ROOT`).
- VST2 support: a **user-provided, legally licensed legacy VST2.4 SDK** with `pluginterfaces/vst2.x/aeffectx.h`. VST2 remains off by default.

On Debian/Ubuntu, install typical dependencies:

```sh
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev qt6-base-dev qt6-wayland
# Optional, for embedding legacy plug-in GUIs via X11/XWayland:
sudo apt install libx11-dev
```

Check `cmake --version`: the distribution's CMake may be too old for VST3.

## Build and run

**GUI + VST3 (recommended):**

```sh
cmake -S . -B build -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON
cmake --build build --parallel 2
QT_QPA_PLATFORM=wayland ./build/vst-host-gui

# Optional: when a plug-in needs its own XEmbed interface, use X11/XWayland:
QT_QPA_PLATFORM=xcb ./build/vst-host-gui
```

**Add VST2 (requires an eligible legacy SDK):** configure the same build with:

```sh
cmake -S . -B build -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON \
  -DVST_HOST_ENABLE_VST2=ON -DVST2_SDK_ROOT="/path/to/licensed/vstsdk2.4"
cmake --build build --parallel 2
```

The repository never downloads, packages, or redistributes the discontinued VST2 SDK. Only enable VST2 hosting if you have the necessary license rights. For `__cdecl` errors from older VST2 headers on Linux, update to the current source; the compatibility shim is already included.

**Headless, SDK-free CLI (microphone and gain only):**

```sh
cmake -S . -B build-headless
cmake --build build-headless --parallel 2
./build-headless/vst-host --help
```

Builds with VST3/VST2 enabled also include the `vst-host` executable. To use a local VST3 SDK rather than fetching one, set `-DVST3_SDK_ROOT=/path/to/vst3sdk` together with `-DVST_HOST_ENABLE_VST3=ON`.

Run automated tests:

```sh
ctest --test-dir build --output-on-failure
```

## GUI workflow

1. Select a **Microphone / input**. **Default microphone** follows PipeWire's default. A specifically selected device is pinned by its stable `node.name`: if unplugged, the virtual mic outputs silence and reconnects to the same device when available. Click **Refresh** to update the input list.
2. Set **Virtual microphone name**, output gain and latency buffer (128/256/512/1024 frames). Try 256 or 512 frames if you hear dropouts.
3. In **Plugin library**, expand **Plugin folders and scanning**. Click **Add Linux directory…** for native VST2/VST3, or **Add Windows directory…** for Windows plugins, then **Scan all**. The folder section can be collapsed again to free workspace. Selecting a directory scans recursively; see [Windows plugins](windows-vst3-bridge.md) for how global bridge registrations are filtered.
4. The **Plugin library** and **Processing chain** are side by side and can be resized by dragging the divider. Search with **Ctrl+F**, select an effect, then click **Add selected to chain** (or double-click it). Drag to reorder or remove effects while audio is stopped; unavailable actions are disabled. You can also add a VST3 bundle directly, or a VST2 `.so` when built with VST2 support.
5. Click **Start audio**. Choose the published virtual microphone in the consuming app. Double-click an effect or click **Edit selected effect**. On Wayland, the dialog opens native Qt **Parameters (host controls)** without requiring X11. If you need a plug-in's custom XEmbed interface, relaunch with `QT_QPA_PLATFORM=xcb` under XWayland.
6. Toggle **Bypass selected effect** without stopping audio. To change devices, add/remove/reorder plugins, or change startup gain/latency, stop audio first.
7. Click **Stop audio** to save each plugin instance's state. Use **Save configuration…** to export it as a portable `.vsthost` file. **Load configuration…** restores a saved setup.

The GUI also stores its chain and settings between launches. Saving an exported configuration while audio runs is disabled because plugin-state snapshots require an orderly stop.

## Headless CLI

Use `--config` to load the same `.vsthost` file without the Qt frontend:

```sh
./build/vst-host --config my-setup.vsthost
```

With `--config`, the headless host loads **exactly the saved processing chain**, in the same order and with the same bypass/state settings as the GUI. Directory paths stored in the file are library search settings, not instructions to enable every discovered effect. Neither those directories nor the bridge's global registry are scanned or synchronized automatically during a `--config` launch.

Or specify an input, effects and virtual-mic name directly:

```sh
./build/vst-host \
  --input "alsa_input.usb-your-microphone" \
  --name processed-mic \
  --plugin "/path/to/NoiseReduction.vst3" \
  --plugin-bypassed "/path/to/Compressor.so" \
  --latency-frames 256
```

`--input` is a stable PipeWire **node name**; omit it to use the default input. `--plugin` can be repeated to build an ordered chain. `--plugin-bypassed` adds an initially bypassed instance. Use `--plugin-dir PATH` for native Linux discovery, and `--windows-plugin-dir PATH` for bridged Windows plugins. The CLI **only auto-appends** effects from directories explicitly supplied using `--plugin-dir` or `--windows-plugin-dir` on that invocation; a saved `.vsthost` file never implicitly activates all of its library directories.

**Directory auto-load is opt-in:** `--plugin-dir PATH` and `--windows-plugin-dir PATH` explicitly append plugins discovered under **those command-line roots only**, even when `--config` also contains saved directory paths. They can add significant CPU load or change your audio if unintended. The CLI logs every active plugin in order; compare that list to the GUI chain when diagnosing differences.

Other options: `--gain 0.0..4.0`, `--bypass` (output-gain bypass), and `--save-config FILE` (writes the effective settings **before** audio starts). Options following `--config` can override scalar values; repeated plugin flags append to the configured effects. Stop with Ctrl+C.

For state persistence, effect formats, and Windows setup, see [Plugins and configuration](plugins.md).
