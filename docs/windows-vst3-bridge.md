# Windows VST2/VST3 plugins (Wine bridge)

The host uses [vstbridge](https://github.com/rations/vstbridge) by default, or [yabridge](https://github.com/robbert-vdh/yabridge) as a fallback, to load Windows VST2/VST3 effects through Wine. These are **external tools**; the host does not install, download, or bundle them.

## Setup

1. Install Wine and a bridge by following its upstream instructions. Verify that `vstbridgectl` (or `yabridgectl`) runs.
2. In the GUI, use **Add Windows directory…** to choose a Windows plugin folder and click **Scan all**.
3. Select its generated effects from **Available plugins** and add them to the chain. They show `(VST2) (WINE)` or `(VST3) (WINE)`.

Windows VST2 requires a build with **VST2 enabled**; VST3 requires VST3 hosting. See [Getting started](getting-started.md) for build options and [Plugins](plugins.md) for format support.

The bridge generates native Linux wrappers, typically under:

| Bridge | VST2 wrappers | VST3 wrappers |
| --- | --- | --- |
| vstbridge | `~/.vst/vstbridge/` | `~/.vst3/vstbridge/` |
| yabridge | `~/.vst/yabridge/` | `~/.vst3/yabridge/` |

Some installations use **inline VST2 wrappers**, placing a generated `.so` beside the original `.dll`. The host supports centralized and inline layouts.

## Which bridge is used?

An explicit `VSTBRIDGECTL` path takes priority, followed by an explicit `YABRIDGECTL` path. Otherwise the host looks for vstbridge in its usual installation directory or `PATH`, then yabridge. An invalid explicit override is an error, not a reason to silently switch backend.

For example:

```sh
VSTBRIDGECTL="$HOME/.local/share/vstbridge/vstbridgectl" \
  QT_QPA_PLATFORM=xcb ./build/vst-host-gui
```

The host registers selected Windows folders with the chosen bridge (avoiding redundant nested registrations), runs its `sync` command **before audio starts**, and discovers the Linux wrappers. Saved wrapper paths are migrated between bridges only when an equivalent wrapper exists; other chain entries remain unchanged.

## Directory selection and duplicates

**The bridge maintains a global directory registry**, shared by other audio applications. `sync` processes all its registered roots. **VST Host Experiment nevertheless shows only automatically discovered Windows plugins whose original DLL/VST3 module is inside a Windows directory selected in this app.** Selecting `Windows/VST2` excludes the sibling `Windows/VST3`; selecting their common parent includes both recursively.

A plugin you added manually or already put in the active chain remains selected even if its source directory is later removed from this app. This is intentional.

Registering overlapping roots (for example, both `Windows` and `Windows/VST3`) can create duplicate wrapper bundles. The host identifies aliases by their original Windows module and shows only one available-plugin entry; it does **not** remove existing wrapper files or change the bridge's shared registry.

To inspect registered directories:

```sh
vstbridgectl list
# Or: yabridgectl list
```

If you decide to clean up overlapping registrations, check other hosts first. Then remove **only** the redundant root with the control tool's `rm` command and run `sync --prune`. **Caution:** pruning may delete globally managed wrappers used by other applications. No manual cleanup is required just to use the host's filtered plugin browser.

## Limitations

- Windows editors depend on Wine, the bridge, and X11/XWayland. Try `QT_QPA_PLATFORM=xcb` if the editor cannot embed; for drawing/input issues, the **Parameters (host controls)** tab is a fallback. DXVK may help some Wine plugins that fail to repaint.
- Bridge operations occur outside the real-time audio callback, but external plugins can still crash or misbehave; there is no general plugin-crash isolation guarantee.
- This project does not redistribute vstbridge, yabridge, Wine, or proprietary plugin binaries.

For step-by-step problem diagnosis, see [Troubleshooting](troubleshooting.md).
