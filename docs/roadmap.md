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
- [x] Define a real-time-safe processor interface and route the gain stage through it.
- [ ] Support runtime gain/bypass changes using bounded real-time-safe control transfer.
- [x] Add ordered processor chains, mono/stereo adaptation between VST3 stages, and cumulative plugin-latency reporting.
- [x] Persist plugin chain order and host audio routing/gain/latency settings in the Qt UI.
- [x] Add portable configuration import/export shared by GUI and CLI; run saved configurations headlessly.
- [ ] Add persisted plugin-internal state and device settings.

## Phase 3 — Native plugins
- [x] Evaluate the Steinberg VST3 SDK license and optional dependency strategy.
- [x] Add an initial optional VST3 adapter and `--plugin PATH` support for one stereo audio effect.
- [ ] Build with the SDK and validate load/process/unload against a real native Linux VST3 plugin.
- [ ] Improve bus negotiation, plug-in failure reporting, state, bypass, and sample-rate/block-size changes.
- [x] Add a multi-plugin VST3 chain in CLI and Qt UI, including ordering and selected-instance editor access.
- [ ] Add parameter/state persistence.
- [ ] Evaluate VST2 support and distribution implications separately.

## Phase 4 — User interface
- [x] Add an optional Qt 6 UI with VST3 editor hosting; headless build remains independent.
- [x] Plugin chain editor, gain bypass, and engine status.
- [x] PipeWire device enumeration/selection in the Qt frontend.
- [ ] Plugin bypass and input/output meters.
- [ ] Ensure the app remains usable headlessly.

## Phase 5 — Later research
- [x] Add configurable plugin-directory scanning and discovery.
- [ ] Assess Wine-based hosting for Windows plugins without requiring users to manage a bridge manually.
- [ ] Prototype compatibility in an isolated helper process.
- [ ] Document unsupported plugins and failure behavior.
