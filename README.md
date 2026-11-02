# VST Host Experiment

A Linux PipeWire host that processes a microphone through an ordered chain of **VST2/VST3 effects** and publishes the result as a **virtual microphone**. Use the Qt desktop app or run the same setup headlessly.

Supports native Linux plugins and Windows plugins through [vstbridge](https://github.com/rations/vstbridge) or [yabridge](https://github.com/robbert-vdh/yabridge) and Wine.

## Quick start

Requires a Linux desktop session with PipeWire, a C++20 compiler, CMake **3.25+** for VST3, Qt 6 Widgets, and `pkg-config` / PipeWire development headers. On Wayland install the Qt Wayland platform plugin; X11 development libraries are optional for legacy XEmbed editor windows.

On Debian/Ubuntu, install the development packages (use a recent enough CMake):

```sh
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev qt6-base-dev qt6-wayland
# Optional: build X11/XWayland plug-in editor embedding
sudo apt install libx11-dev
```

**Compile the program first:**

```sh
cmake -S . -B build -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON
cmake --build build --parallel 2
QT_QPA_PLATFORM=wayland ./build/vst-host-gui

# Optional: use X11/XWayland for embedded plug-in vendor editors
QT_QPA_PLATFORM=xcb ./build/vst-host-gui
```

**Install the already-compiled program for your user:**

```sh
bash scripts/install-user.sh
```

The install script **only copies the compiled** `vst-host-gui` and `vst-host` executables to `~/.local/bin` and creates the application-menu entry in `~/.local/share/applications`. It never compiles, configures, downloads, or installs dependencies, and needs no `sudo`. If you compiled in another directory, use `--build-dir build-gui`.

**Uninstall with a separate script:**

```sh
bash scripts/uninstall-user.sh                 # Keep saved profiles/settings
bash scripts/uninstall-user.sh --remove-config # Also delete app-owned configuration
```

The optional configuration removal is limited to this program's Qt settings and UUID-named saved profiles in its XDG configuration directory. It does **not** remove plug-ins, Wine bridges, exported profiles stored elsewhere, or unrelated files. See [Getting started](docs/getting-started.md) for exact paths.

The first build fetches the pinned Steinberg VST3 SDK unless `-DVST3_SDK_ROOT=/path/to/vst3sdk` is provided. The Qt GUI runs natively under Wayland, including standard plugin parameter controls. With X11/XWayland available, compatible third-party XEmbed editors open in a separate window while the Qt host stays on native Wayland. The editor dialog's **Plugin UI** tab provides a button to bring that companion window forward. Running under Qt xcb still embeds those editors inside the dialog.

**VST2 is optional.** It requires a **locally provided, appropriately licensed legacy VST2.4 SDK**; the repository does not ship or fetch it. Add `-DVST_HOST_ENABLE_VST2=ON -DVST2_SDK_ROOT=/path/to/vst2sdk` to the CMake configuration to enable VST2 effects.

See [Getting started](docs/getting-started.md) for full build options and the CLI.

## Using the GUI

1. Choose a **Microphone / input** (or leave **Default microphone** selected) and set the virtual microphone name.
2. In the **Plugin library**, expand **Plugin folders and scanning** and use **Add Linux directory…** or **Add Windows directory…**, then **Scan all**. Collapse it again to make more room for effects. Alternatively, add an individual plugin bundle.
3. Use the resizable, side-by-side **Plugin library** and **Processing chain** panels. Search with **Ctrl+F**, then double-click an available effect (or use **Add selected to chain**). Drag to reorder while stopped; each entry identifies its format, e.g. `(VST2)` or `(VST3) (WINE)`.
4. Click **Start audio** and select the virtual microphone in Discord, OBS, or another app. Double-click an effect to edit it. **Bypass selected effect** works while audio runs.
5. Click **Stop audio** to capture plugin settings. Use **Save** to store the current chain as a named preset; switch from the compact **Profile** dropdown. The **⋮** menu contains Save as, Rename, Delete, Import/Export `.vsthost`, **Preferences**, **Help**, and **About**. Audio-buffer and CPU details live behind a collapsible **Diagnostics** section. Preferences can optionally show a system tray icon, launch at login with a chosen saved profile, start audio automatically, or open hidden in the tray.

Selecting a specific microphone **pins that input**: if it disconnects, the virtual mic stays available but silent until the same PipeWire node returns. The **Default microphone** choice continues to follow the system default.

**Optional system tray:** Enable **⋮ → Preferences → Show a system tray icon**. When supported by the desktop, closing the main window hides it while audio continues. Click the tray icon or use its menu to show the window; choose **Quit** to exit fully. Under login preferences, **Start hidden in the system tray** is a separate opt-in. If the desktop cannot provide a usable tray (including some GNOME configurations), login startup opens visibly to avoid a stranded process.

Plugin settings, chain order, and host settings are retained between normal launches. Saved named profiles are separate `.vsthost` files under the user configuration directory (`~/.config/vst-host-experiment/profiles/` by default). Stop audio before saving a profile or portable configuration to include the latest plugin state. Autostart is off unless explicitly enabled in Preferences.

## Features and limits

- Ordered native/Wine VST2 and VST3 effect chains, plugin browser/search, per-effect live bypass, built-in parameter controls, and state persistence.
- PipeWire virtual microphone, stable input selection, hot-plug recovery, adjustable gain/latency, live stereo input/output peak and RMS meters with clipping indicators, process CPU and active-effect reported latency, and audio buffer diagnostics, including detection/replacement of nonfinite plugin samples.
- Headless `vst-host` executable with `--config`, direct plugin paths, and recursive directory discovery.
- **Current limits:** effects only (not VST2 instruments/MIDI), stereo microphone routing, no live add/remove/reorder during processing, and vendor-specific XEmbed editor windows require an X11/XWayland display, but can open alongside the native Wayland Qt host. Full native VST3 Wayland editor embedding is a separate workstream. Third-party plugin behavior varies.

## Documentation

| Guide | Contents |
| --- | --- |
| [Getting started](docs/getting-started.md) | Build, run, GUI workflow, CLI |
| [Plugins and configuration](docs/plugins.md) | VST2/VST3, plugin state, `.vsthost` files |
| [Windows plugins](docs/windows-vst3-bridge.md) | Wine, vstbridge/yabridge, directory scoping |
| [Troubleshooting](docs/troubleshooting.md) | No audio, Wine editors, scanning, latency |
| [Architecture](docs/architecture.md) | PipeWire streams, DSP chain, thread boundaries |
| [Roadmap](docs/roadmap.md) | Implemented capabilities and remaining work |

Run tests with `ctest --test-dir build --output-on-failure`. The VST2-enabled path requires local SDK/hardware testing; default CI does not bundle a legacy SDK.
