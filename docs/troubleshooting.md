# Troubleshooting

## Build and startup

| Symptom | Check |
| --- | --- |
| CMake says VST3 needs a newer version | Use **CMake 3.25+** when VST3 is enabled; the SDK-free build needs 3.20+. |
| CMake cannot find PipeWire or Qt | Install the corresponding development packages; see [Getting started](getting-started.md). X11 headers are optional and only needed to build XEmbed editor support. |
| VST2 SDK not found | Point `VST2_SDK_ROOT` to your licensed SDK root containing `pluginterfaces/vst2.x/aeffectx.h`; VST2 is off by default. |
| Legacy VST2 `__cdecl` / `aeffect.h` callback errors | Pull the latest source; the Linux compatibility shim should be included before the SDK headers. Do not edit your SDK. |
| Virtual microphone or selected input is missing | Ensure PipeWire is running in your logged-in user session. Inspect `wpctl status`, refresh GUI devices, and check the input node name. |

To diagnose a compiler failure, rebuild just the failing target with one job, then copy the **first compiler error**:

```sh
cmake --build build --target vst2-processor-test --parallel 1 --verbose
```

## Audio is silent or uses the wrong microphone

- With **Default microphone**, the host follows the system default. Select a named microphone in the GUI to **pin** it instead.
- If a pinned microphone is disconnected, the virtual mic **intentionally outputs silence** until that same PipeWire `node.name` returns. A numeric node ID is not a stable replacement for a device name.
- Make sure the consuming application uses the **virtual microphone** as its input. It must run inside the same PipeWire user session.
- Try starting without plugins, then add effects one at a time. A bypassed effect remains loaded but does not process audio.
- If the **nonfinite samples zeroed** counters increase, the input or an effect is returning NaN/Inf audio values. Try an empty chain, then add effects one at a time; avoid using plugins that repeatedly produce nonfinite output. The sanitization protects downstream audio but cannot prevent a faulty plugin from crashing its own process.
- If output drops out, increase the latency buffer from 128 to 256/512/1024 frames and watch the GUI's dropped/underrun/stale frame counters. **Input** peak/RMS indicates the selected microphone before effects; **Output** indicates the virtual microphone after processing and gain. If input moves but output is silent, bypass effects one by one to identify the stage. **CLIP** indicates a channel reached or exceeded 0 dBFS; reduce the relevant input/plugin gain. Meters display silence if capture stops for several polls and do not measure end-to-end latency. The GUI's **Host CPU** figure is CPU time of this process, not the combined usage of external Wine bridge processes. The active-effect reported latency is a plug-in declaration, not a measured end-to-end hardware/PipeWire delay. PipeWire controls actual graph latency; the setting is a request, not a guarantee.

## Headless mode sounds different or uses more CPU

A `.vsthost` configuration can contain **both** a selected processing chain and directories for browsing available plugins. Headless `--config FILE` now loads the saved chain **exactly**; it does not load every plugin in those directories or synchronize a global Wine bridge just because the folders are stored in the file.

If you add `--plugin-dir` or `--windows-plugin-dir` to the command, that explicitly **adds discovered plugins to the running chain**. This can substantially increase CPU usage or change the stereo output if one of those effects is configured for mono or other channel behavior. Read the printed `Plugin chain N:` lines to confirm the same plugin order and bypass settings as the GUI. The core GUI and CLI use the same stereo PipeWire engine.

If audio still differs with only `--config FILE`, try the same setup with every effect bypassed to distinguish plugin-specific output routing from PipeWire capture/source routing, and provide the `Plugin chain` startup lines.

## Plugin fails to load

- Check the format: VST3 uses a `.vst3` bundle; native/bridged VST2 uses a Linux `.so` wrapper. A Windows `.dll` is **not** directly loadable.
- VST2 requires the optional licensed SDK build. 32-bit native VST2 libraries cannot be loaded directly into a 64-bit host process.
- This is an **audio-effect** host: VST2 instruments with zero audio inputs, MIDI-only plugins and unsupported VST3 bus layouts are outside its current scope.
- Some effects expose sidechain inputs: the host supports additional VST2 input channels, but feeds those auxiliary inputs **silence**. It does not offer independent sidechain routing.
- If an effect fails after updating it, stop audio and use **Clear saved settings for selected effect** to try its default state again.

## Wine editor is blank, frozen, or ignores clicks

1. For the main UI on Wayland, install your distribution's Qt Wayland platform plugin (for example `qt6-wayland`) and run `QT_QPA_PLATFORM=wayland ./build/vst-host-gui`. The standard **Parameters (host controls)** editor is available natively.
2. For a plug-in's **custom XEmbed editor** in native Qt Wayland mode, ensure XWayland is running and `echo "$DISPLAY"` shows an X11 display. Build with optional X11 headers installed. After starting audio, select a loaded effect and choose **Edit selected effect**. Its vendor interface, when supported, opens in a separate XWayland window, not inside the Qt tab. The **Plugin UI** tab now includes **Show plugin window (XWayland)** to bring it forward; the button is disabled until an editor attaches. Run `QT_QPA_PLATFORM=wayland ./build/vst-host-gui --check-xembed-window` to check that an X11 companion window can be created, **not** that a vendor plugin can draw in it. Run `QT_QPA_PLATFORM=wayland ./build/vst-host-gui --check-plugin-ui-tab` to verify the Wayland dialog and tab without loading a plugin. To embed the editor *inside* the Qt dialog, use `QT_QPA_PLATFORM=xcb ./build/vst-host-gui`. If the plug-in UI remains blank, use **Parameters (host controls)**.
3. If a Wine GUI does not repaint after adjusting settings, testing DXVK is worthwhile; effectiveness varies by plugin and system.
4. Confirm your vstbridge/yabridge and Wine versions. For persistent editor issues, test the same plugin and bridge in another compatible VST host to isolate the cause.

## Windows plugins appear from unrelated folders

Bridges keep a **global** plugin registry. The host filters automatic discoveries against the Windows directories you configured, but **explicit plugins and existing chain entries remain visible** even if outside those roots.

Check `vstbridgectl list` (or `yabridgectl list`) for overlapping registered directories; multiple paths can generate wrapper aliases. The host hides equivalent originals without deleting wrappers. Avoid running `sync --prune` until you understand which other applications use those registrations. See [Windows plugins](windows-vst3-bridge.md).

## Plugin settings do not persist

Stop audio normally before exporting a `.vsthost` configuration. Settings are captured during orderly engine shutdown, **not continuously during audio processing**. Unsupported plugins may refuse snapshots; check the GUI/terminal warnings. State saved from a different plugin version can be incompatible. The exported base64 state is not encrypted.

## Reporting a problem

Include your Linux distribution, PipeWire/WirePlumber version, Wine and bridge versions (for Windows plugins), plugin name/format, build flags, the **first relevant error**, and reproduction steps. For audio problems, note the selected input and whether the host works with an empty plugin chain.
