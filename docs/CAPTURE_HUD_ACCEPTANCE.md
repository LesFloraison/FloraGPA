# Capture HUD acceptance audit

Audit checkpoint: `7326f1a`, on `feature/dx11-capture`. This maps the requested
HUD plan to implementation and observed evidence. It does not declare delivery
complete, count HUD experiments toward the separate six-frame capture gate, or
promote old experiments to current-binary GF2 results.

The detailed historical evidence is in [capture-hud-evidence.json](capture-hud-evidence.json).
The current regression is `hidden_state_progress`: 77/77 capture-only CTests,
23/23 focused full-profile CTests, the capture UI CTest, and three standalone
exclusive-fullscreen files with 82 exact checks. GPU checks were serial.

| Requested behavior | Evidence inspected | Result and remaining limit |
| --- | --- | --- |
| Read-only original GPA HUD research, pinned DLL and RVA conclusions | `CAPTURE_HUD.md`, Reference research and Clean presentation storage investigation; ignored `artifacts/capture-hud-plan`; installed shim SHA-256 rechecked against `cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31` | HUD state bracketing, capture toggle, FPS and native Present order located. Original retained-pixel safety is explicitly unproven; product isolation has independent tests. |
| Independent native HUD component, no Qt/Core/Replay/format/GPA dependency | `src/capture/CMakeLists.txt`, HUD sources, package module inventories | Independent component and capture-only package verified. GDI rasterization and native DX11 copies use owned text/pixels; no GPA font/shader assets or private calls. |
| Compact panel, Detailed/Hidden modes, real feature level/driver/adapter/resolution, FPS | `hud_control.cpp`, `hud_model.cpp`, `hud_pixels.cpp`; `buffered_input_progress.gf2`, `hidden_state_progress.gf2` | Historical GF2 three-mode verification; current compact GF2 start-screen screenshot. FPS is measured per swap, with one-second updates, seven-second history and absent values before samples. |
| DPI, resolution changes, small windows, long adapter names | `hud_model_tests.cpp`, `hud_layout_tests.cpp`, `layout_progress` | Native awareness/resolution/resize and exact pixel bounds/alpha verified. Physical cross-monitor transitions remain unverified. |
| Ctrl+F1 and configurable/disabled hotkeys; rising edge, foreground only, no message consumption | `hud_control.cpp`, `hud_model_tests.cpp`, `hud_raw_buffer_tests.cpp`, `raw_input_progress`, `buffered_input_progress` | Legacy, raw-message, mixed and buffered input paths tested; application input bytes/registration preserved. Isolated physical hold and broader native focus transitions remain open. |
| English Armed/Capturing/Saving/Saved/Cancelled/Failed/Busy feedback, timed expiry | `hud_control.cpp`, `hud_model_tests.cpp`, `gf2_completion_progress` | Model expiry and observed GF2 failure/expiry verified. Exhaustive live screenshots of every phase are not established by model tests. |
| Shared request selection/UUID/busy/arm path, explicit swap selection and ambiguity | `capture_requests.cpp`, `controller_tests.cpp`, `CaptureUiTests.cpp` | CLI and HUD use the same entry. Controller covers busy and unique-foreground-or-selection outcome; Qt selection wiring is tested. An actual simultaneous ambiguous multi-swap hotkey exercise is not established by that conditional test. |
| Render outside capture event/object/resource streams; native private calls | `swap_chain.cpp`, `hud_control.cpp`, `presentation_resources.cpp`; injected fixture validators | Capture freezes before HUD; clean storage precedes next recording. Exact event/resource inventory checks and Draw-zero negative controls establish actual replay without HUD objects or work. |
| Clean initial resources, final backbuffer and preview; retained pixels across frames | `hud_clean_buffers_tests.cpp`, `presentation_resources_tests.cpp`, `validate_capture_hud.py`; runtime and package profile evidence | Deterministic resource bytes, preview and both players agree. Blt, multiple flip buffers, repeated captures, partial/failed Present and Resize experiments are recorded. |
| Windowed, borderless and exclusive fullscreen; no desktop-window substitute | `runtime_integration.profiles`, `layout_progress.profiles`, `device_lock_progress.presentation_profiles`, current exclusive package | Native in-process presentation verified in all three modes historically; current exclusive package also passes. |
| Context state, COM identity/lifetime and native Present unbinding | `present_bindings_tests.cpp`, `presentation_resources_tests.cpp`, `hud_hidden_state_tests.cpp` | Getter identity, native unbinding, SO append bytes, UAV counters, predicate values and query statistics checked; all tracked objects release after current teardown fix. |
| TEST exclusion and unsafe query/predicate/mapping/interface paths | `hud_control.cpp`, HUD pixel/hidden-state tests, object tests | TEST does not draw/count/advance capture. Query and predicate skip reasons exercised. Texture/external-map and interface-gap guards exist; a combined native HUD escape/mapping matrix is not yet established. |
| Context concurrency and transparent forwarding | `hud_concurrency_tests.cpp`, per-device presentation gates, `device_lock_progress` | Native/wrapped nested Enter/Leave, waiting context calls, independent devices and concurrent view creation pass. The later `present_concurrency_progress` adds 16 real native/wrapped Present pairs under application-owned native protection, with exact resources and no lock-order failure. Unsynchronized/external-interface races remain outside that proof. |
| ResizeBuffers/1 reference release, reattachment and recovery | `hud_resize_tests.cpp`, `resize_rejection_progress`, live retained-scene exercise | DX11 ResizeBuffers1 native rejection and ordinary Resize recovery verified; no claim of successful D3D12-only ResizeBuffers1 behavior. |
| Native HRESULT/LastError, restoration failure, device loss and no false publication | `hud_fault_tests.cpp`, `fault_progress`, hidden-state native observations | Controlled native-resource failure facade verifies exact forwarding and no frame/partial publication. Real driver removal is unverified. Separate real Present LastError observations are timing dependent and are not compared as a deterministic oracle. |
| Cancellation, final publication race, disconnect and continuing target | `present_session_tests.cpp`, `command_stream_tests.cpp`, `controller_tests.cpp`, historical GF2 disconnect evidence | Armed/recording cancellation, writer interruption and publication races checked; target continues after disconnect. Combined HUD failure/cancellation stress is still narrower than the full requested matrix. |
| CLI launch options, asynchronous Qt workflow, protocol mismatch | `CaptureHudOptions.cmake`, `CaptureUiTests.cpp`, `controller_tests.cpp` | Invalid/conflicting options reject before launch, settings are forwarded, protocol 3 mismatch and identity failures reject. Current UI CTest passes. |
| GF2 visible HUD, hotkey clean start capture, main-scene MSAA refusal | `buffered_input_progress.gf2` and current `hidden_state_progress.gf2` | Prior revision passes clean dual replay, Draw-zero control, MSAA refusal, disconnect and exit. Current PID 32968 has visible compact start HUD; its capture/home/exit follow-up awaits the isolated hold. |
| Personal English commits, honest migration, ignored research/artifacts | Git author/committer, `docs/migration.json`, ignored artifact paths | Floraison identity retained; capture evidence is independent of legacy parity. Formal two-launch/six-frame capture acceptance remains 0/6 and outside HUD acceptance. |

## Next acceptance actions

1. Preserve the live GF2 session's zero-file input baseline for the requested
   single physical hold. Do not press the shortcut programmatically or infer a
   hold result from the earlier user-confirmed multiple separate presses.
2. After that input, record its request/file count, verify the generated scene in
   both players, then finish the current-binary main-scene refusal and normal-exit
   checks. A missing input or reply is not a pass.
3. Close the specific integration gaps above: native focus transitions,
   ambiguous multi-swap hotkeys, remaining external-interface concurrency, and combined
   unsafe-state/failure/cancellation behavior. Component tests remain scoped to
   what they exercise. Keep physical monitor/driver-loss evidence explicitly open
   unless actually observed.

This checklist makes the remaining work concrete; it does not relax the original
plan or replace missing evidence with source inspection.
