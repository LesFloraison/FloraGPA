# Capture HUD acceptance audit

Audit checkpoint: counted user hold on `20e38bf`, on `feature/dx11-capture`. This maps the requested
HUD plan to implementation and observed evidence. It does not declare delivery
complete, count HUD experiments toward the separate six-frame capture gate, or
promote old experiments to current-binary GF2 results.

The detailed historical evidence is in [capture-hud-evidence.json](capture-hud-evidence.json).
The current regression is `publication_progress`: 83/83 capture-only CTests,
33/33 focused full-profile CTests including capture UI, and five injected profiles
with 15 files and 410 exact checks. Two HUD publication profiles additionally
pass 119 checks each. The latest GF2 start capture executes in both players with
exact clean backbuffer/preview pixels and a Draw-zero negative control. GPU checks
were serial. Runtime SHA-256 is recorded in the evidence JSON. The subsequent
test-only `interaction_progress` adds native focus/selection evidence, five files
and 120 dual-player checks on the unchanged DLL, plus 4/4 capture-only and 2/2
full-profile focused CTests including capture UI.

The later `input_diagnostics_progress` adds read-only hotkey counters. Its two
runtime builds, six focused capture-only tests and Qt capture UI pass. The new
GF2 short-control session was closed by the user. After relaunch, the user
confirmed one physical hold: zero baseline to one edge/attempt/accepted request
and one file, with no automated capture chord. The new file executes exactly in
both players, matches the clean preview/backbuffer, exports VS/PS and 105 resources,
and changes both outputs identically when all 18 Draw counts are zeroed. Legacy
repeat messages remain zero: the strict typematic gate is still unpassed, while
the measured single submission and user attestation are recorded separately. The earlier reported hold
reached MSAA refusal with no file on the old runtime; last-request-only status
could not measure its submission count. Rendering code is unchanged, but that
old observation is not promoted to a counted long-hold pass.

| Requested behavior | Evidence inspected | Result and remaining limit |
| --- | --- | --- |
| Read-only original GPA HUD research, pinned DLL and RVA conclusions | `CAPTURE_HUD.md`, Reference research and Clean presentation storage investigation; ignored `artifacts/capture-hud-plan`; installed shim SHA-256 rechecked against `cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31` | HUD state bracketing, capture toggle, FPS and native Present order located. Original retained-pixel safety is explicitly unproven; product isolation has independent tests. |
| Independent native HUD component, no Qt/Core/Replay/format/GPA dependency | `src/capture/CMakeLists.txt`, HUD sources, package module inventories | Independent component and capture-only package verified. GDI rasterization and native DX11 copies use owned text/pixels; no GPA font/shader assets or private calls. |
| Compact panel, Detailed/Hidden modes, real feature level/driver/adapter/resolution, FPS | `hud_control.cpp`, `hud_model.cpp`, `hud_pixels.cpp`; `publication_progress.gf2` | Latest GF2 Compact/Detailed/Hidden cycle and main-scene feedback verified. FPS is measured per swap, with one-second updates, seven-second history and absent values before samples. |
| DPI, resolution changes, small windows, long adapter names | `hud_model_tests.cpp`, `hud_layout_tests.cpp`, `layout_progress` | Native awareness/resolution/resize and exact pixel bounds/alpha verified. Physical cross-monitor transitions remain unverified. |
| Ctrl+F1 and configurable/disabled hotkeys; rising edge, foreground only, no message consumption | `hud_control.cpp`, `hud_model_tests.cpp`, `hud_raw_buffer_tests.cpp`, `interaction_progress` | Actual native foreground/background transitions verified for x64 buffered Raw Input and mixed legacy/raw input. Background chords reach the registered application/sink without changing HUD mode or submitting capture; foreground return restores one capture and one mode edge. Registration and retained pixels stay exact. User-attested single hold has exactly one measured submission; legacy typematic messages and held-key-through-focus transitions remain unverified. |
| English Armed/Capturing/Saving/Saved/Cancelled/Failed/Busy feedback, timed expiry | `hud_control.cpp`, `hud_model_tests.cpp`, `gf2_completion_progress` | Model expiry and observed GF2 failure/expiry verified. Exhaustive live screenshots of every phase are not established by model tests. |
| Shared request selection/UUID/busy/arm path, explicit swap selection and ambiguity | `capture_requests.cpp`, `controller_tests.cpp`, `CaptureUiTests.cpp`, `hud_selection_target.cpp`, `interaction_progress` | Two real same-HWND foreground chains produce `Select swap chain`; both explicit IDs reach arm. A unique recent candidate is selected automatically, and resumed ambiguity refuses again. Three selected files execute with exact red/blue pixels while the other chain is idle. Another chain Present during recording remains explicitly unsupported and publishes nothing; this is not full overlapping multi-swap capture support. |
| Render outside capture event/object/resource streams; native private calls | `swap_chain.cpp`, `hud_control.cpp`, `presentation_resources.cpp`; injected fixture validators | Capture freezes before HUD; clean storage precedes next recording. Exact event/resource inventory checks and Draw-zero negative controls establish actual replay without HUD objects or work. |
| Clean initial resources, final backbuffer and preview; retained pixels across frames | `hud_clean_buffers_tests.cpp`, `presentation_resources_tests.cpp`, `validate_capture_hud.py`; runtime and package profile evidence | Deterministic resource bytes, preview and both players agree. Blt, multiple flip buffers, repeated captures, partial/failed Present and Resize experiments are recorded. |
| Windowed, borderless and exclusive fullscreen; no desktop-window substitute | `publication_progress.profiles` | Current native in-process presentation passes blt, flip3, borderless, exclusive and a System32-PATH exclusive package profile: three clean files and 82 checks per profile. |
| Context state, COM identity/lifetime and native Present unbinding | `present_bindings_tests.cpp`, `presentation_resources_tests.cpp`, `hud_hidden_state_tests.cpp` | Getter identity, native unbinding, SO append bytes, UAV counters, predicate values and query statistics checked; all tracked objects release after current teardown fix. |
| TEST exclusion and unsafe query/predicate/mapping/interface paths | `hud_control.cpp`, HUD pixel/hidden-state tests, object tests | TEST does not draw/count/advance capture. Query and predicate skip reasons exercised. Later `mapping_guard_progress` verifies dynamic texture/DXGI Surface skip-and-resume, preserved mapped buffer bytes, and an actual native ID3D11Debug escape that skips HUD and rejects armed/future capture. |
| Context concurrency and transparent forwarding | `hud_concurrency_tests.cpp`, per-device presentation gates, `device_lock_progress` | Native/wrapped nested Enter/Leave, waiting context calls, independent devices and concurrent view creation pass. The later `present_concurrency_progress` adds 16 real native/wrapped Present pairs under application-owned native protection, with exact resources and no lock-order failure. Unsynchronized/external-interface races remain outside that proof. |
| ResizeBuffers/1 reference release, reattachment and recovery | `hud_resize_tests.cpp`, `resize_rejection_progress`, `publication_progress` | Fixed reproduced HUD-only ordinary Resize failure by draining deferred private-view references after detach. Native held-reference rejection, DX11 ResizeBuffers1 rejection, recovery and full-size retained-scene tests pass; no claim of successful D3D12-only ResizeBuffers1 behavior. |
| Native HRESULT/LastError, restoration failure, device loss and no false publication | `hud_fault_tests.cpp`, `fault_progress`, hidden-state native observations | Controlled native-resource failure facade verifies exact forwarding and no frame/partial publication. Real driver removal is unverified. Separate real Present LastError observations are timing dependent and are not compared as a deterministic oracle. |
| Cancellation, final publication race, disconnect and continuing target | `present_session_tests.cpp`, `command_stream_tests.cpp`, `controller_tests.cpp`, `mapping_guard_progress`, `publication_progress` | HUD-active two/three-buffer profiles each run 12 publication-race attempts, all Cancelled without files and with exact retained pixels. Three Completed controls per profile separately reject late cancellation. No claim that both outcomes won a racing attempt. Combined cancellation/failure guards pass; latest GF2 keeps presenting after disconnect, ignores both shortcuts and exits normally. Actual driver removal is unverified. |
| CLI launch options, asynchronous Qt workflow, protocol mismatch | `CaptureHudOptions.cmake`, `CaptureUiTests.cpp`, `controller_tests.cpp` | Invalid/conflicting options reject before launch, settings are forwarded, protocol 3 mismatch and identity failures reject. Current UI CTest passes. |
| GF2 visible HUD, hotkey clean start capture, main-scene MSAA refusal | `publication_progress.gf2` | Production `d4ae3e4` passes three modes, start hotkey capture, exact dual/backbuffer/preview pixels, 18-Draw zero control, 108-resource inventory and VS/PS exports. Main scene visibly refuses MSAA; a later Hidden request refuses unmatched Unmap and publishes nothing. Disconnected shortcuts leave requests/input counts unchanged while Presents advance; normal exit confirmed. The later counted hold on `20e38bf` is recorded in `single_hold_progress`; typematic messages were not observed. |
| Personal English commits, honest migration, ignored research/artifacts | Git author/committer, `docs/migration.json`, ignored artifact paths | Floraison identity retained; capture evidence is independent of legacy parity. Formal two-launch/six-frame capture acceptance remains 0/6 and outside HUD acceptance. |

## Next acceptance actions

1. The new session PID 14736 has a zero baseline, user-confirmed single hold,
   one measured submission and one dual-verified file. No further user input is
   pending for that observation. The strict legacy-repeat validator remains
   unpassed because zero typematic messages were observed; do not conflate
   that separate condition with the measured submission count.
2. Ordinary native focus transitions and simultaneous ambiguous multi-swap hotkeys
   now have explicit integration evidence. A physical key held across a focus
   change has not been exercised. Preserve the separate capture limitation:
   another chain Present during recording still explicitly rejects the frame.
3. Keep physical cross-monitor transitions, actual driver removal and arbitrary
   unsynchronized/external-interface races outside the verified scope unless
   actually exercised. Component tests establish only their recorded conditions.

This checklist makes the remaining work concrete; it does not relax the original
plan or replace missing evidence with source inspection.
