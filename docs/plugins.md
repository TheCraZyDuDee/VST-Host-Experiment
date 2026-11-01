# Plugins and configuration

## Supported formats

| Plugin | On disk | Requirements |
| --- | --- | --- |
| Linux VST3 | `.vst3` directory | VST3-enabled build |
| Linux VST2 | `.so` library | VST2-enabled build with licensed legacy SDK |
| Windows VST3 | `.vst3` directory, wrapped as Linux VST3 | Wine + vstbridge (preferred) or yabridge; VST3-enabled build |
| Windows VST2 | `.dll`, wrapped as Linux `.so` | Wine + bridge; VST2-enabled build |

Windows DLLs are **not** loaded directly: point the host at a Windows directory and let the bridge produce native Linux wrappers. Native Linux plugins do not require Wine. Browser/chain entries show `(VST2)` / `(VST3)` and `(WINE)` where appropriate.

The host is for **audio effects**, not MIDI instruments. The main audio path is stereo float at 48 kHz. VST3 effects use their first negotiated main audio bus. VST2 effects may expose up to 64 input/output channels: the microphone feeds the first pair, extra inputs (including sidechains) get silence, and only the first output pair reaches the virtual microphone.

## Editing and bypass

While audio runs, select an effect and open its GUI by double-clicking or using **Edit selected effect**. The editor's instance selector can switch between running effects. On X11/XWayland, plugins with compatible native editors appear embedded; on Wine, drawing/input behavior also depends on Wine and the bridge.

The **Parameters (host controls)** tab exposes up to 512 standard plugin parameters, useful when a plugin has no usable native editor. Not every custom GUI setting is exposed as a parameter.

**Bypass selected effect** works live without unloading the plugin. Adding, removing and reordering effects requires stopping audio. The gain-stage **Bypass output gain** is a separate setting.

## Saving effect settings

Each chain entry owns a separate plugin-state snapshot, including when the same plugin appears twice. On normal **Stop audio**, the host captures VST3 component/controller state or VST2 bank-chunk/parameter state **after** audio callbacks stop. On the next start, it restores settings before processing begins.

- Desktop settings persist the chain, plugin states, device, gain and latency between ordinary launches.
- **Save configuration…** exports the current setup and state to a portable `.vsthost` file. Stop audio first to capture the latest edits.
- **Clear saved settings for selected effect** resets only that instance on its next start.
- If a plugin cannot provide a snapshot, the host reports a warning and retains any previously captured state. A state from an incompatible plugin version may need to be cleared.

Plugin snapshots have a 16 MiB limit per component/controller part. They are opaque, **not encrypted**, and may contain license or personal settings. Treat exported configurations accordingly.

## Portable `.vsthost` files

The GUI and headless CLI share a line-oriented text format:

```ini
# Example: paths and names must match your system
input = "alsa_input.usb-your-microphone"
output_name = "processed-mic"
gain = 1
latency_frames = 256
bypass = false

plugin = "/home/user/.vst3/vstbridge/NoiseReduction.vst3"
plugin_bypassed = false
plugin = "/home/user/.vst/vstbridge/Compressor.so"
plugin_bypassed = true

plugin_directory = "/home/user/.vst3"
windows_plugin_directory = "/home/user/Plugins/Windows/VST2"
```

Repeating `plugin` preserves chain order; `plugin_bypassed` applies to the preceding plugin. Optional `plugin_component_state` and `plugin_controller_state` keys follow a plugin entry when saved settings exist (base64-encoded binary). Directory keys can repeat. Older files without state/bypass keys remain valid. Invalid or unknown keys cause a clear load error.

Load from the CLI with `./build/vst-host --config my-setup.vsthost`. The CLI's `--save-config` exports **before** processing, so it cannot capture edits made later in that session.

## Discovery behavior

- **GUI:** scanned plugins populate the **Available plugins** library; they are **not** automatically added to the active chain. Search by name/path.
- **Headless CLI:** paths from repeated `--plugin-dir` and `--windows-plugin-dir` options are automatically appended to the chain (sorted after explicitly specified plugins).
- Equivalent wrappers of the same Windows module appear once in the library; intentionally repeated effect instances in an active chain remain separate.
- Windows discovery respects the selected Windows source folders even when the bridge is globally registered with other directories. See [Windows plugins](windows-vst3-bridge.md).

For build commands and usage, see [Getting started](getting-started.md). For editor problems and audio issues, see [Troubleshooting](troubleshooting.md).
