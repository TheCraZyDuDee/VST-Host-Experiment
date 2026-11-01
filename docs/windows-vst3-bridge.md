# Windows VST3 bridging: vstbridge preferred, yabridge fallback

## Decision

**Prefer [vstbridge](https://github.com/rations/vstbridge) for Windows VST3
plugins**, with [yabridge](https://github.com/robbert-vdh/yabridge) as a
compatibility fallback. Both are optional external applications, not bundled,
downloaded, or installed by the VST Host Experiment.

The native host still uses the same C++ VST3 processor and PipeWire engine.
A Wine bridge produces Linux VST3 wrappers, which the host discovers and
loads through its existing processor chain. Wine and the plugin processes
remain external. Native Linux VST3 effects need neither bridge.

This changes the **bridge backend**, not the `.vsthost` file format: the
`windows_plugin_directory` keys still point to Windows plug-in directories.

## Why vstbridge

- vstbridge is an actively updated yabridge fork. Version **0.0.4**, released
  October 28, 2026, contains compatibility changes for recent Wine versions,
  including Wine 11.20, and multi-monitor editor input fixes.
- It specifically implements a Windows-side XEmbed coordinate correction
  for the failure in which a VST3 GUI renders but ignores mouse input.
- `vstbridgectl` supports the same `add`, `sync`, and `status` commands
  already used by our engine.
- The fork installs its VST3 wrappers into `~/.vst3/vstbridge/`, separate
  from yabridge's `~/.vst3/yabridge/`.
- Wine GUI interaction remains plugin-, Wine-, desktop-, and bridge-dependent.
  Compatibility with RNNoise and Supertone Clear still requires testing.

See:
- [vstbridge project](https://github.com/rations/vstbridge)
- [vstbridge 0.0.4 release](https://github.com/rations/vstbridge/releases/tag/v0.0.4)
- [yabridge changelog](https://github.com/robbert-vdh/yabridge/blob/master/CHANGELOG.md)

## Install and migration

Install vstbridge separately using the instructions and architecture-specific
release package in the upstream release. The project's installer normally
puts `vstbridgectl` under `~/.local/share/vstbridge/vstbridgectl`.
Do **not** rename yabridge's installed binaries or symlink its directories
into vstbridge: their runtime libraries and configuration files are separate.

VST Host Experiment finds the bridge control tool in this order:

1. A valid explicit `VSTBRIDGECTL` path or executable, if specified.
2. A valid explicit `YABRIDGECTL` path or executable, if specified.
3. `~/.local/share/vstbridge/vstbridgectl` or `vstbridgectl` on `PATH`.
4. `~/.local/share/yabridge/yabridgectl` or `yabridgectl` on `PATH`.

An explicit but invalid override is an error, not permission to silently use
a different bridge. The process uses `posix_spawnp` with an argument vector;
plugin directory paths are never shell-interpolated.

The GUI's single Linux/Windows directory list and portable configuration keys
are unchanged. On **Scan all**, Windows directories are registered with the
selected tool and synchronized in a background worker. Native wrappers appear
in the available-plugin browser with the `(WINE)` label. The GUI status
identifies the active bridge.

Existing chain entries pointing at `~/.vst3/yabridge/...` are migrated to the
equivalent path under the selected vstbridge wrapper directory **only if that
new path already exists**. The reverse migration is supported for an explicit
yabridge fallback. This preserves the order and number of effect instances.
If no equivalent wrapper exists, the original path is kept; users can
re-scan, confirm synchronization, and re-add the effect explicitly. CLI
chains containing explicit wrapper paths are migrated on startup when
Windows directories were configured and synchronized.

**Important:** Existing per-plug-in settings or state are not migrated by
this mechanism; this host currently saves plugin paths and audio settings,
not native VST3 plugin state. yabridge and vstbridge have distinct global
configuration and plugin wrapper directories. Removing a Windows search
directory from our UI does not unregister it from the external bridge.

## Runtime behavior

The bridge must be prepared before PipeWire audio starts. No bridge launch,
plugin scan, Wine initialization, filesystem traversal, or wrapper allocation
happens inside the real-time processing callback. The GUI starts only plugins
selected in the active chain; discovery never auto-activates every wrapper.
The CLI retains its documented directory-discovery/auto-loading behavior.

Status and preparation errors from the selected bridge are surfaced in the
GUI or CLI. Native Linux plug-ins remain unaffected if no Windows directories
are configured. The host does not download or install either bridge.

## Limitations and follow-ups

- The vstbridge 0.0.4 release was reviewed, not executed on the user's Linux
  workstation here. Test RNNoise and Supertone Clear with Wine 11.20.
- Runtime Wine process crashes, audio xruns, plugin state snapshots, editor
  resize semantics, and hot-swapping bridge backends need further validation.
- The host's UI is Qt6/X11 (or XWayland via `QT_QPA_PLATFORM=xcb`); there is
  no Wayland-native editor embedding.
- The host-provided **Parameters** tab remains a supported workaround when
  a plug-in's graphical editor ignores clicks, regardless of bridge.
- vstbridge and yabridge are GPL-licensed third-party applications. They
  remain external runtime dependencies; this repository does not embed
  or redistribute their code.
