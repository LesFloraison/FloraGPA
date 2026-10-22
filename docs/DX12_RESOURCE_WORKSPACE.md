# Unified resource workspace (2026-10-21)

The central **Resources** workspace is the single entry for DX11/DX12 texture
and buffer inspection. **Current Draw** retains the existing output/input rail,
individual binding slots, thumbnails and collapsed input-buffer group.
**All resources** contains the former left-hand catalogue, search and type
filters, using the current execution boundary without assigning an arbitrary
bound view. Buffer navigation opens the existing byte viewer inside Resources.
Final Frame remains a separate operation.

DX12 requires a submitted execution instance: a recorded command can have
multiple submissions. Selecting an instance automatically queries bindings,
selects the previous valid slot or the first RTV/DSV/texture/buffer, and reads
its contents. Input bindings default to Before; RTV, DSV and UAV default to
After. Boundary, mip, array layer, volume slice, plane, sample, channel/range
and buffer byte-range changes refresh after the existing 200 ms debounce.
Replay and Refresh explicitly retry/recapture. Failures do not enable query
isolation or enter a retry loop.

## Coverage and identities

Resources uses the same `readRdcAnalysis` service as Pipeline, with no simulated
Read click. `Dx12DrawResources.cpp` adapts verified RenderDoc pipeline rows to
the shared binding model. Creation record and captured object must both match
the session. Unmapped/mismatched rows remain visible with errors. Registers,
spaces, array elements, descriptor identity, format, view range and buffer
range are retained; repeated bindings remain separate.

This remains **partial RenderDoc descriptor coverage**, visibly labelled in
the rail. Reflected/observed descriptors are potential bindings, not proof of
GPU reads or writes. Unenumerated descriptor heap entries are not invented.
DX12 uint64 creation/object/execution identifiers use decimal JSON strings.
DX11's existing numeric interfaces are unchanged.

RenderDoc's public format representation collapses the two depth/stencil SRV
plane formats into one D24S8/D32S8 value. Such SRV rows explicitly report that
the bound plane cannot be verified; **All resources** can inspect either plane
without claiming it is the bound view. DSV depth/stencil switching is supported.

## Boundaries, workers and cache

Native recapture emits a versioned, checked resource-boundary marker after
draining the selected Before/After execution prefix. The reader requires the
matching execution and query-isolation policy, rejects missing/duplicate
markers and selects that exact RDC event. Before does not mean the previous
draw. Pipeline and After reads share a recapture; Before content creates or
reuses a separate recapture only when needed.

The in-session recapture key contains capture/session identity, execution,
experiment, allocation/device policy, query policy, RenderDoc path and DLL
digest, and boundary. The native device selection is automatic for DX12.
An application/backend update starts a new in-memory cache.

`resource-workspace` is an isolated analysis action. It reads the selected
texture or buffer before scheduling thumbnails. Thumbnail batches contain at
most 16 selected/visible rows and deduplicate identical content requests in the
batch; each batch uses one reader process and a shared recapture. The existing
96-pixel maximum dimension and 128 MiB decoded thumbnail cache remain.

RenderDoc exports display PNGs using the view component type, mip/layer/slice,
sample and depth/stencil channel. Raw storage is separate and hash/length
checked before publication. Resolved displays export raw sample zero; volume
storage contains all slices of the mip and depth/stencil storage retains the
backend's packed components. Export metadata states this scope.
Formats with fewer than four components display with opaque alpha, preventing
RenderDoc's implicit integer alpha of 1 from hiding RG/R textures. The raw
artifact retains the original data.

Resource changes invalidate old image, buffer and export state immediately.
Cancelled or superseded reports must pass revision, worker identity, complete
request, execution, resource lifetime and boundary checks before publication.
Image decoding/storage validation runs off the UI thread. Background completion
does not switch the active analysis page.

## Bounds, editing and saved state

`Dx12ResourceSelection.cpp` is shared by the frontend, job preparation and
worker startup. It validates integer coordinates, arithmetic, mip/layer/plane
counts, samples, volume slices, view bounds and byte ranges before GPU work.
The backend checks the actual replay resource again. 3D slices never contribute
to the D3D12 array subresource index. Illegal old indices reset to the new
view's first valid selection. Only stencil formats offer the stencil plane;
planar video requests currently report unavailable.

The advanced Index control shares the mip/layer/plane conversion. DX12 project
schema 1 remains supported; optional `ui.dx12.resources` saves workspace scope,
binding, display and buffer settings. Existing buffer interpretation, edits
and exports use the embedded viewer. DX12 edits retain the native patch
engine's explicit Before submission / After execution semantics and validation.

## Validation

`DrawResourceTests` covers high uint64 identities, repeated submissions/views,
register spaces/arrays, sparse RTVs, CBV/SRV/UAV/VB/IB, mapping failures,
subresource index 4, stencil, 3D slices, MSAA and corrupt requests.

`FloraResourceWorkspaceFixture` generates independent D3D12 captures with known
values: color/depth 0.25 before and 0.75 after, stencil 37/201 and buffer 7/42.
It writes different values after the selected marker. Both exact-boundary
checks pass, including mip 1 / array layer 1, separate raw storage and duplicate
thumbnail requests. This verifies the RDC reader against known GPU operations;
real capture tests separately exercise native prefix recapture.

Local evidence is under `artifacts/resources-unified/` and
`build/resources-*.txt` (not distributed with a clone). GPU runs are serialized.
The compatible patched RenderDoc 1.45 from the existing package is used.
Unpatched official 1.45 correctly refuses captures requiring the existing
discard-layout patch. No GPA binary analysis or new RenderDoc patch was needed.

### Recorded acceptance results

Release build, Windows 11, Qt 6.11.2, patched RenderDoc 1.45. These are elapsed
wall-clock measurements from this machine, not performance guarantees. Warm
reads reuse the recapture but still launch an isolated reader.

| Capture / execution | Binding list | First content | Selected thumbnail | Recaptures |
| --- | ---: | ---: | ---: | ---: |
| BF1 / 53533 | 19.746 s | 29.625 s (buffer) | N/A | 1 |
| BF1 / 37273 | 22.219 s | 31.576 s | 41.282 s | 1 After + 1 Before |
| Talos / 12675 | 7.494 s | 10.045 s | 12.458 s | 1 |
| Hellblade2 / 10267 | 16.085 s | 19.476 s (UAV texture) | 22.805 s | 1 for initial content |

BF1 37273 input read took 22.352 s; returning to the output took 10.189 s
without another recapture. BF1 53533 genuinely has no RTV/DSV: its accessible
resources include a VS UAV buffer and vertex buffers. The workspace correctly
opens a buffer instead of inventing a render target. Talos exercises a depth
array view at layer 1; BF1 37273 exercises DSV depth/stencil switching.
Hellblade2 36779 additionally verifies a full-size input texture and buffers.

The actual-window tests pass automatic selection, thumbnails, embedded buffer
reads, rapid resource/boundary changes, stale export invalidation and keeping
the Shader page active during background completion. Legacy schema-1 project
loading resets invalid index 4 to 0 and preserves new workspace settings on
save/reload. Talos 19408 explicitly refuses an incomplete ResolveQueryData
(140 of 236 results missing), including after retry: zero recaptures and no
automatic isolation. This refusal is not counted as successful content access.

The final shared-resource suite reports 17 passed / 0 failed / 1 skipped (the
optional real-capture test), including the independent known-value fixture.
The optional DX11 BF1 window test was then run separately: 3 passed / 0 failed.
Other relevant suites: UI 50 passed / 47 environment-dependent skips; output
storage 29 passed; RDC 11 passed; RDC jobs 60 passed; GUI lifetime 6 passed.
All report zero failures. The final DX12 acceptance run reports 5 passed,
covering legacy settings, strict refusal and Hellblade2 automatic navigation.
After the implicit-alpha correction, the Hellblade2 window test and both
known-value boundary checks were repeated successfully (3 passed each). The
packaged reader then returned all six fixture requests without errors.

Independent BF1 native readback comparison is recorded in
`bf1-native-comparison.json`: mean absolute RGBA byte differences from Resources
are approximately [0.051, 0.048, 0.038, 0.047]. Two native runs themselves differ
by [0.075, 0.076, 0.056, 0.073]. These real-frame GPU variations are reported
separately; they are not claimed as byte equality and do not relax the
deterministic fixture's known-value checks. MSAA/3D validation is covered by
model/request tests, not a complete GPU format matrix.

The portable build is available locally at
`out/FloraGPA-resources-final-20261021/FloraGPA.exe`, including its pinned patched
RenderDoc and DXC dependencies. The CLI startup and packaged resource reader
are smoke-tested separately.

Useful commands after a Release build:

```powershell
# Pure/model and existing DX11 workspace regression; optional oracle uses
# FLORA_RENDERDOC_LIBRARY and FLORA_RESOURCE_FIXTURE_OUTPUT.
build/vs2022/Release/FloraDrawResourceTests.exe
build/vs2022/Release/FloraRdcTests.exe
build/vs2022/Release/FloraRdcJobTests.exe

# Actual Windows window, automatic reads, thumbnail, boundary/resource
# navigation, depth/stencil, embedded buffers and stale-result checks:
# FLORA_DX12_CAPTURE_DIR, FLORA_RENDERDOC_LIBRARY,
# FLORA_RESOURCE_CAPTURE, FLORA_RESOURCE_EXECUTION, FLORA_RESOURCE_OUTPUT.
build/vs2022/Release/FloraDx12SessionTests.exe automaticResourceWorkspace
```

## Retained analysis and CPU bindings (2026-10-22)

Resource browsing now has separate capture-context and selection lifetimes.
The GUI owns one isolated `FloraGPA.Rdc --serve` process; its single worker
thread owns the replay controller, execution index and resource catalog.
Ordinary selection cancellation discards obsolete results without unloading
the controller. Native capture, Pixel History and Shader Debug still use
isolated workers. Opening another capture or changing replay context releases
the controller. A crash/timeout permits one restart, then one-shot fallback;
a reported controller allocation/open failure does not trigger retry loops.

The schema-1 JSON-lines protocol accepts `open`, `execute`, `cancel`, `close`
and `shutdown`; string serials and generations preserve 64-bit identities.
Stdout carries protocol messages, stderr carries diagnostics, and result
artifacts remain in request-owned temporary directories. The original `--job`
interface and its default `legacy` output remain supported.

The workspace explicitly requests `thumbnail`, `preview` or `storage` output.
Thumbnails render into a headless target bounded to 96 x 96, including extreme
aspect ratios; they do not first export a full-sized image. Display previews
do not call raw texture storage readback. Raw export snapshots the resource,
execution, boundary, capture and experiment identity, then reads storage in an
isolated worker and validates byte count, SHA-256 and sidecar metadata.

Navigation uses a 100 ms debounce for new GPU work. Cached content and CPU
binding metadata can publish immediately. Pipeline completion schedules the
selected resource without a second debounce. Visible thumbnails are executed
one at a time, with the selected preview taking priority; an eligible selected
display image also supplies its thumbnail. CPU rows are replaced by observed
GPU rows even when their selection context is unchanged.
Immediate completion scheduling is restricted to DX12. DX11 retains its
existing 200 ms resource scheduler so background thumbnail work does not
preempt History/Checkpoint actions immediately after a foreground task.

After foreground work and visible thumbnails finish, one idle second allows
a full-frame analysis capture. Its key excludes the selected DC. Version-2
Before/After markers carry execution, source record, submission, queue, list,
recording generation and ordinal. Before is an explicit marker, not `eid-1`.
Missing/ambiguous pairs, unsupported queue order or failed preparation use the
original exact-prefix path. A failed context is not retried automatically.
Only one replay controller is active; switching captures first releases it.
Background GPU work waits for automatic draw timing collection and can be
preempted by foreground selection.

The CPU binding index uses the existing recipe decoders, submission index,
address index and root-signature deserializer without creating GPU resources.
Sparse per-slot histories restore command-list state by recording generation.
RTV/DSV descriptors are consumed at recording time; shader-visible descriptors
are resolved at submission time. Descriptor copies preserve source provenance,
and resource/heap creation identities distinguish reused addresses. Unresolved
or GPU-generated state remains pending. Direct Draw and Dispatch queries use
their respective graphics/compute binding scopes; indirect operation scope
remains pending until replay confirms it. CPU bindings describe potential
bindings, not measured shader accesses or original-frame fidelity.

Display caches use 240 MiB for prepared images and 16 MiB for thumbnails.
Serialized binding snapshots use a 64 MiB metadata-cache budget. The separate
CPU index also has a conservative 64 MiB accounting guard. Disk analysis
captures use an 8 GiB LRU with the active capture pinned. Content keys retain
execution, lifetime, boundary, subresource, view/display parameters and replay
context; no unproven cross-DC content equality is assumed.

Internal regression switches are `FLORA_DISABLE_RDC_SERVICE`,
`FLORA_DISABLE_FAST_PREVIEW` and `FLORA_DISABLE_FRAME_REUSE` (presence disables
the corresponding path). `FLORA_RESOURCE_TRACE` records preview-cache keys.
Backend result telemetry includes capture open, index, event positioning,
queue, request, display conversion, storage read and byte counts. Native reports
include per-stage `latency_ms`. GUI diagnostic properties expose capture
count, cache hits and CPU binding publication time.

The independent D3D12 fixture compares prefix and full-frame Before/After
color, depth, stencil, buffer, mip/array, four-sample MSAA, R32_UINT and 3D
slice data, including raw hashes and display equality. It also verifies
preview-only requests, bounded thumbnails and deferred exports. The headless
thumbnail target's sRGB encoding is compensated so its displayed UNorm/depth
values match the saved preview. CPU tests cover repeated submissions, descriptor
rewrites, recording generations and resource identity reuse. Real-frame CPU
index tests sample 20 Draw executions from each of BF1, Talos and Hellblade2.
These checks do not establish all GPU formats or alias/multi-queue combinations;
unsupported reuse remains on the exact-prefix path. The expanded deterministic
oracle is preserved under `artifacts/dx12-final-format-oracle-3`.

### Measured navigation and recovery evidence

The BF1 GUI comparison uses the same 21 selections (one cold selection followed
by 20 measured switches, including returning to the first DC). The compatibility
baseline disables retained sessions, fast previews and full-frame reuse; CPU
metadata indexing remains enabled in both modes, so its list latency is not an
old-GUI baseline. Percentiles use nearest rank and include the first controller
open on the prepared full frame.

| BF1 measurement | Compatibility path | Optimized path |
| --- | ---: | ---: |
| Cold first-content | 12,003 ms | 11,614 ms |
| Post-cold first-content p50 | 27,110 ms | 958 ms |
| Post-cold first-content p95 | 31,497 ms | 1,234 ms |
| Native captures across all selections | 21 | 2 |

This is a 96.1% reduction in post-cold p95 for this supported sample. All 20
switches after full-frame preparation added zero native captures. Optimized
binding-list publication p95 was 25 ms. Some DCs select a buffer first, so
"first content" includes a populated buffer view as well as texture images.
Local evidence is `artifacts/dx12-final-bf1-navigation/navigation.json` and
`artifacts/dx12-navigation-bf1-baseline.json`.

Talos has multiple frame queues and deliberately uses exact-prefix fallback.
Of 21 attempted selections, 17 loaded successfully in both modes. For the 16
successful post-cold samples, p95 changed from 11,893 ms to 9,132 ms (23.2%).
Failures at executions 14303, 15746, 17477 and 17832 are preserved in both
reports and excluded from content percentiles. They fail native prefix Close
with `0x80004005`; execution 14303 also reproduces with the pre-change
`ec6cb45` portable worker. These failures are not optimization passes.
Talos uses the explicit query-isolation policy for this comparison; strict
missing-query mode was separately checked to show CPU bindings without
submitting resource-capture GPU work. Evidence is under
`artifacts/dx12-matrix-20261022/talos-{optimized,baseline}` and
`build/resource-strict-cpu-acceptance.log`.

Hellblade2 also uses the multi-queue fallback. Both modes attempted 21
selections, with 17 successes and the same four prefix-Close failures
(executions 12653, 12948, 26534 and 26829). Successful post-cold p50 changed
from 39,369.5 ms to 29,859 ms, while p95 changed only from 64,319 ms to
63,292 ms (1.6%). Native capture attempts were 25 and 24 respectively.
This sample does not meet the 70% first-preview reduction target: repeated
native prefix capture still dominates its long tail. Its cold first-content
times were 17,739 and 20,204 ms; no cold-start improvement is claimed.
Evidence is under `artifacts/dx12-matrix-20261022/hellblade2-{optimized,baseline}`.

CPU snapshot querying was measured independently from GUI publication. The
final 20-Draw-per-frame run recorded p95 of 4.25 ms (BF1), 0.21 ms (Talos) and
2.63 ms (Hellblade2), after index builds of 144, 26 and 161 ms respectively.
See `build/resource-cpu-final.log`; this is CPU query time, not end-to-end DC
preview latency.

The retained real-capture sequence (pipeline, buffer, display preview,
thumbnail) records one `OpenCapture`, no raw storage file for either image
request, and a thumbnail bounded to 96 x 96. Its evidence is
`artifacts/dx12-resident-sequence-final/summary.json`. Service-client tests
exercise process reuse, two failed crash/protocol attempts followed by legacy
fallback, no retry for reported open/allocation failure, stale callback
suppression and process termination after explicit close. They do not wait
out the production 180-second timeout.

The final BF1 workspace interaction test records 20 cached input/output
selection pairs at p95 5 ms, with both images checked against their previously
accepted contents. `build/resource-final-cache-acceptance.log` also covers
thumbnail publication, resource/boundary navigation and stale-result handling.
The automatic-timing ordering test reaches the GPU timing chart in 8,987 ms
from opening BF1, while CPU navigation is active. Until timing data is
published it asserts zero resource-capture attempts and no resource image;
CPU bindings are available. This is open-to-chart time, not an isolated GPU
query duration. See `build/resource-final-timing-order.log`.

`tools/benchmark_dx12_navigation.py` reproduces GUI measurements with explicit
capture, library, execution list and query policy, and writes separate raw
navigation, test-log and summary files. `--baseline` selects the compatibility
path; `--require-reuse` makes unsupported preparation a test failure. Use a
fresh `--out` directory for each run and run GPU comparisons sequentially.

### Final Release regression and portable verification

The Release build and final package completed successfully. The full CTest run
covered 159 targets in 887.21 seconds (`build/resource-complete-regression.log`):
156 passed, two reproduced baseline failures, and the recovery target was
stopped for diagnosis. Its fault-worker fixture lacked the replay identity
envelope already required by the pre-change GUI, so intended artifact-failure
cases never reached artifact validation. The fixture now echoes that envelope;
separate missing/invalid-identity cases still verify rejection. Production
identity validation was not weakened.

After rebuilding, the complete recovery target passed all 123 QtTest cases,
with zero failures or skips, in 232.434 seconds
(`build/resource-recovery-final.log`). An intermediate run encountered child
process startup failures; the isolated reproducer and subsequent full rerun
passed, and the failed log is retained as
`build/resource-final-recovery-regression.log`. Combining the full suite and
this targeted retest gives 157 passing CTest targets and two baseline failures:
`replay_debug_model` (the existing `typedInstructionWatchLifetime` assertion)
and `class_linkage` (NVIDIA `nvGetCompilerInterface` crash). This is not a claim
that a single full-suite invocation passed. Optional real-capture cases skipped
inside otherwise passing targets are not used as optimization evidence.

DX11 History and Checkpoint regressions passed after restricting immediate
resource scheduling to DX12 (`build/resource-dx11-scheduling-tests.log`). The
final backend-switch test also passed, checking DX11 image/geometry results,
DX12 capture switching, execution/source-record domains and stale selections
(`build/resource-final-backend-switch.log`). Its event-count assertion now
uses the selected domain's session count instead of a historical hardcoded
record-count threshold.

The final portable directory is
`out/FloraGPA-dx12-resources-20261022-final`, with pinned RenderDoc, DXC and Qt
dependencies. Minimal Windows-only PATH checks passed GUI `--version`, CLI
`--help`, BF1 native replay, the retained analysis sequence, and GUI opening
through automatic GPU timing publication. The GUI screenshot was visually
inspected. Packaged pipeline, buffer, preview and thumbnail requests shared
one `OpenCapture`; both image requests omitted raw-storage reads and files.
Evidence is retained in:

- `build/resource-final-package.log` and `build/resource-portable-startup.json`
- `artifacts/dx12-portable-resident-final/summary.json`
- `artifacts/dx12-portable-smoke-summary.json`
- `artifacts/dx12-portable-gui-final.png`
- `artifacts/dx12-portable-modules-final.json`

The module verifier checked four successful native/analysis process snapshots:
dependencies came from the package, Windows or the capture's Agility SDK,
with no external GPA/Python runtime. This is a portable smoke test on the
development machine, not clean-machine certification or a lifetime DLL trace.
The package's `build-info.json` identifies the final source commit and hashes
the shipped application executables.

## Multi-queue prefix reuse (2026-10-23)

The previous queue-count guards rejected every Talos and Hellblade2 full-frame
analysis capture. Retaining a controller therefore only helped reads within
one prefix: changing DC still recreated the native device/resources, replayed
initialization, captured a new prefix, and opened that new RDC. This repeated
work dominated navigation latency. The guards have been replaced by per-execution
prefix qualification; the native queue scheduler and its Fence semantics are
unchanged.

New full-frame captures emit version-3 Before/After markers. In addition to the
existing execution/source/submission/queue/list/generation/ordinal identity,
they record the first and last frame executions and whether every queue
operation through the selected submission has all required producers inside
that CPU prefix. An unresolved dependency still prevents native frame capture.
A dependency on a later CPU producer disqualifies that particular prefix.

The retained analysis controller validates the certificate once when indexing:

- Every execution in the declared frame range must have exactly one execution
  marker; missing/duplicate commands or inconsistent identities reject reuse.
- Each selected Before/After pair must bracket all actual actions of its
  invocation, including expanded indirect actions and experiment work.
- Every earlier execution must finish before Before, and every later execution
  must start after After. Prefix maxima and suffix minima over numeric execution
  IDs make these checks independent of JSON key order and avoid per-click scans.
- A reordered or dependency-open prefix falls back individually to the old
  exact-prefix path. The failure reason is reported in the task log.

The pinned RenderDoc D3D12 replay implementation waits for idle on queue
changes (`Serialise_ExecuteCommandLists`), so qualifying the observed command
prefix does not rely on inventing a GPU order from CPU timestamps. This is
equivalence to FloraGPA's existing replay observation boundary, not proof that
an originally racing application had a deterministic cross-queue result.
Resource lifetimes, query-isolation policy, experiment identity and the global
resource lifetime fallback retain their existing checks.

Version-2 markers retain the old single-queue restriction, counting copy-only
queues from the complete execution index as well as queues with work markers.
Before continues to select an explicit marker, never `eid - 1`. Only one
controller is active, automatic GPU timings still run first, and full-frame
preparation remains cancellable background work. `FLORA_DISABLE_FRAME_REUSE`
continues to select the previous per-DC path for comparison.

Preparation now includes opening the retained controller, validating/indexing
the boundary certificate, and replaying the selected pipeline before publishing
`analysisFrameReady`. The first implementation only prepared the RDC file:
Talos's first subsequent click still took 3.373 seconds, reducing the improvement
to 64.06% on the 16 mutually successful samples. Moving this real work into idle
preparation removes that hidden first-click cost. `preparation_ms` reports the
entire wait separately; the benchmark still includes the first subsequent click.
The warm-up publishes no resource image or binding rows. Selection cancellation
invalidates its result while preserving the service, and idle work can resume
on the same capture. Explicit cancellation closes the service and prevents
idle preparation from automatically restarting in that context; foreground
navigation can still reuse its saved capture. The optional
`analysisWarmupPreemption` GUI test interrupts controller opening with rapid
navigation and requires recovery without another native capture.

The new `dx12MultiQueueBoundaryValues` test uses an independent D3D12 fixture
with Copy -> Direct -> Copy queues, captured Fence dependencies, and a later
poison write. Prefix and reused Before/After reads must match every raw buffer
word (7 before, 42 after; never the later 99). Mapping tests cover repeated
source records, expanded actions, reordering, missing commands, wrong identities,
forward dependencies, and legacy captures with a copy-only second queue.

`dx12RealAnalysisBoundaries` adds optional reproducible real-frame comparison.
Set `FLORA_MULTIQUEUE_REAL_CAPTURE`, `FLORA_MULTIQUEUE_REAL_OUTPUT`,
`FLORA_RENDERDOC_LIBRARY` and comma-separated `FLORA_RESOURCE_EXECUTIONS`;
Talos also needs `FLORA_RESOURCE_ISOLATE=1`. It creates one full capture and
independent Before/After prefix captures, compares known binding identities and
view ranges after each DC, and compares raw SHA-256 and displayed pixels for
representative bound resource categories. Each run requires a fresh output
directory and preserves reports, jobs, RDCs and `comparison.json`.
`FLORA_MULTIQUEUE_REAL_CAPTURE_CACHE` can explicitly reuse completed diagnostic
captures when repeating comparisons; boundary identities are checked first.
Mismatches still fail the test, but subsequent resources and the other boundary
are checked so one mismatch cannot hide remaining results.

### Real boundary evidence and an unstable output

Talos executions 12675, 16201 and 19408 passed 14 representative resource/side
comparisons, including raw SHA-256 and preview equality. Their observed binding
identities and view ranges also matched the prefix path. Evidence is
`artifacts/multiqueue-talos-boundaries-1` and
`build/multiqueue-talos-boundaries-1.log`.

Hellblade2 executions 9731 and 22736 passed 14 resource/side comparisons and
binding comparisons. At execution 37090, input buffer 537415 and texture 505987
remain byte-exact, but output texture 544605 differs at both Before and After.
The strict comparison test remains failed for that output; no tolerance,
hash exclusion or shader/scheduling change was introduced to make it pass.
The original failure and completed late-boundary comparison are preserved in
`artifacts/multiqueue-hellblade-boundaries-1` and
`artifacts/multiqueue-hellblade-late-boundaries-2`.

To distinguish boundary errors from same-path variation, the *same prefix RDC*
was replayed three times, navigating away and back between reads. Texture
544605 produced three different hashes while the two input hashes remained
unchanged. Repeating the same experiment with the full RDC also changed that
output on every replay. Before-side prefix-to-prefix pairs differ at
5,476–6,767 of 2,304,000 pixels; the nine cross-path pairs differ at
4,804–6,484 pixels. These finite observations establish same-path instability,
not a numeric acceptance tolerance or proof that every cross-path difference
has the same cause. The existing upstream Hellblade probe/allocation studies
in `DX12_REPLAY.md` also document order-dependent replay data, but this change
does not modify those shaders or claim full original-GPA fidelity.

Before stability evidence is `artifacts/multiqueue-hellblade-repeat-1`, including
raw files, hashes and `variation.json`. The reusable diagnostic
`tools/compare_rdc_replay_stability.py` accepts saved prefix/reuse jobs and a
different navigation execution, saves all samples, and reports stability per
resource. An unstable output is explicitly unverified for cross-path byte
equality; successful navigation timings do not count as its correctness pass.
The public diagnostic was also run on the After jobs, with the same result:
both input resources are stable, while every prefix and reused output hash
changes. See `artifacts/multiqueue-hellblade-after-stability/summary.json`.
This leaves 18 byte-exact Hellblade resource/side comparisons and two unstable
output comparisons across the three selected DCs; all three After binding
identity/view comparisons passed.

The final direct multi-queue and format fixture run passed all four QtTest
cases (including setup/cleanup), without skips, in 22.211 seconds:
`build/multiqueue-final-oracles.log`. The event/certificate suite passed all
13 cases in `build/multiqueue-final-events-tests.log`.

The final GUI warm-up preemption, explicit-stop and automatic-timing-priority
run passed all three checks (5 QtTest cases including setup/cleanup, no skips,
44.711 seconds) in `build/multiqueue-final-scheduling.log`. During the timing test, CPU navigation
and binding indexing remained available while resource capture count stayed
zero until the timing chart was populated.

### Final navigation measurements

The comparison baseline disables **only** frame reuse, retaining the resident
service and fast preview path. Each sample uses the same 21-execution sequence
(20 selections plus a return), query policy and machine. Cold selection is
reported separately. Four existing prefix `Close` failures per capture are
listed explicitly and excluded from paired latency statistics, leaving 16
mutually successful post-cold samples. No slow successful sample is removed.
Percentiles use nearest rank; with 16 samples p95 is the maximum.

| Capture | Prefix p50 / p95 | Reuse p50 / p95, same 16 samples | p95 reduction |
| --- | --- | --- | --- |
| Talos, query isolation | 8,128 / 9,385 ms | 561 / 802 ms | 91.45% |
| Hellblade2, strict queries | 33,992.5 / 74,903 ms | 1,088 / 1,425 ms | 98.10% |

Both optimized runs completed all 21 selections with images and no failures.
Across all 20 post-cold selections, Talos preview p50/p95 is 507/767 ms and
binding-list p95 is 5 ms; Hellblade2 preview p50/p95 is 1,088/1,272 ms and
binding-list p95 is 19 ms. Native capture attempts dropped from 24 in each prefix run
to 2 for Talos and 3 for Hellblade2, with **zero additional captures on every
selection after readiness**, including the first one. These capture totals
include initial exact-boundary captures and background full-frame preparation;
the old totals also include the four failed prefix attempts.

Cold first previews still cost 6,573 ms (Talos) and 14,513 ms (Hellblade2).
Subsequent idle preparation cost another 9,251 ms and 48,501 ms respectively,
including remaining visible thumbnails, the idle delay, full capture, controller
opening and indexing. This work is reported, not hidden in an excluded click;
the optimization targets navigation after preparation and does not claim a
70% cold-start improvement. Automatic timing collection is measured separately.

Evidence: `artifacts/multiqueue-final-20261023/{talos,hellblade2}-prefix`
and `artifacts/multiqueue-release-20261023/{talos,hellblade2}`. Each optimized
directory contains the full selection log, summary and paired comparison.
The four prefix failures per capture remain failures, even though the reused
path can display those selections; display success alone does not establish
their raw-byte correctness.

BF1 regression also completed all 21 selections, including buffer selections:
post-cold content p50/p95 1,086.5/1,249 ms, binding-list p95 27 ms, two total
captures and none after preparation. Its first content took 13,752 ms and
background preparation 21,306 ms. See `artifacts/multiqueue-release-20261023/bf1`.

### Release regression and portable validation

The final Release build succeeded (`build/multiqueue-release-final-build.log`).
The complete serial CTest run finished in 737.18 seconds: **157/159 targets
passed**, with the two existing failures retained:

- `replay_debug_model`: `typedInstructionWatchLifetime`, expected watch status
  `unavailable` at `ReplayDebugModelTests.cpp:202`.
- `class_linkage`: `emptyFunctionTable` crashes with `0xc0000005` near
  `nvGetCompilerInterface`, matching the previous driver-side failure signature.

See `build/multiqueue-release-regression.log` and its JUnit XML. The recovery
target passed all 123 cases (239.047 seconds); resource browsing passed 18 cases
with two optional real-capture cases skipped. Skips are not acceptance evidence:
the separately run real-frame comparisons above retain the Hellblade output
failures. No new regression failure was observed.

The portable directory is `out/FloraGPA-multiqueue-20261023-release`. Its four
application executables match the final Release hashes. With PATH reduced to
Windows directories, GUI version/CLI startup, Talos full native capture, four
cross-DC Before/After resident reads, and Hellblade2 draw timing replay passed.
All four resident reads kept `OpenCapture` count at one; texture previews read
no raw storage. BF1 GUI startup and automatic timing completed, with the
1600x950 screenshot visually checked in
`artifacts/multiqueue-portable-20261023/gui-bf1.png`.

One configuration limitation was retained explicitly: Hellblade2 GUI startup
with the default **4 GiB native allocation budget** fails automatic timing at
creation 521408 (536,870,912 bytes requested after 4,118,872,064 allocated).
The same packaged worker succeeds at **12 GiB**, the budget used for all three
navigation runs. For this sample, set the Resources `Budget` control to
**12288 MiB** and collect GPU metrics again. The default was not silently
raised; this native allocation budget is not a hard RenderDoc VRAM limit.
The failed default-budget check is preserved in
`build/multiqueue-portable-hellblade-default.log`, and the successful timing
report is under `artifacts/multiqueue-portable-20261023/hellblade-timings`.

Module verification accepted six successful native/analysis snapshots, using
only the package, Windows and each capture's Agility SDK, with no external
GPA/Python runtime. Evidence is `modules-talos.json`, `modules-hellblade.json`
and `resident-summary.json` in the same artifact directory. This is a smoke
test on the development machine with a restricted PATH, not clean-machine
certification. The package includes this report and a source-commit/executable
hash manifest in `build-info.json`.
