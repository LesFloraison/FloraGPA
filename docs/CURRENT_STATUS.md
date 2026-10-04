# Current capabilities and compatibility

Reviewed **2026-10-04**. The latest [initial cache recovery](INITIAL_CACHE_AUDIT.md)
adds initial category/descriptor membership and the conditional CSUAV collector.
Three original captures match 31,934 index records, 19,387 cache descriptors and
all 6,597 ERG collectors. This closes the prior 224 CSUAV gaps for that initial
scope. Full uint16 classification, original cache/collector boundary probes,
related CTest and four goldens pass. Edited versions, other index profiles and
traditional list execution remain unaccepted; GPU execution is unchanged.

The preceding [initialization reference recovery](INITIALIZATION_REFERENCES_AUDIT.md)
adds native read-only metadata for 25 fixed ERG collector wire types: 6,110 original
observations match exactly and 150 original boundary probes pass. The 224 observed
cache-dependent CSUAV collectors remain explicit gaps. API resource references and
GPU execution are unchanged; traditional lists remain unaccepted. Related CTest
and four GF2/BF1 golden checks pass. No new capture registrations were added.

The preceding [initialization scheduler investigation](INITIALIZATION_SCHEDULER_AUDIT.md)
observes 7,042 ERG initializations in eight unchanged captures and verifies 56
controlled original version schedules. Original scheduler-to-list registration
can preserve non-ID order and repeated events. Eight paired observer/control
playbacks have equal raw pixels. This is development evidence: full file-to-version
dependency recovery and traditional list execution remain open; production code
and the compatibility counts below are unchanged.

The preceding [merged-list evidence batch](DEFERRED_MERGE_AUDIT.md)
adds 64 original captures, all expanded immediate streams. All 64 pass repeated
independent/original playback against captured application output, with 384 strict
buffer exports and 306 diagnostic controls. In 32 dynamic-CB cases the original
shim already loses the workload's values and saves complete zero writes; correct
replay of those files is not recovery of the uninjected application result.
The 32 immutable-CB controls retain correct resource/image results. Capture-side
state restoration failures remain separately recorded in both groups.

The combined inventory is now **470 registered cases / 460 distinct hashes**:
448 replay-positive registrations (438 unique) and 22 rejection files. Of the
new positives, 32 explicitly carry a capture-side workload-data mismatch. These
counts measure replay of saved captures, not complete workload/API fidelity or
M3 completion. Production code/package is unchanged in this evidence-only batch;
the preceding 406-case GPU matrix was not rerun. See the
[merged-list baseline](deferred-merge-baseline.json).

The latest production build accepts
[proved unused missing-CB intervals](CONSTANT_BUFFER_LIFETIME_AUDIT.md), resolving
the nine rejection files from the [first M3 evidence batch](DEFERRED_VERSION_AUDIT.md).
All 18 paired originals now pass repeated replay and strict intermediate byte
checks. Missing buffer descriptors, storage and native bindings inside those
intervals remain unavailable; prefixes and native observers cannot fabricate them.
All 18 files contain expanded immediate commands, so traditional Command List
execution remains unaccepted. Correction: `6bc3cca`;
package: `out/FloraGPA-cb-lifetimes-final-20261004/`.

The preceding CB-lifetime inventory contains **406 registered cases / 396 distinct capture hashes**:
384 positive registrations (374 unique) and 22 rejection files. The new batch
has 36 independent positive runs, 36 original player runs, 108 strict resource
exports and 54 diagnostic controls. Related CTest/Qt checks and four GF2/BF1
checks pass. The old 388 preflight outcomes are unchanged; their full GPU matrix
was not rerun in this batch. See the [current baseline](cb-lifetime-baseline.json)
for scope and counts.

The preceding fixed M2 inventory contains **388 registered cases / 378 distinct capture hashes**:
366 positive cases (356 unique captures) and 22 explicit rejections (four initial
LOD dependencies, seven context-state files, nine missing discard views and two
ambiguous discard-pointer records). Earlier summaries counted registrations as
files; ten extra registrations share hashes with other synthetic fixtures.
That M2 batch reran all 388 cases. Six formerly rejected SO files now replay
through proven unused binding intervals. Their absent descriptors/storage and
intermediate native bindings remain unavailable and are explicitly reported.
The other four new SO files verify complete resource readback and writes.

## Supported scope

The compatibility target is **GPA 2025 R1 legacy DX11 / IGPA v3 on Windows x64**.
Ordinary replay is native C++20/Qt and requires neither Python nor GPA.
Support is established for specific layouts, command/resource semantics and
tested workloads; opening a file does not prove accurate replay.

| Area | Available behavior | Boundaries and evidence |
|---|---|---|
| Preflight and API inspection | Offline diagnostics, decoded fields, references, coverage and Qt diagnostics | Structural success is not GPU success; metadata is not execution. [Acceptance infrastructure](COMPATIBILITY_BASELINE.md) |
| Context-state objects | Checked Device5 creation/flags and Context4 swap inspection; GetCreationFlags as read-only metadata; located preflight/runtime rejection | State-object execution remains unsupported. Actual non-null swaps can serialize zero identities and stale Draw snapshots. Null input cannot be proven from zero alone. [Original evidence and boundaries](CONTEXT_STATE_DISCOVERY.md) |
| Resource discard | Native Context1 calls for observed Context4 DiscardResource / DiscardView / DiscardView1 records; API fields, diagnostics and per-event CLI history | Missing view descriptors and zero-count pointer-presence loss reject. Discard does not define bytes; complete undefined-region propagation remains open. Tested whole-resource/view and rectangle forms have separate boundaries. [Discard acceptance](DISCARD_AUDIT.md) |
| Pipeline getter observations | 34 additional checked immediate Context4 getter families; decoded optional outputs and reference details | Metadata never restores native bindings or fabricates observed objects. Nonempty SO getter/write originals pass; absent-only unused lifetimes have separate provenance and inspection limits. [Getter acceptance](PIPELINE_GETTER_AUDIT.md), [SO lifetimes](SO_LIFETIME_AUDIT.md) |
| Omitted constant-buffer bindings | Checked per-slot lifetimes closed by explicit setters/ClearState before GPU use; mixed saved slots retain native execution | Missing descriptors/storage and in-interval native bindings are not recovered. Prefixes, native command observers and unsupported edits reject. [CB lifetimes](CONSTANT_BUFFER_LIFETIME_AUDIT.md) |
| Immediate-context replay | Captured draws/dispatches, supported setters, uploads, copies (including Context1 transfers), ClearView, queries/predication and presentation | Broader layouts, resource versions and some presentation/transfer cases remain incomplete. [Setters](PIPELINE_SETTER_AUDIT.md), [texture transfers](TEXTURE_COPY_AUDIT.md), [Context1 transfers](TRANSFER1_AUDIT.md), [ClearView](CLEAR_VIEW_AUDIT.md), [presentation](PRESENT_REPLAY_AUDIT.md) |
| Frame-time creation | Validated buffers, textures/views, shaders, pipeline states, class linkage/instances and predicates | Device5 coverage is not every interface version or descriptor combination; required missing data and unresolved identities are rejected. [Buffers](BUFFER_CREATION_AUDIT.md), [textures](TEXTURE_DIMENSIONS_AUDIT.md), [texture views](VIEW_CREATION_AUDIT.md), [buffer views](BUFFER_VIEW_CREATION_AUDIT.md), [pipeline](PIPELINE_CREATION_AUDIT.md), [geometry/SO](GEOMETRY_CREATION_AUDIT.md), [classes](CLASS_CREATION_AUDIT.md), [predicates](PREDICATE_CREATION_AUDIT.md) |
| Texture and output inspection | Presentation/RTV/DSV selection, mip/layer/slice, typed/channel/range controls, supported MSAA resolve/sample and planar inspection, exports | Format/device limits remain; legacy P010/P016 initial data can contain recovered Y only. [Frame output](FRAME_OUTPUT_MIGRATION.md), [texture inspection](TEXTURE_INSPECTION_MIGRATION.md), [planar writes](PLANAR_WRITE_MIGRATION.md) |
| Pipeline, buffers and geometry | Captured versus native replay state, before/after boundaries, constants/counters, IA and supported post-shader output | Missing state retains provenance; SO cursors have no native getter; frame-before counters require evidence or explicit experiments. [Boundaries](BEFORE_BOUNDARY_MIGRATION.md), [geometry](POST_TRANSFORM_MIGRATION.md), [HS output](HULL_OUTPUT_MIGRATION.md), [counters](BUFFER_VIEW_CREATION_AUDIT.md) |
| Experiments | Shader replacement, six-stage shader/IA/CB/SRV/sampler/output/SO/pipeline setter edits, buffer/texture edits and history | Event-scoped edits differ from persistent setters; consumer and format combinations retain their own limits. [Shader setters](SHADER_SETTER_MIGRATION.md), [event textures](EVENT_TEXTURE_MIGRATION.md), [reports](EXPERIMENT_REPORT_MIGRATION.md) |
| Coverage and Quad | Native execution, Qt controls, navigation, export and cancellation | Coverage is not an overdraw counter; Quad groups are not physical hardware quad counts. Sample/order/coordinate limits remain. [Coverage](COVERAGE_UI_MIGRATION.md), [Quad](QUAD_UI_MIGRATION.md) |
| Shader tools and debugging | DXBC/reflection, supported HLSL recovery, compilation/projects, source metadata and native GS/HS/DS checkpoint UI | Recovered HLSL is not original source; source availability and shader operations constrain debugging. [Recovery](HLSL_RECOVERY_MIGRATION.md), [projects](SHADER_PROJECT_MIGRATION.md), [checkpoints](CHECKPOINT_UI_MIGRATION.md) |
| RenderDoc analysis | Pixel History, VS/PS/CS recorded debugging, Replay Mesh and Replay Metrics | Requires compatible external RenderDoc 1.45 release DLL; unavailable values and backend limits remain explicit. [History](PIXEL_HISTORY_UI_MIGRATION.md), [debugging](REPLAY_DEBUG_UI_MIGRATION.md), [mesh](REPLAY_MESH_MIGRATION.md), [metrics](REPLAY_METRICS_MIGRATION.md) |
| GPU measurements | Native DX11 statistics/timing; Intel MD foundation, scheduled/uniform Qt collection and event-group CLI collection | Intel paths require supported hardware/driver. Event-group Qt/session integration, GTPin/Shader Profiler and further consumers remain incomplete. [Statistics](GPU_STATISTICS_MIGRATION.md), [timing](GPU_PROFILE_MIGRATION.md), [scheduled UI](MD_ITERATIONS_UI_MIGRATION.md), [uniform UI](UNIFORM_METRICS_UI_MIGRATION.md), [groups](MD_HOTSPOTS_MIGRATION.md) |
| Captured contexts and command lists | Identity/evidence inspection; verified expanded streams from two-context A/B/A and three-context merged-list workloads | Traditional list execution remains incomplete. Dynamic-CB merge captures can already contain wrong data; native/injected/original/independent results and restoration discrepancies remain separate. [Merge evidence](DEFERRED_MERGE_AUDIT.md), [deferred evidence](DEFERRED_VERSION_AUDIT.md), [context workflow](USAGE.md#contexts-and-pipeline-boundaries) |
| Resource minimum LOD | Native setters, initial getter evidence, frame-time creation defaults, ClearState preservation and disabled-setter experiments | Missing required initial state is rejected; nonzero-LOD full storage export and UAV/output/interface guards remain boundaries. Unused static SRVs use program declarations. [LOD](RESOURCE_LOD_AUDIT.md), [shader usage](MIN_LOD_USAGE_AUDIT.md) |
| Query and predication | Captured Query metadata/history; native predicate Begin/End, binding and Device5 frame-time creation | New predicates remain unissued until their recorded interval completes. Ordinary Query records may omit identities, intervals or full result bytes; pre-frame predicate history is not reconstructed. [Predicate creation](PREDICATE_CREATION_AUDIT.md) |

## Latest full registered-scope gate

The [gate baseline](m2-gate-baseline.json) records 732 positive replays and
44 located negative attempts. All 365 previously stable case images remain
unchanged; Helldivers is the one known-variable case. There are 353 diagnostic
control runs and 254 resource exports, including 250 strict byte goldens and
four Helldivers boundary observations. All 732 positive dependency audits pass.

All 48,796 registered command records decode. Their 214 observed wire families
have 95 execution classifications, 117 metadata classifications and two
unsupported context-state classifications. These are observed type/decoder
counts, not full semantic acceptance percentages: individual descriptors,
identities, context kinds and execution boundaries still determine support.

Forty-one related CTest suites pass. Qt/Worker has 56 passing cases; Context has
10, including Finish disable/edit/linked/deferred counterexamples. Ten CPU gate
tests reject incomplete evidence, changed diagnostics, crashes, timeouts and
false determinism claims. Four isolated GF2/BF1 golden/negative checks pass.
The two original Finish files pass four further independent and four original
player runs with equal pixels; original adapter/configuration equivalence remains
unproven. The original player was not rerun for the entire matrix.

The registered ordinary-path M2 gate passes: cases replay or retain specific,
reproducible rejections. Traditional lists, broader resource dependencies and
M3–M6 completion are still open. Package: `out/FloraGPA-m2-gate-20261004/`.

## Previous focused discard validation

Twenty-six new originals contain fifteen positives and eleven explicit
negatives. The completed-view producer verifies 408 native/injected frames;
the first missing-view originals remain separate evidence. New package replay
passes all fifteen positive files twice and locates all eleven rejections in
22 attempts. Thirty post-write/pre-Present resource exports match strict CPU
byte hashes. All 52 original-player runs match producer final images, with
original device/configuration equivalence still unproven.

Twelve related CTest suites pass. Discard tests have 54 passing cases, including
60 repeated hardware/WARP full replays, two verified SRV-draw prefixes, 28
disabled-discard controls and 88 full storage checks. Qt/Worker has 56 passing
cases. Existing SO/getter files pass 44 package runs; context-state control
passes twice and seven context-state files retain fourteen located rejections.
Four GF2/BF1 golden/negative checks pass under isolated runtime dependencies.

Calling Discard is counted separately from the driver's physical invalidation,
which may be omitted legally. Subsequent defining writes provide the byte
oracles. This batch does not rerun the entire historical matrix or certify
reads from discarded contents before those writes.

## Previous focused context-state validation

Eight unmodified originals have 192 verified native/injected producer frames,
2,688 binding checks and 264 exact draw-byte checks. Independent replay passes
the ordinary control twice and explicitly rejects seven state-object files in
fourteen attempts. Preflight locates fourteen ambiguous swaps and two successful
creations with missing returned identity. Three record families are now decoded;
state swapping itself is not counted as supported execution.

Sixteen original-player runs repeat stably, but three of eight files disagree
with their producer's final colors. A fourth file matches the final image while
its intermediate Draw snapshots remain stale. This reinforces the need for
resource/state evidence in addition to final image equality.

Eight related CTest suites pass, including 17 context-state, 22 SO, 30 getter and
56 Qt/Worker cases without skips. Twenty-two existing SO/getter files pass 44
serial package replays, and four GF2/BF1 golden/negative checks pass with isolated
runtime dependencies. The [baseline](context-state-baseline.json) keeps these
focused results separate from the older full matrix below.

## Previous full accepted replay matrix

The [SO lifetime baseline](so-lifetime-acceptance-baseline.json) records the
then-current regression, plus focused storage/cursor and original-player
checks. Historical getter and SO discovery baselines remain unchanged.

| Measure | Recorded result |
|---|---|
| Enrolled inventory / positive files / explicit negatives | 354 / 350 / 4 |
| Ordinary independent positive runs | 700 |
| Repeated output | 349 stable files; one known-variable Helldivers file |
| Previously stable image hashes | All 343 unchanged (339 full-baseline files plus four SO discovery positives) |
| Diagnostic control runs | 353; plus twelve focused disabled-SO-draw controls |
| Matrix resource boundary exports | 224: 220 strict byte goldens and four Helldivers diagnostics |
| Focused SO resource exports | 44: 32 strict final bytes and twelve prefilled before-draw observations |
| Newly admitted unused SO files | Six original files, seven unmaterialized setters; missing resources are not fabricated |
| SO corpus hardware/WARP runs | 36, across nine files; older getter SO file also covered by CTest and matrix |
| New original-player runs | 20, across ten files; exact producer/native final pixels |
| Related CTest suites / golden checks | 39 / 4 passed |
| SO lifetime / getter / Qt-Worker cases | 22 / 30 / 56 passed, no skips |
| Remaining enrolled rejections | Four initial minimum-LOD dependencies with precise event/resource diagnostics |

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
seven original runs). Those historical focused runs are not added to the 700-run matrix.
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
| M2 — Ordinary replay | Registered-scope gate passed: 366 positive cases and 22 located rejections; auxiliary/snapshot contracts audited. This is not universal API support. State-object identity loss, mixed saved/missing SO targets and unverified lifetimes remain explicit boundaries. |
| M3 — Deferred Context / Command List | Current next stage; incomplete. Installed DLL/evidence hashes and ten expanded legacy originals rechecked. Require original retained-list captures proving identity, build order, resource versions, repeated execution, restore-state and event mapping before production support. |
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
