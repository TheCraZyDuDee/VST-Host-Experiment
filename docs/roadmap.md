# Roadmap

## Phase 0 — Foundation
- [x] Establish goals and architectural constraints.
- [x] Add a minimal CMake/C++20 scaffold.
- [ ] Add command-line argument handling and structured diagnostics.
- [ ] Add a small test target and CI build.

## Phase 1 — Audio routing proof of concept
- [ ] Inspect PipeWire and JACK API availability on target Linux distributions.
- [ ] Enumerate input devices and select a microphone.
- [ ] Capture audio and expose a named virtual source.
- [ ] Verify routing into a second application and measure CPU use, latency, and xruns.
- [ ] Support clean shutdown and device disconnect/reconnect.

## Phase 2 — Processing graph
- [ ] Define a real-time-safe processor interface.
- [ ] Run a test processor (gain/bypass) through the live signal path.
- [ ] Add chain ordering, channel-layout checks, and latency reporting.
- [ ] Add persistence for device and graph settings.

## Phase 3 — Native plugins
- [ ] Evaluate VST3 hosting libraries/SDK integration and licensing.
- [ ] Discover and load a native Linux VST3 plugin.
- [ ] Handle plugin state, bypass, failures, and sample-rate/block-size changes.
- [ ] Evaluate VST2 support and distribution implications separately.

## Phase 4 — User interface
- [ ] Add an optional Qt 6 UI only after the engine API is stable.
- [ ] Device selection, plugin chain editor, bypass, meters, and status.
- [ ] Ensure the app remains usable headlessly.

## Phase 5 — Later research
- [ ] Assess Wine-based hosting for Windows plugins without requiring users to manage a bridge manually.
- [ ] Prototype compatibility in an isolated helper process.
- [ ] Document unsupported plugins and failure behavior.
