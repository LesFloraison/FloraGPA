# Current capabilities and compatibility

Reviewed **2026-10-04**. The latest accepted build is pinned by the
[pipeline getter baseline](pipeline-getter-baseline.json), following discovery
revision 656bf82. The [getter audit](PIPELINE_GETTER_AUDIT.md) records its scope.

The inventory contains **345 files: 340 positive replays and five explicit
missing-information/resource rejections**. The eight previous getter blockers
now pass, together with four new boundary originals. One additional SO capture
lacks its referenced buffer descriptor and remains a located negative. Original
GPA can replay its final image; proven handling of that unused binding lifetime
is still open, so this rejection is not a claim that its final pixels are
fundamentally unrecoverable.

## Supported scope

The compatibility target is **GPA 2025 R1 legacy DX11 / IGPA v3 on Windows x64**.
Ordinary replay is native C++20/Qt and requires neither Python nor GPA.
Support is established for specific layouts, command/resource semantics and
tested workloads; opening a file does not prove accurate replay.

| Area | Available behavior | Boundaries and evidence |
|---|---|---|
| Preflight and API inspection | Offline diagnostics, decoded fields, references, coverage and Qt diagnostics | Structural success is not GPU success; metadata is not execution. [Acceptance infrastructure](COMPATIBILITY_BASELINE.md) |
| Pipeline getter observations | 34 additional checked immediate Context4 getter families; decoded optional outputs and reference details | Metadata never restores native bindings or fabricates observed objects. Nonempty SO production path remains unaccepted because its saved original lacks the buffer entry. [Getter acceptance](PIPELINE_GETTER_AUDIT.md) |
| Immediate-context replay | Captured draws/dispatches, supported setters, uploads, copies (including Context1 transfers), ClearView, queries/predication and presentation | Broader layouts, resource versions and some presentation/transfer cases remain incomplete. [Setters](PIPELINE_SETTER_AUDIT.md), [texture transfers](TEXTURE_COPY_AUDIT.md), [Context1 transfers](TRANSFER1_AUDIT.md), [ClearView](CLEAR_VIEW_AUDIT.md), [presentation](PRESENT_REPLAY_AUDIT.md) |
| Frame-time creation | Validated buffers, textures/views, shaders, pipeline states, class linkage/instances and predicates | Device5 coverage is not every interface version or descriptor combination; required missing data and unresolved identities are rejected. [Buffers](BUFFER_CREATION_AUDIT.md), [textures](TEXTURE_DIMENSIONS_AUDIT.md), [texture views](VIEW_CREATION_AUDIT.md), [buffer views](BUFFER_VIEW_CREATION_AUDIT.md), [pipeline](PIPELINE_CREATION_AUDIT.md), [geometry/SO](GEOMETRY_CREATION_AUDIT.md), [classes](CLASS_CREATION_AUDIT.md), [predicates](PREDICATE_CREATION_AUDIT.md) |
| Texture and output inspection | Presentation/RTV/DSV selection, mip/layer/slice, typed/channel/range controls, supported MSAA resolve/sample and planar inspection, exports | Format/device limits remain; legacy P010/P016 initial data can contain recovered Y only. [Frame output](FRAME_OUTPUT_MIGRATION.md), [texture inspection](TEXTURE_INSPECTION_MIGRATION.md), [planar writes](PLANAR_WRITE_MIGRATION.md) |
| Pipeline, buffers and geometry | Captured versus native replay state, before/after boundaries, constants/counters, IA and supported post-shader output | Missing state retains provenance; SO cursors have no native getter; frame-before counters require evidence or explicit experiments. [Boundaries](BEFORE_BOUNDARY_MIGRATION.md), [geometry](POST_TRANSFORM_MIGRATION.md), [HS output](HULL_OUTPUT_MIGRATION.md), [counters](BUFFER_VIEW_CREATION_AUDIT.md) |
| Experiments | Shader replacement, six-stage shader/IA/CB/SRV/sampler/output/SO/pipeline setter edits, buffer/texture edits and history | Event-scoped edits differ from persistent setters; consumer and format combinations retain their own limits. [Shader setters](SHADER_SETTER_MIGRATION.md), [event textures](EVENT_TEXTURE_MIGRATION.md), [reports](EXPERIMENT_REPORT_MIGRATION.md) |
| Coverage and Quad | Native execution, Qt controls, navigation, export and cancellation | Coverage is not an overdraw counter; Quad groups are not physical hardware quad counts. Sample/order/coordinate limits remain. [Coverage](COVERAGE_UI_MIGRATION.md), [Quad](QUAD_UI_MIGRATION.md) |
| Shader tools and debugging | DXBC/reflection, supported HLSL recovery, compilation/projects, source metadata and native GS/HS/DS checkpoint UI | Recovered HLSL is not original source; source availability and shader operations constrain debugging. [Recovery](HLSL_RECOVERY_MIGRATION.md), [projects](SHADER_PROJECT_MIGRATION.md), [checkpoints](CHECKPOINT_UI_MIGRATION.md) |
| RenderDoc analysis | Pixel History, VS/PS/CS recorded debugging, Replay Mesh and Replay Metrics | Requires compatible external RenderDoc 1.45 release DLL; unavailable values and backend limits remain explicit. [History](PIXEL_HISTORY_UI_MIGRATION.md), [debugging](REPLAY_DEBUG_UI_MIGRATION.md), [mesh](REPLAY_MESH_MIGRATION.md), [metrics](REPLAY_METRICS_MIGRATION.md) |
| GPU measurements | Native DX11 statistics/timing; Intel MD foundation, scheduled/uniform Qt collection and event-group CLI collection | Intel paths require supported hardware/driver. Event-group Qt/session integration, GTPin/Shader Profiler and further consumers remain incomplete. [Statistics](GPU_STATISTICS_MIGRATION.md), [timing](GPU_PROFILE_MIGRATION.md), [scheduled UI](MD_ITERATIONS_UI_MIGRATION.md), [uniform UI](UNIFORM_METRICS_UI_MIGRATION.md), [groups](MD_HOTSPOTS_MIGRATION.md) |
| Captured contexts and command lists | Identity/evidence inspection, inventory and explicitly checked limited metadata paths | Production replay of captured Deferred Context / ExecuteCommandList semantics remains incomplete. Self-created lists in metrics/diagnostics do not establish captured-list replay support. [Context workflow](USAGE.md#contexts-and-pipeline-boundaries) |
| Resource minimum LOD | Native setters, initial getter evidence, frame-time creation defaults, ClearState preservation and disabled-setter experiments | Missing required initial state is rejected; nonzero-LOD full storage export and UAV/output/interface guards remain boundaries. Unused static SRVs use program declarations. [LOD](RESOURCE_LOD_AUDIT.md), [shader usage](MIN_LOD_USAGE_AUDIT.md) |
| Query and predication | Captured Query metadata/history; native predicate Begin/End, binding and Device5 frame-time creation | New predicates remain unissued until their recorded interval completes. Ordinary Query records may omit identities, intervals or full result bytes; pre-frame predicate history is not reconstructed. [Predicate creation](PREDICATE_CREATION_AUDIT.md) |

## Latest accepted replay matrix

The subsequent [SO lifetime investigation](SO_LIFETIME_DISCOVERY.md) adds nine
original files against the unchanged `5ebcd8f` binary: four replayable with
hardware/WARP byte and execution-count checks, five located missing-buffer
blockers. Original playback matches producer images for all nine. Cumulative
inventory is 354 files (344 positive, ten explicit rejections); the full
historical regression below remains the last complete matrix run. The
[focused baseline](so-lifetime-baseline.json) reports the additional checks
separately. Missing-buffer lifetime handling is still unimplemented.

The [getter baseline](pipeline-getter-baseline.json) records the last full
regression matrix; its five missing-state/resource negatives retain explicit diagnostics.

| Measure | Recorded result |
|---|---|
| Enrolled inventory / positive files / explicit negatives | 345 / 340 / 5 |
| Ordinary independent positive runs | 680 |
| Repeated output | 339 stable files; one known-variable Helldivers file |
| Previously stable image hashes | All 327 unchanged |
| Diagnostic control runs | 353 |
| Resource boundary exports | 208: 204 strict byte goldens and four Helldivers boundary diagnostics |
| Getter producer checks | 312 frames and 1,272 return assertions across thirteen original files |
| Getter type coverage | 34/34 identified families decoded as metadata; 73 records including the SO negative |
| Getter hardware/WARP positive rows | 24; two replays plus disabled-getter control per row |
| New original-player runs | 26 completed; twelve files equal producer, one class-instance discrepancy |
| Related CTest suites / golden checks | 38 / 4 passed |
| Getter / Qt-Worker cases | 30 / 56 passed, no skips |
| New rejection evidence | Missing SO buffer 44 at event 43; four prior minimum-LOD negatives retained |

These are separate measurements, not an overall correctness percentage. The
matrix includes research fixtures and self-owned original captures as well as
game frames; repeated equality alone does not certify intermediate state.

Twelve previously accepted transfer fixtures use presentation markers, so their images alone
cannot prove the resource writes. Exact byte boundaries verify every fixture,
including buffers, 3D, 1D arrays/mips and BC1. DISCARD fixtures overwrite complete
destinations; partial-discard untouched contents are not reconstructed.

Fourteen earlier direct-copy captures omit the context resource. Existing checked
Map READ/Unmap evidence permits independent replay; the original player returns
an Open error with an inner unknown-resource-type error. These files remain
separate controls. Adding an ordinary backbuffer clear to the producer causes
GPA to save the context and the new originals open/replay successfully. The
[transfer audit](TRANSFER1_AUDIT.md) retains both datasets and the failure evidence.

The [ClearView acceptance](CLEAR_VIEW_AUDIT.md) remains valid, including its
mandatory resource-byte checks, resolved-MSAA scope and uncertified D16 boundary.
The [boundary baseline](resource-boundary-baseline.json) pins strict expected-hash
enforcement, buffer exports, 18 CPU checks and four live correct/wrong-hash tests.

Explicit SetPredication now survives omitted predicate fields in original Draw
snapshots. Two intentionally suppressed-Draw originals exposed the defect;
unobserved full replay, native binding checks and exact final pixels now guard
it on hardware and WARP. Failed initial acceptance evidence is retained. Newly
created predicates do not receive fabricated empty intervals. Missing pre-frame
query history and unsupported interface/lifetime layouts remain separate limits.

The original-player class discrepancy is retained: thirteen diagnostic runs on
the RTX 3070 Laptop GPU observe zero class instances supplied to CSSetShader,
then a null CS and unchanged zero UAV bytes at Dispatch. Native/shim-injected
producers and FloraGPA hardware/WARP agree on expected outputs. This localizes
a failure in the development original-player path; it does not establish the
behavior of every original GPA GUI path. The general corpus original-player
adapter was not identified, so those runs do not establish strict equivalence
under matched device/driver settings.

[Comparison reporting](original-comparison-baseline.json) separately verifies
execution status and image comparisons (13 CPU checks, nine independent runs,
seven original runs). These focused runs are not added to the 680-run matrix.
Helldivers' [sharpening variability](HELLDIVERS_REPLAY_FIX.md) remains visible;
no shader modification, global tolerance or automatic event disable manufactures
a stable result. Earlier Intel-metric batches retain their separately documented
intermittent BF1 baseline mismatch.

A Qt regression prompted investigation that reproduced an export/cache lifetime defect. The [export fix](TEXTURE_EXPORT_FIX.md)
retains the selected texture across a queued refresh inside the save dialog.
The first UI failure and corrected deterministic counterexample are retained;
the other 35 suites passed initially and the complete UI suite passes after the fix.

## Remaining roadmap

| Stage | Current position and completion gate |
|---|---|
| M1 — Acceptance infrastructure | Minimum loop delivered: corpus, coverage, preflight, serial comparison and diagnostic queue. Extend evidence as paths arrive. |
| M2 — Ordinary replay | In progress. The 34 getter blockers are resolved within the observed immediate layouts. Complete SO readback/write/getter originals are now verified; five new missing-buffer lifetime cases remain blocked. Next prove safe lifetime handling, then audit remaining interfaces, auxiliary records and resource semantics; enrolled immediate-context paths must replay correctly or reject with reproducible, located diagnostics. |
| M3 — Deferred Context / Command List | Incomplete. Require original captures proving identity, build order, resource versions, repeated execution, restore-state and event mapping before production support. |
| M4 — Resources and boundaries | Incomplete. Verify saved initial/differential data, subresources, counters, Query/Predication and presentation; distinguish absent information from implementation gaps. |
| M5 — Stable compatibility release | Incomplete. Broaden captures, repeat/long-duration checks, recovery and large-file testing; validate clean-environment build/deployment and publish a fixed matrix. |
| M6 — Analyzer and other versions | Incomplete. Continue remaining Qt consumers, advanced profiling/debugging and version-specific adapters after core replay gates. Existing analyzer features remain available. |

Forward order is **M1 → M2 → M3 → M4 → M5**, then remaining M6 work. Existing
research/analyzer components do not complete earlier compatibility gates.
The scope is replay of existing captures; self-owned capture probes are
development fixtures, not a complete capture tool.

The [module ledger](migration.json) remains **72 ported / 117 partial / 15 pending**
across 204 Python modules. This is migration bookkeeping, not a denominator for
original GPA functionality. The [Chinese development log](MIGRATION_STATUS.md)
retains its chronology and historical counts.

## Evidence and maintenance

Capture names, device details, hashes and original baseline JSON remain intact.
Paths under `artifacts/`, `out/`, `build/` and the external reference workspace
identify local evidence, not files supplied by a clone. Original captures,
proprietary DLLs and the recovered Python reference are not bundled. Hashes
identify evidence; they do not make missing artifacts public.

One historical aggregate log is unavailable: an earlier Context1 batch's wrapper
overwrote the previous ClearView CTest log. Its original hash, archived per-suite Qt results
and corpus reports remain; the incident and current regression evidence are
recorded in the [transfer audit](TRANSFER1_AUDIT.md).

For a capability change, update this summary with a source revision and specific
acceptance evidence. Keep original batch records and JSON; add supersession
links instead of replacing historical results with new numbers. Independent
clean-Windows deployment and complete M2–M6 acceptance remain open.
