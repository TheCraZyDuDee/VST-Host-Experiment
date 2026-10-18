# Roadmap

## Phase 0 — Foundation
- [x] Establish goals and architectural constraints.
- [x] Add a minimal CMake/C++20 scaffold.
- [x] Add the first PipeWire capture/source implementation.
- [ ] Compile on a Linux system with PipeWire development packages and fix API/compiler issues.
- [ ] Add unit tests for the SPSC buffer and a CI build.

## Phase 1 — Audio routing proof of concept
- [x] Add a capture stream and named virtual-source node.
- [x] Add a fixed gain/bypass processing stage.
- [ ] Verify microphone selection and source visibility on PipeWire/WirePlumber.
- [ ] Test mono microphones, channel negotiation, and device disconnect/reconnect.
- [ ] Measure latency, CPU use, buffer underruns, and dropped frames.
- [ ] Add a status/diagnostics path for buffer statistics.

## Phase 2 — Processing graph
- [ ] Define a real-time-safe processor interface.
- [ ] Support runtime gain/bypass changes using bounded real-time-safe control transfer.
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
