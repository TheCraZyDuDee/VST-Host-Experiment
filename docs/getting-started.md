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

## Install already-built executables (no sudo)

**First compile the program**, using the GUI + VST3 build instructions above. The installer will not configure or compile anything.

From the repository root, after your `build/` directory contains both executables:

```sh
bash scripts/install-user.sh
```

This script copies only the following program files into your user installation:

| File | Destination |
| --- | --- |
| Compiled GUI executable | `~/.local/bin/vst-host-gui` |
| Compiled headless executable | `~/.local/bin/vst-host` |
| Desktop application-menu entry | `~/.local/share/applications/vst-host-experiment.desktop` |

The launcher uses the installed executable's absolute path and lets Qt select Wayland or X11 normally. No `sudo`, package manager, compilation, CMake installation step or extra dependency installation is performed. Qt, PipeWire, Wine and third-party plugin dependencies still need to be available on the user's system.

For a different build directory, run `bash scripts/install-user.sh --build-dir build-gui`; for an alternative absolute user prefix, add `--prefix /your/absolute/path` to installation **and uninstallation**. The script refuses to overwrite unrelated desktop entries or unowned binaries.

### Uninstall the program

Use the **separate** uninstaller:

```sh
bash scripts/uninstall-user.sh
```

This removes the managed binaries and application-menu launcher plus a matching login-autostart entry, but **preserves your saved profiles and settings** by default.

To explicitly delete application configuration as well:

```sh
bash scripts/uninstall-user.sh --remove-config
```

The optional flag deletes only the app-specific Qt settings file (`$XDG_CONFIG_HOME/VSTHostExperiment/VSTHostExperiment.conf`, normally under `~/.config`) and UUID-named saved profiles under `$XDG_CONFIG_HOME/vst-host-experiment/profiles/`. It preserves unrelated files even in those directories, exported `.vsthost` files elsewhere, your plugin bundles and Wine bridge installations. The scripts never recursively erase `~/.config` or other shared user directories. An unrecognised desktop launcher cannot authorize removal of existing binaries.

## GUI workflow

1. Select a **Microphone / input**. **Default microphone** follows PipeWire's default. A specifically selected device is pinned by its stable `node.name`: if unplugged, the virtual mic outputs silence and reconnects to the same device when available. Click **Refresh** to update the input list.
2. Set **Virtual microphone name**, output gain and latency buffer (128/256/512/1024 frames). Try 256 or 512 frames if you hear dropouts.
3. In **Plugin library**, expand **Plugin folders and scanning**. Click **Add Linux directory…** for native VST2/VST3, or **Add Windows directory…** for Windows plugins, then **Scan all**. The folder section can be collapsed again to free workspace. Selecting a directory scans recursively; see [Windows plugins](windows-vst3-bridge.md) for how global bridge registrations are filtered.
4. The **Plugin library** and **Processing chain** are side by side and can be resized by dragging the divider. Search with **Ctrl+F**, select an effect, then click **Add selected to chain** (or double-click it). Drag to reorder or remove effects while audio is stopped; unavailable actions are disabled. You can also add a VST3 bundle directly, or a VST2 `.so` when built with VST2 support.
5. Click **Start audio**. Choose the published virtual microphone in the consuming app. Double-click an effect or click **Edit selected effect**. On native Wayland, **Parameters (host controls)** remain inside Qt. When an XWayland `DISPLAY` is available and the GUI was built with optional X11 support, a compatible plug-in's XEmbed editor opens in a **separate XWayland window** without restarting the host. Use the **Plugin UI** tab and its **Show plugin window (XWayland)** button if the companion window is behind the main app. The `QT_QPA_PLATFORM=xcb` launch option retains the older embedded-editor dialog.
6. Watch the compact **Audio levels** panel: Input L/R shows the microphone before effects; Output L/R shows the signal sent to the virtual microphone after effects and output gain. Each channel shows peak and smoothed RMS in dBFS; **CLIP** marks samples at/above digital full scale within the recent poll interval. These meters do not measure system-wide speaker volume or plugin processing latency. The separate **Performance** line reports this host process's CPU consumption (relative to one logical core, so it may exceed 100% if multiple threads are busy) and the latency declared by enabled effects. It does not include the CPU of external Wine bridge processes or actual PipeWire/device end-to-end latency. Toggle **Bypass selected effect** without stopping audio. To change devices, add/remove/reorder plugins, or change startup gain/latency, stop audio first.
7. Click **Stop audio** to capture each plugin instance's state. Use **Save profile** to create or update a named setup, then select it from the **Profile** dropdown. The **⋮** menu provides **Save profile as…**, **Rename**, **Delete**, **Import .vsthost as profile…**, and **Export current .vsthost…**. Loading a profile replaces the current chain/routing, so switching is disabled while audio runs. Use **Preferences…** from the same menu for login autostart settings.

The GUI also stores its last working chain and settings between launches. Explicit named profiles are `.vsthost` files stored in `$XDG_CONFIG_HOME/vst-host-experiment/profiles/` (normally `~/.config/vst-host-experiment/profiles/`), indexed by friendly names. A profile is only overwritten when you choose **Save profile**. Exported `.vsthost` files remain portable. Saving profiles and exports while audio runs is disabled because plugin-state snapshots require an orderly stop.

### Login autostart

Open **⋮ → Preferences…**, enable **Launch VST Host when I log in**, choose a **Login profile** from your saved presets and optionally enable **Start the virtual microphone automatically at login**. Autostart is off by default. An ordinary manual launch will not auto-start audio. The setting writes only `$XDG_CONFIG_HOME/autostart/vst-host-experiment.desktop` (usually `~/.config/autostart/`), never a system-wide service, and refuses to overwrite an unrelated entry of the same name. The login launcher uses the executable path from the currently running app; installing via `bash scripts/install-user.sh` first gives it a stable path. After moving/reinstalling to another path, reopen Preferences and save to refresh the launcher. A missing or deleted login profile prevents automatic audio startup rather than loading an unintended fallback.

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
