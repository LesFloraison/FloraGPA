# Native DX11 product scope and closure

Reference: installed GPA 2025 R1, `25.1.26603.47871`. This is a bounded native
backend acceptance, not a claim to implement every original GPA feature.
Independent capture development is outside this change.

**2026-11-03: bounded branch development is complete; formal release is not
accepted.** Removed product entries and external runtime dependencies are gone,
the blocking class-linkage crash is handled, and no new regression remains in the
exercised retained scope. This is not an all-green test or numerical qualification
claim: the failures, skips and target restrictions below remain explicit.
[Machine-readable results](research/native-gpa-closure-validation.json) preserve
the complete effective test inventory, workload outcomes and evidence hashes.

## Product entry inventory

| Product entry | Decision | Basis and acceptance |
| --- | --- | --- |
| Open GPA frame, API navigation, replay and preflight | Retain | Existing native replay scope; corpus replay and retained UI/core suites |
| Texture/buffer storage, resource bindings and pipeline inspection | Retain | GPA resource identities; resource, pipeline, selection and export suites |
| Geometry inputs and post-transform mesh | Retain | GPA mesh workflow; native mesh model/UI suites and same-device game baseline comparisons |
| Shader source/DXBC/reflection, replacement and experiment projects | Retain | Existing shader inspection/replacement scope; shader, experiment and recovery suites; recovered HLSL remains labelled |
| Coverage and Quad diagnostics | Retain | Original diagnostic evidence and retained instrumentation/UI suites; neither promises RenderDoc fragment histories |
| DX11 statistics/timing and Intel metrics | Retain | Native queries and Metrics Discovery; counter/metric suites and packaged collection; Intel hardware metrics require a supported driver |
| GPA event pixel history | Retain within certified target scope | Original callback evidence in `research/DX11_PIXEL_HISTORY_DIAGNOSTICS.md`; rerun twelve scenarios on Intel and NVIDIA |
| VS/PS/CS execution debugger | Removed previously | Original availability property is constant false; `research/DX11_DEBUGGER_AVAILABILITY.md` |
| GS/HS/DS checkpoint tabs, stepping, watches and configuration UI | Remove | No verified original available product entry; remove rather than extending hidden debugger protocols |
| `shader-checkpoint`, instruction-level geometry trace exports | Reject publicly | CLI and Worker share the rejection; internal model/instrumentation remains available for mesh validation |
| Event storage history mode | Internal validation only | No GUI action or public CLI entry; snapshot collection still supports GPA diagnostic results and internal tests |
| Per-fragment identity/order, shader-output and merger reconstruction | Removed previously | Original observed history returns event rows, not RenderDoc's fragment contract |
| RenderDoc recapture, external backend, settings and runtime distribution | Removed | Native Worker only; old commands reject, old paths are ignored; source, import and runtime event audits |

The pinned player SHA-256 is
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`;
the presenter SHA-256 is
`35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80`.
Both installed images were checked against existing Ghidra evidence. Hidden
functions are not treated as evidence of an available original product feature.

## Compatibility and exclusions

- Analysis backend identity is `dx11-native/21`; stale caches from earlier
  backends cannot be accepted as current results.
- Event diagnostics accept single-sample 2D color render targets at layer zero.
  Depth targets, MSAA, other layers/slices and nonzero samples reject explicitly.
  Broader targets are future work, not a branch closure prerequisite.
- Existing experiments retain their resource identity and schema. Retired
  checkpoint configuration cannot restore a removed UI entry.
- `checkpoint_ui` and `UiTests::nativeDebugControls` are retired, not passed.
  Internal checkpoint/model, storage, source and mesh tests remain because
  they validate shared infrastructure. Historical research stays archival.
- The empty-function-table dynamic pixel shader crashes on the observed NVIDIA
  devices `10de:249d` and `10de:2c58`. These exact device/bytecode combinations
  reject before driver compilation with a WARP instruction. WARP output and
  ordinary hardware class linking remain tested; this is not a vendor-wide ban.

## Validation record

Work logs and raw original results are under `artifacts/native-closure-20261102`.
The fresh build is `build/native-closure`, with the external capture directory
explicitly configured. The default replay-only configuration registers 150 tests;
capture-only development is not added to this branch's release gate.

The fresh all-target Release build succeeds. A serial run of all 150 CTest
registrations and a targeted rerun after the UI scope corrections produce:

| Final effective result | Count |
| --- | ---: |
| Suites passed with no skipped cases | 101 |
| Suites completed with skipped subcases (not complete passes) | 45 |
| Failed suites | 4 |
| Qt cases passed / failed / skipped | 2201 / 5 / 949 |
| Missing executables / CTest timeouts | 0 / 0 |

`test-summary.json` combines `ctest.xml` with the later `affected.xml`; original
logs and both sets of Qt output remain intact. Most skips require optional
specialist captures/reference installations or explicit environment opt-ins.
They do not certify those corpora. The Intel hardware opt-in was actually run,
and its numerical failure is included above. Retired checkpoint UI tests and the
instruction trace portion of `PostTransformTests::mainWindowInspection` are
retirements, not passes. Internal trace/model tests remain.

The initial reused `build/native-gpa` failed on corrupt old objects. It is not the
final build evidence. The new directory was configured and built from scratch;
after scope fixes the full target set built again successfully. Initial coverage
and post-transform UI failures were stale references to removed entries; retained
assertions still run, and the corrected suites pass. No remaining new regression
was observed in the exercised retained scope.

### Historical problems and follow-up

| Problem | Reproduction / result | Classification and next action |
| --- | --- | --- |
| `class_linkage` | Original empty-table dynamic PS crashes NVIDIA `10de:2c58`; exact-device guard added; 10 Qt cases pass | Blocking crash fixed by explicit unsupported rejection; WARP succeeds. Future hardware support needs a driver-safe path without changing shader semantics |
| `hlsl_recovery` | 9 pass / 1 fail, `simultaneousBf1Replacements` | Existing BF1 golden mismatch; current actual equals prior baseline and packaged BF1 replay. Recovering/exporting HLSL works; old golden remains unchanged. Investigate the historical image/reference difference before claiming full fidelity |
| `recovery_ui` | 7 pass / 1 fail, same BF1 image difference | Same historical issue, not an added UI regression. Source replacement flow still completes; full BF1 recovered-image qualification stays open |
| `dxbc_feedback_relocation` | 42 pass / 2 fail, `gpu-4318-1-0` and `gpu-4318-2-0` | Previously documented NVIDIA raw/structured compute UAV feedback mismatch. Compute execution debugger is outside product scope; retained graphics Coverage/Quad tests pass. Shared compute feedback is not numerically qualified; retain failing assertions and investigate separately |
| `metrics_discovery` | Hardware opt-in: `hardwareLifecycle`, Intel `8086:7d67`, `RenderBasic/CsThreads` returns 0 for expected 6; unchanged prior executable also fails | Existing device/driver numerical qualification failure. Packaged collection/record availability is separate from value correctness. Do not claim verified Intel hardware counter values; investigate Metrics Discovery/driver behavior. Native D3D11 statistics remain available |

The BF1 actual hash is
`3d431509ec8258047bc7381367a2861e36ba26abad658bc642524692c07426cc`;
the unchanged expected hash is
`1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6`.
Reproduce these suites with `ctest --test-dir build/native-closure -C Release
--output-on-failure -R "^(class_linkage|hlsl_recovery|recovery_ui|dxbc_feedback_relocation|metrics_discovery)$"`.
For Intel hardware qualification set `FLORA_TEST_INTEL_METRICS=1`; preserve the failing assertions. These issues do not block
the bounded scope removal, but they prevent broader numerical/fidelity claims.

`NativeAnalysisTests` passes all 62 cases, including
`isolatedInventoryAndStaleResult` and `failCancelTimeoutAndRetry`.
`NativeHistoryTests` passes 170 cases, including cancellation, unsupported MSAA,
depth/layer/sample rejection and replay-state comparisons before/after analysis.
The history UI passes 16 cases, including GPA diagnostics presentation, selection
mismatch and stale context rejection. Backend version 21 prevents old caches
from restoring a removed feature.

The unedited Intel Helldivers `metric-profile --set RenderBasic --event 200`
attempt fails its existing image-invariance guard: 126,638 pixels differ between
baseline and instrumented replay, with matching dimensions, frame, experiment
and event mapping. No profile is published. The capture already has a
`known_variable` full-image policy; this is consistent with that limitation,
but does not prove counter correctness or qualify this workload. It is recorded
as a failed profile, not a pass or an unsupported adapter. The native metrics
implementation and its image assertion were not changed by this task.

### Portable package and original differential

The final package matrix completes **50 workloads: 49 successful executions,
1 failed Intel Helldivers profile** described above. All 50 dependency audits
pass. The matrix validator exits **1** because the profile failed; its completion
marker records completion and counts, not an all-passed verdict. GF2, BF1 and Helldivers replay, selected-resource extraction, selected final
mesh and all-event native counters complete on NVIDIA `10de:2c58`, Intel
`8086:7d67` and WARP. All nine selected meshes match prior same-device bytes;
GF2/BF1 NVIDIA images match the prior baseline. Exported analysis artifact sizes
and SHA-256 values are checked against their manifests.

Helldivers full-frame images retain their existing `known_variable` policy.
Three NVIDIA replays with event 18401 disabled are identical and have 129
dispatches; before-event and disabled-event images match the prior same-device
package. Suppressing draws changes the output. This validates those bounded
controls, not a cross-GPU or deterministic full-frame image claim.

The pinned original twelve-scenario history fixture was reprobed separately on
Intel and NVIDIA. The final package matches each device's original event order,
pixel bytes, status and counts: ten returned event rows against both original
callbacks. Unsupported targets reject in the native tests. No GPA binary runs
as part of the product workload; the original probe is a separate reference tool.

Intel GF2/BF1 metric-profile collection and catalog discovery complete; Intel
numerical qualification remains failed. WARP/NVIDIA have native DX11 counters
but do not provide Intel MD hardware counters. The packaged GF2 GUI opens and
replays with its API log, texture and metrics visible; the screenshot was checked
and the retired checkpoint tab is absent.

Import audit: **33 EXE/DLL files**, zero forbidden imports. Runtime audit:
**6,613 image-load events and 159 process creations**, zero forbidden or
unresolved images across the 50 workloads (including two prior-package control
runs). The complete process trees include CLI/Worker/GUI and Windows console
hosts; no GPA or RenderDoc process is started. Source matches are explicit
removed-command rejection or an optional capture injector exclusion list, not
load/link dependencies. Historical research scripts are marked archival.

The first Intel Helldivers texture attempt hits the default 180-second Worker
deadline. Its explicit `--timeout 600` retry succeeds in 239.891 seconds; the
failed attempt and rejected nonempty-output retry remain preserved. Long-running
analysis is not counted as passed until it actually completes.

Deliverable: [FloraGPA-native-closure-20261103-validation.zip](../out/FloraGPA-native-closure-20261103-validation.zip),
47,067,099 bytes, 38 archive entries. SHA-256:

`c34dd7ef05e9458612d55335064f6137e53d9754c3a44c46ac48d97fe56177b6`

The source manifest and product-binary hashes are in the compact ledger. The ZIP
was rehashed and checked against the final source manifest. Full raw logs live in
[the local artifact directory](../artifacts/native-closure-20261102), with
[effective test results](../artifacts/native-closure-20261102/test-summary.json)
and [package workload results](../artifacts/native-closure-20261102/package-validation/validation.json).


`tools/audit_native_runtime.py` uses Windows debug events for process creation,
exit and DLL loads, including modules unloaded before completion. It runs
packaged workloads with a Windows-only PATH. Its positive control follows a
parent/child pair; negative controls detect a deliberately renamed executable
and a DLL loaded and immediately unloaded before process completion.
This developer-host audit is not a substitute for independent clean-host release
validation.

## Reproduction

Configure a new VS2022 x64 build with Qt 6.11.2, `BUILD_TESTING=ON` and
`FLORA_TEST_CAPTURE_DIR` pointing to the local corpus. Build Release, then run
`ctest --test-dir <build> -C Release --parallel 1 --output-on-failure
--output-junit <results.xml>`. Preserve all Qt logs; skipped cases and missing
executables are not passes.

Package with `tools/package.ps1 -BuildDirectory <build> -OutputDirectory <empty>`.
`tools/validate_native_closure.py --package <package> --captures <corpus>
--out <fresh-results> --original-results <probe-results>` checks the packaged
workloads, same-device replay/mesh baselines and original event diagnostics.
It requires the local historical baseline artifacts; these are evidence inputs,
not runtime dependencies.

Formal release remains gated on a separate Windows environment without GPA or
RenderDoc installed. No automatic merge or publication is part of this work.
