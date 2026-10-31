# Windows VST3 compatibility evaluation

## Recommendation

Use Wine plus yabridge as an optional compatibility backend rather than implementing a new Windows VST3/COM proxy layer in this host.

The host should remain a native Linux process. Only Windows plugin code should run under Wine, in yabridge's plugin-host process. The resulting Linux-side VST3 wrapper can then be loaded by the existing native VST3 processor chain. This avoids running the audio engine, PipeWire streams, or Qt frontend under Wine.

The initial integration is now implemented in the host: configured Windows VST3 directories are registered with yabridge, synchronized before PipeWire starts, and the generated Linux-side VST3 wrappers are loaded through the normal processor chain. The host still depends on separately installed Wine and yabridge binaries.

## Why this approach

- Wine translates Windows system calls to Linux; it is not a CPU emulator. A compatible Windows plugin can execute on the host CPU.
- yabridge already implements the difficult VST3 object/interface proxying, Windows message-loop handling, plugin lifecycle, and compatibility behavior.
- yabridge uses Unix-domain sockets for control/callback traffic and shared memory for audio buffers, with the socket channel also providing synchronization. That avoids serializing/copying full audio buffers through ordinary messages on every block.
- VST3 instances from the same module are hosted in one Wine process, matching the VST3 module/factory model.
- The host can continue using its existing processor chain and real-time callback contract through the Linux wrapper.

Sources:
- [yabridge architecture](https://github.com/robbert-vdh/yabridge/blob/master/docs/architecture.md)
- [yabridge project and compatibility notes](https://github.com/robbert-vdh/yabridge)
- [Wine overview](https://www.winehq.org/)

## Implemented user experience

1. The GUI and portable configuration have a separate Windows VST3 search-directory list.
2. The host locates yabridgectl on PATH, at `~/.local/share/yabridge/yabridgectl`, or through `YABRIDGECTL`.
3. The host invokes `yabridgectl add` for configured directories and `yabridgectl sync` before plugin loading. It does not install system packages or change the user's Wine prefix.
4. yabridge selects the Wine prefix associated with each plugin directory using its normal behavior.
5. The host reads `yabridgectl status` to find the generated VST3 wrapper directory and adds it to the native VST3 scanner.
6. The wrappers are loaded into the existing ordered processor chain and are exposed in the GUI chain list after startup.
7. Bridge setup errors are surfaced in the GUI status line and CLI output. Runtime bridge crash isolation and recovery remain future work.

The first implementation should support 64-bit Windows VST3 effects. 32-bit bridging, plugin installers, licensing/activation helpers, and editor edge cases should be treated as compatibility follow-ups rather than assumed to work.

## Performance and real-time constraints

The Wine process is an unavoidable additional process for Windows-only plugins. yabridge's shared-memory audio path is preferable to designing a new per-block socket serialization protocol. The host must still measure callback duration and xruns with real plugins; "low overhead" is not a promise of zero overhead. Wine scheduling, plugin thread behavior, GUI rendering, and plugin-specific DSP can all affect latency stability.

The bridge must be initialized and warmed up before the PipeWire stream starts. No process launch, socket setup, shared-memory allocation, plugin scanning, or Wine prefix work may occur in the audio callback. A bridge failure must never block the native audio thread indefinitely; the host should emit silence for the affected stage or stop the chain cleanly.

## Licensing and packaging

The yabridge project is GPL-3.0-or-later. The current host repository does not declare a project license. Do not vendor yabridge source or link against its code until the host's license and distribution obligations have been reviewed. A first integration can treat yabridge and Wine as optional external runtime dependencies and invoke their documented command-line tooling, without copying their implementation.

Wine and yabridge are optional dependencies. Native Linux VST3 hosting and the default headless build must continue to work when neither is installed.

## Known compatibility limits

Wine does not guarantee that every Windows plugin will work. Plugins may depend on Windows services, installers, activation systems, graphics APIs, codecs, or drivers that are unavailable or incompatible. Some plugins may require extra Wine components or graphics libraries. The UI must show unsupported or failed plugins explicitly and must not report a successful load merely because a wrapper file exists.

## Implementation status

- [x] Evaluate the lowest-overhead practical architecture.
- [x] Add Windows VST3 directories to configuration and GUI.
- [x] Detect yabridgectl and invoke its documented setup/sync commands; dependency installation remains external.
- [x] Synchronize Windows plugin wrappers and load them through the native chain.
- [ ] Add bridge lifecycle monitoring, failure isolation, and runtime diagnostics.
- [ ] Validate latency, xruns, editor behavior, and representative Windows plugins on Linux hardware.
