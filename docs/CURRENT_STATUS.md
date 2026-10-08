# Current capabilities and compatibility

Reviewed **2026-10-08**. The user authorized M4, then M5, while keeping M3 open.
The latest [background byte export correction](BYTE_EXPORT_ACCEPTANCE.md),
implementation `d8f13a1`, moves buffer and raw-resource writes off the UI thread.
Immutable owners survive selection/capture changes, and cancellable 1 MiB staged
writes preserve prior targets on cancellation or failure. Texture export becomes
available again only for the same accepted asset and revision. Six serial CTest
suites pass 270 top-level Qt rows without failures/skips. The 44-file
`out/FloraGPA-byte-export-20261008/` package passes six relocated checks and four
golden/control replays. Only the GUI changes; the 565-registration matrix below
is inherited through unchanged CLI/Worker and 43 identical package files, not
rerun. The baseline pins 203 local evidence files. This does not change replay
semantics or complete M3/M4/M5, clean-host deployment or renewed long-soak
acceptance. The unnecessary full Worker buffer CSV remains the next large-file
improvement; the module migration ledger is unchanged.

The preceding [SM4.0 loop correction](SM40_LOOP_MIP.md), implementation `4c429d2`,
removes four false missing-MinLOD rejections. Checked LOOP/ENDLOOP and
BREAK/CONTINUE paths use a bounded worklist; dimensions read through backedges
still require saved state. Eight untouched original captures pass 288 producer
image checks; hardware/WARP replay accepts five and retains three located
refusals. Six relevant serial CTest suites pass, with 555 explicitly logged Qt
rows without failures/skips. The 44-file `out/FloraGPA-loop-mip-20261008/`
package passes four relocated checks, four golden/control replays and the
40-suite matrix: 565 registrations / 555 hashes, 526 completions / 516 unique
completions, 39 located refusals. All 557 previous preflight reports, execution
counts and deterministic images remain unchanged. Original-player MinLOD
differences and unverified device/configuration scope are retained. The baseline
pins 1,085 evidence files. This does not complete M3/M4/M5, general shader
control-flow coverage, clean-host deployment or renewed long-soak acceptance.
Module totals remain unchanged.

The preceding [output storage exporter](OUTPUT_STORAGE_EXPORT.md), implementation
`49d9084`, checks the accepted byte count, SHA-256 and exact Worker sidecar before
publishing. Both outputs are staged; storage copies use cancellable 1 MiB chunks
in a background job with independent completion. Validation/staging failures and
pre-publication cancellation preserve prior files. Windows lock controls prove
the separate partial-publication boundary and retry. Five serial CTest suites
pass 250 top-level Qt rows without failures/skips, including 111 isolated Worker
scenarios. The 44-file `out/FloraGPA-storage-export-20261008/` package passes six
relocated checks and four golden/control replays. Only its GUI changes, so the
557-registration replay matrix is inherited through the 43 identical remaining
files, not rerun. The baseline pins 188 local evidence files. Other exports,
multi-file transaction recovery, wider stress and clean-host deployment remain
open; M3/M4/M5 and module totals are unchanged.

The preceding [export ownership correction](EXPORT_ASSET_OWNERSHIP.md), implementation
`e6f6323`, retains output storage, PNG, buffer, resource-byte and geometry assets
across capture changes inside file choosers. Old-code controls reproduce exporting
the replacement capture's storage and PNG. Five original-GF2 cases compare all
files and bytes before/after the switch. Three serial CTest suites pass 108 Qt
rows without failures/skips. The 44-file `out/FloraGPA-export-owner-20261008/`
package passes four relocated checks and four golden/control replays; only its
GUI differs. The 557-registration matrix remains inherited through unchanged
CLI/Worker and 43 identical package files. The baseline pins 181 evidence files.
Large-export background I/O/cancellation, complete read integrity, multi-file
failure recovery and other analyzer exports remain open. This is not clean-host,
renewed long-soak or complete M3/M4/M5 acceptance.

The preceding [resource selection correction](RESOURCE_SELECTION_IDENTITY.md),
implementation `61ec9a8`, keeps direct inspection, explicit bindings, project
restoration and accepted image identity consistent. Diagnostic and unavailable
images cannot export the previous resource's raw bytes. Three new isolated
controls reject invalid image resource IDs and verify retry. Seven serial CTest
suites pass 250 top-level Qt rows without failures/skips. The 44-file
`out/FloraGPA-resource-selection-20261008/` package passes five relocated checks
and four golden/control replays; only the GUI changed. The 557-registration
matrix below is inherited through identical CLI/Worker and 43 unchanged files,
not rerun. The baseline pins 344 evidence files. No project-format expansion,
clean-host deployment or renewed long-soak acceptance is claimed; M3/M4/M5 and
module totals remain incomplete.

The preceding [input clone LOD correction](LOD_CLONE_INHERITANCE.md), implementation
`1c84431`, restores input texture experiments at proved current nonzero MinLOD.
Clones retain RESOURCE_CLAMP and inherit current LOD; unknown initial state
still refuses. Twelve original captures check five clone variants and the
original at six Draw boundaries. Native hardware/WARP/injected programs pass
432 original and 2,160 clone image/storage/LOD checks. Ten serial CTest suites
pass 530 explicitly logged Qt rows without failures/skips. The 44-file
`out/FloraGPA-lod-clone-20261008/` package passes four relocated checks, four
golden/control replays and the 39-suite matrix: 557 registrations / 547 hashes,
521 completions / 511 unique completions, 36 located refusals. All prior 545
preflight reports, execution counts and deterministic images remain unchanged.
The baseline pins 2,285 evidence files. The original private-player nonzero-LOD
discrepancy remains diagnostic, with device/configuration equivalence unverified.
This is not clean-host, renewed long-soak or complete M3/M4/M5 acceptance.
Its resource-table selection caption issue is resolved by `61ec9a8` above;
module totals do not change. Completion remains separate from fidelity and
missing information.

The preceding [clamped texture storage acceptance](LOD_STORAGE_READBACK.md),
implementation `22fdac3`, restores full inspection/export at known nonzero
resource MinLOD without changing sampling state. Twelve untouched original
captures verify 1D/2D arrays and 3D storage on hardware/WARP; a preserved draft
counterexample keeps nonzero-LOD input clones explicitly rejected. Nine relevant
CTest suites pass; 470 explicitly logged Qt rows have no failures/skips. The
44-file `out/FloraGPA-lod-storage-final-20261008/` package passes the extended
38-suite matrix: 545 registrations / 535 distinct capture hashes, 509 completions
and 36 located refusals. The prior 533 preflight reports, execution counts and
deterministic images remain unchanged. Helldivers and information-missing
`query_sync_9` retain their existing variable policies. Four relocated package
checks and four golden/control replays pass; 1,906 evidence files are pinned.
Completion and fidelity remain separate: 35 completed registrations have known
capture limitations. Unknown initial state, input-clone LOD inheritance and
wider resource/M5 boundaries remain. This is not new clean-host, committed-source
rebuild or long-soak acceptance; M3/M4/M5 and module totals remain incomplete.

The preceding [metric catalog acceptance](METRIC_CATALOG_ACCEPTANCE.md), implementation
`b2d7562`, moves catalog reading/parsing and complete display-field validation
into cancellable background jobs. A preserved old-code control reproduces
partial publication after a later malformed metric. Ten new isolated scenarios
cover failure, cancellation, switching, close/destruction and retry. Five
relevant suites ultimately pass 150 top-level Qt rows, including all 97 isolated
Worker scenarios and real Intel collection/export checks. The initial live UI
tests timed out in their file-dialog driver; exact-path/accepted-signal correction
`6198c8d` resolves the reproduced focus/selection issue without changing production
dialogs or weakening assertions. All failures and diagnostics are retained.
The 44-file `out/FloraGPA-catalog-20261008/` package changes only its GUI. Seven
relocated/system-PATH checks and four golden/control replays pass; 101 evidence
files are pinned. Replay scope and the 533-case matrix below remain inherited
through identical CLI/Worker files. Synchronous model publication, metric-result
acceptance, other specialized readers, wider sustained testing and clean-host
deployment remain open. This does not complete M3/M4/M5 or expand module coverage.

The latest [process-memory attribution](PROCESS_MEMORY_ATTRIBUTION.md), harness
`bfc7cc8`, adds test-only address/heap metadata and independent evidence checks.
A short control and a 611.793-second/84-cycle recovery run pass 264 strict images
in total; all twelve memory snapshots are complete with no unassociated blocks.
After history/log/cache clears, the sustained run has 355,864 fewer busy heap
bytes than its first paired baseline, but 2,129,920 more bytes of process commit.
The residual is concentrated in heap-associated committed allocations; it is
not an equivalent increase in observed live heap data. Observer effects, address
reuse and copy-on-write ownership limit further attribution. CPU controls and
the ordinary eight-row recovery CTest pass. The existing switch-mip package's
44 hashes are unchanged, so replay scope and matrix results remain as below.
The [baseline](process-memory-baseline.json) pins 662 evidence files. This does
not explain every byte of the separate prior 30-minute run or complete M5,
independent clean-host deployment or wider sustained workflows.

The latest [SM4.0 switch correction](SM40_SWITCH_MIP.md), implementation
`6cb0893`, removes four demonstrated false missing-MinLOD rejections. Checked
SWITCH/CASE/DEFAULT/BREAK paths preserve any queried dimension lane still live
at a join. Eight untouched original captures pass 288 producer image checks;
count-only paths match hardware/WARP replay, while three information-missing
controls retain their located refusals. Five relevant CTest suites pass, including
123 resource-LOD and 70 main-UI rows. Original-player comparisons, packaged
goldens and relocated Unicode/system-PATH Qt retry checks pass their scoped rules.
The 44-file `out/FloraGPA-switch-mip-20261008/` package passes the extended
37-suite matrix: 533 registrations / 523 hashes, 497 completions and 36 located
refusals. All preceding 525 full preflight reports, counts and deterministic
images remain unchanged; Helldivers and `query_sync_9` retain their existing
variable policies. A registration line-ending hash error stopped the initial
gate before the new suite; its failure is preserved, all prior evidence was
revalidated and the remaining suite executed. No runtime or acceptance rule was
relaxed. The [baseline](switch-mip-baseline.json) pins 976 evidence files.
This does not complete M3/M4/M5 or establish a new clean-source build/sustained
soak. Other shader forms, missing capture information and broader M5 work remain.

The preceding [thumbnail acceptance](THUMBNAIL_ACCEPTANCE.md), implementation
`41d5a18`, moves thumbnail report/PNG preparation into cancellable background
jobs and rejects an invalid batch before publishing any icon. A preserved old-code
control reproduced partial publication after a later missing PNG. Complete binding
and subresource identity is now checked. Seven relevant CTest suites pass with
222 top-level Qt rows, including 87 isolated Worker scenarios (12 new).
The 44-file `out/FloraGPA-thumbnail-20261008/` package passes four golden/control
replays and seven relocated system-PATH checks; only the GUI changed. Build and
relocated recovery each check four GF2/BF1 cycles and twelve strict images.
The existing 525 registrations (492 completions, 33 located refusals) are inherited
through identical CLI/Worker binaries, not rerun in this UI batch. A queue review
retains the existing missing-state, identity and discard boundaries; it does not
exhaust all remaining M4 work. Other specialized readers, model/text publication,
exports, memory attribution, broader sustained testing and independent clean-host
deployment remain open. M3/M4/M5 and module counts remain incomplete/unchanged.

The preceding [diagnostic attachment acceptance](DIAGNOSTIC_ATTACHMENT_ACCEPTANCE.md),
implementation `6b01ad8`, reads Coverage/Quad attachments and prepares their
diagnostic images in cancellable background jobs. It rejects overflowing Quad
histogram lengths before multiplication. Eight relevant CTest suites pass with
217 top-level Qt rows, including all 75 isolated Worker scenarios (23 new).
The initial missing-file assertion mismatch is preserved and corrected; no
production rejection was relaxed. The 44-file package
`out/FloraGPA-diagnostic-20261008/` passes seven relocated-system-PATH checks and
four golden/control replays. Only its GUI differs from the matrix-tested
branch-mip package; no new core replay coverage or full-matrix rerun is claimed.
Build and relocated recovery each check four GF2/BF1 cycles and twelve strict
images. This is not a renewed 30-minute soak. Other specialized consumers,
large model/text publication, exports, broader memory attribution and independent
clean-host deployment remain open. M3/M4/M5 are incomplete.

The preceding [same-source release acceptance](CURRENT_SOURCE_RELEASE_ACCEPTANCE.md)
builds production and eight selected test targets from archived revision
`078d638`. The 44-file package `out/FloraGPA-verified-source-20261008/` passes
334 relocated Qt rows, four further Windows-platform large-output checks, two
shipping GUI startups, four golden/control replays and the complete 525-case
matrix. All prior preflight reports, counts and deterministic images are unchanged;
Helldivers and information-missing `query_sync_9` retain their variable policies.
A new 30-minute/248-cycle recovery run passes 744 strict image checks and all
1,737 immutable progress snapshots. Process handles start/end at 248; private
bytes rise about 23.3 MiB (GF2) and 19.2 MiB (BF1), without complete attribution.
This establishes evidence for that build on this host, not independent clean-host
deployment or a leak-free claim. Runtime sources and compatibility scope were
unchanged within that batch. Coverage/Quad attachment preparation and an independently reproduced
malformed histogram-length boundary were still prototype work in that package;
the later accepted GUI correction is recorded above. M3/M4/M5 remain incomplete.

The latest [SM4.0 branch correction](SM40_BRANCH_MIP.md), implementation
`b8ab3dc`, removes four demonstrated false missing-MinLOD rejections. Checked
IF/ELSE/ENDIF edges track unused dimension lanes across both outcomes and joins;
one-arm overwrites do not erase another path's dependency. Eight untouched
original captures pass 288 producer image checks across hardware, WARP and
injected runs. Four new count-only workloads now match the application and
observed original-player images; three true missing-state cases still reject at
Draw 23 / resource 24. Explicit-LOD mode 17 remains equal to the application and
different from the local original player. Adapter/GUI equivalence is unverified.
Five related CTest suites pass, including 106 resource-LOD and 69 main-UI rows.
The proof remains bounded to 256 instructions and 64 nested IF blocks, with no
loops, calls or guessed branch outcomes. Missing data is not reconstructed.
The full 36-suite gate passes 525 registrations / 515 unique captures: 492
completions (482 unique) and 33 located rejections, from 1,050 ordinary attempts,
684 controls and 826 resource exports. All preceding 517 preflight reports,
execution counts and deterministic images remain unchanged. Fidelity assessments
are 60 passed / 32 capture-side mismatches / 10 information-missing / 423
unassessed; 35 completed cases retain known capture limitations. Package
`out/FloraGPA-branch-mip-20261008/` passes relocated system-PATH Qt retry workflows
and four unchanged golden/negative checks. See the
[pinned baseline](branch-mip-baseline.json); M3/M4/M5 remain incomplete.

The preceding [analyzer payload extension](ANALYZER_PAYLOAD_ACCEPTANCE.md),
implementation `f70d65a`, moves eight additional output files off the event
thread: Quad, coverage, timings, statistics, replayed pipeline, predication, IA
geometry and shader output geometry. Missing, truncated, malformed and non-object
payloads fail before publication. Native payloads retain full integer precision;
the existing request-owned cancellation and revision guard applies throughout.
This prepares the first JSON read; Coverage and Quad still reread their reports,
load attachments and decode diagnostic images synchronously in their consumers.
Those paths are not covered by the geometry responsiveness measurements.
29 reader rows and 52 isolated Worker scenarios pass, including nine new geometry
cases. Seven consumer CTest suites pass, including 68 main-UI rows and eight
GF2/BF1 recovery cycles / 24 strict image checks. The recovery parent has three
intentional child-entrypoint skips. No execution semantics or supported capture
scope changed; the 72/117/15 module ledger remains unchanged. Model/widget
publication, retained JSON memory, other specialized readers and exports remain
separate work. See the linked acceptance record for package evidence and limits.
Package `out/FloraGPA-payload-20261008/` passes five relocated system-PATH checks,
four golden/negative checks with unchanged hashes/counts, and four further
recovery cycles / twelve strict images in 28.940 seconds. Only the GUI executable
differs from the preceding report package. This is same-host short-run evidence,
not independent clean-machine deployment or renewed sustained-soak certification.

The preceding [common Worker report change](WORKER_REPORT_ACCEPTANCE.md) moves
report I/O and Qt/native JSON parsing off the event thread, reusing the prepared
native replay report for export. Request-owned cancellation, revision checks and
directory lifetime prevent obsolete publication. Implementation `5f1464e`; test
synchronization `c4dd4d4`. The 21 reader rows and 43 isolated Worker scenarios
pass, including nine new report cases and five 64 MiB success/cancel/switch/close/
destroy workflows. Focused and relocated Windows runs record maximum busy
heartbeat intervals of 15 and 13 ms; these are host observations, not latency
bounds or full large-file acceptance.

All 24 distinct related CTest suites eventually pass, including 68 main-UI rows
and eight real-frame recovery cycles / 24 strict images. Initial failures and
retries are preserved: a short counter fixture set, import tests racing an active
preview, a file-picker helper waiting with no selected filename, and a reused
Quad output directory. One identified waiting test process was deliberately
stopped. Final tests wait for preview completion and use bounded filename entry;
production import guards remain unchanged. Two optional Intel live-collection
cases remain skipped, as do two intentional child-only recovery entrypoints in
the parent CTest. The relocated parent-only selector has no skips.

Package `out/FloraGPA-report-20261008/` has 44 files; only the GUI binary changes
from the preceding SM4.0 package. Five relocated-package checks, actual Windows
GUI GF2/BF1 screenshots, four golden/negative checks and four further recovery
cycles / twelve strict images pass. CLI and Worker remain byte-identical, so the
517-registration matrix below is retained, not rerun in this batch. See the
[report acceptance baseline](worker-report-acceptance-baseline.json). Specialized
payload parsing, model/widget publication, exports, wider sustained testing and
independent clean-machine acceptance remain open. Qt's single parse call is not
interruptible; cancellation is checked around it. M3/M4/M5 remain incomplete.

The preceding [SM4.0 mip-query correction](SM40_MIP_DIMENSIONS.md) proves that
dimension lanes from a full-vector RESINFO are unused within a bounded,
understood straight-line program. Three untouched-original workloads previously
rejected now match complete application images; live dimensions and same-texture
sampling still require known MinLOD. Shaders, capture bytes and thresholds are
unchanged. Producer `02ecf8e`; implementation `559db57`.

The current 35-suite matrix passes **517 registrations / 507 unique files**:
**487 native completions (477 unique)** and **30 located rejections**, across
1,034 ordinary attempts, 684 controls and 826 strict resource exports. All 511
preceding cases retain complete preflight reports, execution counts and
deterministic images. Observed variation is retained for Helldivers and the
already-diagnosed missing Query/End sample `query_sync_9`. The latter is capture
information loss, not proven application nondeterminism. Fidelity assessments
are **55 passed / 32 capture-side mismatches / 7 information-missing / 423
unassessed**; 35 completed cases retain known capture limitations. These are
sample assessments, not an overall GPA feature-completeness percentage.

Five related CTest suites pass, including 89 LOD and 68 GUI rows. Six new
originals have 216 hardware/WARP/injected producer-image checks; original private
player comparisons retain the explicit-LOD disagreement and unverified adapter
boundary. The first full-gate attempt passed 511 cases then failed a new-manifest
CRLF/LF hash check. Both the failed run and the corrected complete rerun are
preserved. Package `out/FloraGPA-sm40-mip-20261008/` has 44 files; only GUI, CLI
and Worker change from the buffer package. Relocated system-PATH Qt retry checks
and four GF2/BF1 golden/negative checks pass. See the
[SM4.0 acceptance baseline](sm40-mip-baseline.json). M3/M4/M5 remain incomplete;
this package has no new sustained soak or independent clean-machine acceptance.

The preceding [buffer acceptance change](WORKER_BUFFER_INTEGRITY.md) moves buffer
reads and SHA-256 checks off the event thread. It rejects inconsistent file
lengths/hashes and metadata that does not match the requested resource, range or
event boundary. Shared cancellation/revision guards prevent obsolete publication.
Eight related suites pass, including 29 reader rows and 34 isolated Worker fault/
recovery scenarios. Implementation `4fe2ee7`; the 44-file package is
`out/FloraGPA-buffer-20261008/`. Only the GUI binary changes from the preceding
display-preparation package; replay semantics and the 511-registration matrix
remain unchanged. Report/model work and other analyzer payloads remain outside
this background byte reader.

Seven relocated-package checks also pass with system-only PATH, as do four
golden/negative checks and four GF2/BF1 recovery cycles with twelve strict image
checks. See the [buffer acceptance baseline](worker-buffer-integrity-baseline.json).

The preceding [display preparation change](IMAGE_DISPLAY_PREPARATION.md) removes a
discarded RGB conversion before replay/texture RGBA publication and prepares the
Qt painting format in the existing background validation task. Original pixels,
image-integrity checks and cancellation/revision guards remain intact. An
8192×4096 comparison measures about 650–680 ms for the old GUI path and 23–25 ms
for prepared publication; these are host observations, not a general latency bound.
Seven related suites pass after correcting one resource-test fixture selection;
native Windows/offscreen portable checks, 23 Worker fault scenarios, four golden/
negative checks and relocated GF2/BF1 recovery pass. Package:
`out/FloraGPA-image-display-20261008/` (44 files), implementation `8c02de4`.
Only the GUI binary changes from the previous same-workspace mip-count package.
Replay compatibility remains the 511-registration scope below; no full GPU matrix
rerun or new format support is claimed. Other diagnostic image paths, overlays,
models and general UI latency still require work.

The preceding [persistent Qt recovery batch](PERSISTENT_QT_SOAK.md) passes 246 cycles
in 30 minutes 5 seconds, with 738 strict GF2/BF1 image checks. One QtTest window
exercises production UI/Workers through cancellation, failed open, retry, event
navigation and Final replay. Immutable progress evidence and timeout process-tree
cleanup have separate negative controls. Eight recovery Qt rows and three CPU
process controls pass. The first progress-write failure is preserved as a failed
run, not included in successful acceptance. Private memory grows by about
19–24 MiB. Subsequent [retention controls](QT_RETENTION_CONTROL.md) isolate test
history, application logs and Qt's bounded icon cache. A final 40-cycle control
releases approximately 0.37, 0.80 and 1.64 MiB respectively; allocation stacks
confirm the growing icon images are freed by cache clearing. This does not account
for every byte of the sustained-run private-memory increase or certify all paths
leak-free. Production cache/log behavior is unchanged. Handles and QObject counts
show no sustained accumulation.

Fresh committed-source configure/build/package passes on this host: 1,049 files
at `0db936e`, 44 packaged files, four golden/negative checks, the six-case mip gate
and two shipping-GUI startup checks. That batch's package:
`out/FloraGPA-clean-build-20261008/`. Production replay source is unchanged.
The full 511-registration matrix below is not claimed rerun on these fresh
binaries. Offscreen QtTest recovery does not certify every shipping-GUI workflow
or independent clean-machine deployment. M3/M4/M5 remain open.

The preceding [mip-count compatibility correction](MIP_COUNT_LOD_COMPATIBILITY.md)
allows verified RESINFO count-only reads despite omitted initial resource MinLOD.
Three untouched-original modes previously rejected now match full application
images; dimensions and sampling the same clamped resource still reject when
LOD is missing. At that checkpoint, SM4.0 full-vector lowering remained
conservative; the later bounded proof above narrows that limitation. Six originals,
216 hardware/WARP/injected producer images, actual replacement shaders
and malformed programs define this narrow boundary. Original-player equality
is observed for the three corrected modes, not asserted for all configurations.

That preceding 34-suite matrix passes **511 registrations / 501 unique files**:
**483 native completions (473 unique)** and **28 located rejections**, with
1,022 ordinary attempts, 684 controls and 826 strict resource exports. All prior
505 cases retain their complete preflight reports, execution counts and
deterministic images. Thirty-five completed cases still have known capture
limitations. Capture-fidelity assessments remain separate: 51 passes, 32 known
capture-side mismatches, five information-missing cases and 423 unassessed
registrations. These counts are not a GPA feature-completeness percentage.
That batch's package: `out/FloraGPA-mip-count-20261008/` (44 files).
Implementation: `d340c8d`; five related CTest suites pass, including 76 resource
LOD and 67 Qt rows. Four packaged golden/negative checks and relocated
system-PATH Qt retry checks pass. Evidence and remaining proof limits are pinned
in the [mip-count baseline](mip-count-lod-baseline.json).

The preceding [asynchronous image validation](ASYNC_IMAGE_VALIDATION.md) moves
replay/texture artifact hashing and decoding off the Qt event thread. Pending
validation remains busy and cancellable; superseded results cannot replace the
retained replay image. Request-owned temporary storage survives window closure
until its reader exits. Five 4096×4096 workflows verify event-loop response,
cancel/switch/close/destroy and full-image acceptance; all 11 chunk/row/decoder
boundary interruption positions allow exact retry. Five CTest suites pass,
including 66 Qt rows and 23 Worker fault scenarios. Relocated system-PATH checks
have no skips; four packaged golden/negative checks also pass. Implementation:
`72404ff`; package: `out/FloraGPA-image-async-20261007/` (44 files). Only the GUI
binary changed in that batch; the new mip-count matrix above now supersedes
its 505-registration / 495-file baseline. Mid-call decode/I/O cancellation, synchronous ImageView/model
work and independent clean-machine acceptance remain open; the sustained recovery
batch above adds a scoped 30-minute check, not general long-duration acceptance.

The preceding [Query completion correction](QUERY_COMPLETION.md) restores CPU/GPU
ordering for successful GetData with a complete saved predicate interval. Three
untouched original captures reproduce earlier-copy corruption despite complete
Map payloads. Corrected predicate replay matches the independent application's
full image and buffer oracle. Two ordinary Query captures omit the query/End
boundary; they retain located preflight/runtime notices and compact Qt warnings,
and are **not accepted as application-faithful**. Local original-player outputs
also differ from the application; its GUI and selected adapter remain unverified.

That revision's 33-suite matrix passed **505 registrations / 495 unique files**: **479 native
completions (469 unique)** and **26 located rejections**, with 1,010 ordinary
attempts, 684 controls and 826 strict resource exports. All preceding 502 cases
retain outcomes, deterministic images and existing execution counts. Offline
comparison checks all 492 prior unique files; only GetData handling and 41
missing-completion warnings change. The 35 known capture limitations remain
separate from completion. Capture-fidelity assessments are 47 passes, 32 known
capture-side mismatches, three information-missing cases and 423 unassessed
registrations; these are not an overall correctness percentage.

Six relevant CTest suites pass after correcting a tooltip-length regression:
33 Query, 43 predication, 19 SO, 173 preflight, 21 cancellation and 66 Qt rows,
with no final skips. Four packaged golden/negative checks and four relocated
system-PATH Qt rows pass. Runtime: `c7cb7cd`; GUI: `734b359`; package:
`out/FloraGPA-query-sync-final-20261007/` (44 files). The [pinned baseline](query-completion-baseline.json)
retains original capture hashes, failed intermediate checks and final evidence.
Missing ordinary Query boundaries, texture-diff pitches, retained command-list
semantics, general cancellation latency and independent clean-machine acceptance
remain open. M3/M4/M5 and the 72/117/15 module ledger remain incomplete.

The preceding [worker image integrity correction](WORKER_IMAGE_INTEGRITY.md) rejects
nine malformed display-artifact combinations previously accepted as successful
replay. PNG dimensions/pixels, raw RGBA length/hash and report availability must
agree before Qt replaces an output. Failure preserves the previous image and
label, and real-Worker retry succeeds. Five CTest suites pass; relocated system-
PATH checks pass 27 artifact rows, 18 worker scenarios and four real GF2/BF1
recovery cycles. Four packaged golden/negative checks also pass. Runtime GUI:
`ea98485`; package: `out/FloraGPA-worker-image-20261006/` (44 files). Only the GUI
binary changes; CLI/Worker and all other deployed files retain identical hashes.
No full GPU matrix rerun or expanded format support is claimed. Specialized
analyzer/storage failures, synchronous image-processing latency and independent
clean-machine acceptance remain open.

The preceding [READ Map synchronization correction](MAP_READ_SYNCHRONIZATION.md)
restores saved CPU/GPU ordering before later NO_OVERWRITE writes. Eight untouched
original captures pass independent native hardware/WARP resource oracles; two
also expose wrong earlier-copy images in the previous replay and local original
private player. Corrected replay matches the application. The original GUI and
its selected adapter were not verified; this is not a blanket original-GPA claim.

The resumed 32-suite matrix passes **502 registrations / 492 unique files**:
**476 native completions (466 unique)** and **26 located rejections**, with 1,004
ordinary attempts, 684 controls and 808 resource exports. Previous 494 cases keep
their outcomes, deterministic images and existing execution counts. READ coverage
changes from metadata to execute; no other full preflight fields change. The 33
known capture limitations remain separate from replay completion. Fidelity has
46 assessed passes, 32 capture-side mismatches, one information-missing case and
423 unassessed registrations; these are not an overall correctness percentage.

Seven related CTest suites, 60 Map rows, 65 Qt rows, four packaged golden/negative
checks and the relocated Draw/Final workflow pass without skips. Runtime:
`15ddac1`; package: `out/FloraGPA-read-map-sync-20261006/` (44 files).
Missing texture-diff pitches, retained command-list semantics, complete M4/M5
acceptance and independent clean-machine deployment remain open.

The preceding [context/lifetime cancellation](LIFETIME_AUDIT_CANCELLATION.md) covers
implicit context recovery and unused SO/CB searches, including nested Map audit
and safe cache publication. Eight new lifetime fixtures cover 1,969 preflight
interruptions; the five preceding semantic fixtures cover 6,530. Direct context
and lifetime tests interrupt 129 additional positions with same-frame retry.
Seven CTest suites, four packaged golden/negative checks and the portable Qt
preflight panel pass without skipped rows. All 484 unique files retain exactly
equal full preflight reports and exit codes. Runtime: `f940832`; package:
`out/FloraGPA-lifetime-cancel-20261006/` (44 files). No new replay capability or
full GPU matrix rerun is claimed. Individual decoders, synchronous I/O and GUI
model population still prevent a universal cancellation latency bound.

The preceding [occluded Present TEST correction](OCCLUDED_PRESENT_TEST.md) admits
two original blt-model captures previously rejected at event 27. Saved
`DXGI_STATUS_OCCLUDED` with TEST preserves RTV identity/storage and permits later
in-frame writes; ordinary non-S_OK Present and unverified flags still reject.
Native hardware/WARP and injected producer bytes, original-player comparisons,
27 Present test rows and 64 UI rows without skips pass. The full 31-suite matrix
passes 494 registrations / 484 unique files: 468 native completions (458 unique)
and 26 located rejections, including 988 ordinary attempts, 684 controls and 728
resource exports. All preceding 492 cases retain complete preflight reports,
execution counts and deterministic image hashes. The 33 known capture limitations
remain separately reported. Runtime: `f46197f`; package:
`out/FloraGPA-occluded-present-20261006/` (44 files). M3/M4/M5 remain open.

The preceding [semantic audit cancellation](SEMANTIC_AUDIT_CANCELLATION.md) extends
preflight cancellation through nine bulk audit families. All 6,012 injected
preflight cancellation positions preserve honest diagnostics and permit exact
retry; nine related CTest suites and four packaged golden/negative checks pass.
Complete preflight reports for all 482 unique registered files are unchanged.
The portable preflight-panel harness also passes failure/cancel/switch/retry.
Runtime: `14a7860`; package: `out/FloraGPA-semantic-cancel-20261006/` (44 files).
Its implicit recovery/lifetime gap is covered by the newer batch above;
individual decoders and GUI model population remain incomplete. No new replay capability is claimed;
the preceding complete GPU matrix remains authoritative, and M3/M4/M5 stay open.

The preceding [per-slot counter correction](COUNTER_SLOT_COMPATIBILITY.md) admits
mixed UAV workloads when checked shader code proves the unavailable hidden
counter is not consumed at that view's bound slots. Two unmodified original
captures reproduced false rejection; an explicit-reset companion is the control.
Three modes pass 72 native/injected producer frames, complete buffer oracles,
original-player comparisons and hardware/WARP tests. Direct counter consumers
still reject missing values, including an edited shader that starts using u1.
Unfamiliar operands and dynamic linkage remain conservative.

The continuous matrix now passes **492 registrations / 482 unique captures**:
**466 native completions (456 unique) and 26 located rejections**, in 30 suites,
984 ordinary attempts, 680 controls and 716 resource exports. All previous 489
findings and deterministic hashes are unchanged; Helldivers keeps its separate
variation policy. Completions still include 33 known capture limitations. The
new independent-image fields cover five cases: four matches, one known missing
MSAA input difference; 487 remain unassessed on that axis.

Eight related CTest suites, 34 counter rows, 63 Qt rows without skips, a portable
three-capture UI workflow and four isolated-PATH golden/negative checks pass.
Runtime: `d09352a`; package: `out/FloraGPA-counter-slots-20261006/` (44 files).
The [baseline](counter-slot-baseline.json) pins the original capture and regression
evidence. M3/M4/M5 remain incomplete; slot analysis does not prove branches
unreachable, recover missing inputs or establish clean-machine deployment.

The preceding [MSAA initialization notices](MSAA_INITIALIZATION_NOTICES.md) preserve
materialized resource/data identities in CLI/Worker output even after Resolve
produces a single-sample image. Qt displays a compact count and groups repeated
reasons in its tooltip. These are resource-level notices, not proof of image
corruption or final-output dependency. Both original MSAA captures retain exact
prior pixels and execution counts; GF2/BF1 goldens and disabled-Draw controls pass.
BF1 has 20 notices matching existing preflight findings; its golden is unchanged.

Seven relevant CTest suites pass across the initial batch and a fixture repair;
11 frame-output and 44 Map rows pass. The full Qt run passes 60 rows with one
optional skip, separately exercised successfully. Final system-PATH portable
Qt tests pass seven rows without skips, including original MSAA, BF1 grouping
and capture-switch clearing. The initial invalid READ-as-write fixture failure
is retained and now has an explicit negative control. Package:
`out/FloraGPA-msaa-notices-final-20261006/`; runtime reporting `168fca4`, GUI
`8169c14`. GPU execution semantics are unchanged; the full corpus is not newly
rerun. See the [pinned baseline](msaa-initialization-notice-baseline.json).
M3/M4/M5 remain incomplete.

The preceding [capture-fidelity reporting acceptance](CAPTURE_FIDELITY_REPORTING.md)
separates native completion, original-player agreement and application fidelity.
A fresh continuous 29-suite run passes all expected outcomes for 489 registrations
/ 479 unique files: 463 native completions and 26 located rejections, with 978
ordinary attempts, 674 control runs and 692 resource exports. All previous
findings and deterministic hashes are unchanged. The completions include **33
known capture limitations**: 32 capture-side mismatches and one missing-input
MSAA file. They must not count as faithful application reconstruction.

Capture assessment labels are 423 unassessed, 33 passed within documented scope,
32 capture-side mismatches and one information loss. Only the two MSAA cases
currently carry the new independent application-image reference: initialized
matches, retained differs. Both match the original player's repeated output;
its adapter is unidentified. A fresh uninjected producer passes 24 frames and
all four sample-plane checks; 25 CPU harness tests pass. This batch changes
reporting and acceptance metadata, not C++ execution. See the
[pinned baseline](capture-fidelity-baseline.json). M3/M4/M5 remain incomplete.

The preceding [source-build and packaging acceptance](SOURCE_BUILD_AND_PACKAGING.md)
builds all production targets from 1,005 committed source files with system PATH,
then packages with the configured Qt/VS installation instead of workstation paths.
The 44-file package excludes host-PATH DX12 compiler DLLs, retains the DX11
compiler, and refuses occupied destinations. Four golden/negative replays, two
actual production GUI open/replay workflows, module audits and four build-process
checks pass. The initial harness PATHEXT error is preserved separately from the
successful continuous run. Package: `out/FloraGPA-clean-build-20261006/`.
That batch changed no C++ runtime semantics and did not rerun the full GPU
matrix; the fresh continuous matrix is recorded above. This is clean-source validation on the developer host; independent
machine deployment, broader failures and long-duration acceptance remain open.
See the [pinned baseline](source-build-packaging-baseline.json).

The preceding [counter-usage correction](COUNTER_USAGE_COMPATIBILITY.md) permits
indexed access to a Counter UAV when checked executable shader code proves the
hidden counter unused. Three new unmodified original captures include the
previously rejected trigger and two controls. All match the producer's exact
buffer/image oracle and the original player's repeated image output; its replay
adapter remains unidentified. Actual counter consumption, CopyStructureCount
and counter inspection still require a known value. Shader replacements are
checked against their actual native bindings. No counter value is invented.

The registry now contains **489 registrations / 479 unique hashes**, including
**463 default replay-positive registrations (453 unique) and 26 rejection
files**. All 29 suites pass their expected outcomes: 978 ordinary attempts,
674 diagnostic control runs and 692 resource exports. The preceding 486
registrations retain their diagnostic findings, outcomes and deterministic
image hashes. Helldivers retains its separate variability policy. A final-suite
manifest newline-hash error interrupted the first runner after 28 passing suites;
that suite was run separately after correction and all saved results were
rechecked. This is a composed serial acceptance record, not a claim that the
interrupted runner completed successfully. Package:
`out/FloraGPA-counter-usage-20261006/`. The module ledger remains
72 ported / 117 partial / 15 pending. Runtime: `c386d3a`. Eleven related CTest
suites, 27 counter rows, 59 Qt interaction rows without skips and four isolated
PATH package golden/negative checks pass. See the
[pinned baseline](counter-usage-baseline.json). M3/M4/M5 remain incomplete.

The preceding [worker failure recovery correction](WORKER_FAILURE_RECOVERY.md)
preserves unterminated final stderr errors and retains the actual timeout reason
through worker completion. Both defects are reproduced before the fix. Eight
fast fault cases pass; a separate native-Qt, system-PATH portable run passes nine
cases including the real 180-second timeout. Each case retries the real Worker
and checks the previous image exactly, completion count and restored UI actions.
Runtime: `757e9a2`; package: `out/FloraGPA-worker-recovery-20261006/`.
Four related UI CTest suites, both optional original-capture UI workflows and
four packaged golden/negative checks pass. Only the GUI executable changes from
the preceding package. These controlled
faults expand M5 recovery evidence without changing replay compatibility. Broader
analyzer/driver/storage failure paths, long-duration operation and independent
clean-machine deployment remain open. See the
[pinned baseline](worker-failure-recovery-baseline.json).

The preceding [Qt action retention fix](QT_ACTION_RETENTION.md) removes repeated
internal tool-button connections under the verified Qt 6.11.2 runtime. A direct
counterexample and allocation-stack comparison identify the source: the selected
live connection group grows from 1,398 to 5,926 blocks before the fix, while
remaining at 304 blocks after it. Dynamic menus, user callbacks and action state
are covered by regression tests. The fix is scoped to the MainWindow subtree and
coalesces cleanup after normal event processing; Qt DLLs are unchanged.

A separate portable 40-cycle recovery run passes 80 strict GF2/BF1 Final-image
checks. BF1 heap growth falls from 3.31 MB to 0.72 MB; clearing only retained logs
releases 0.74 MB. Warmed handles, GDI/USER objects and Qt object class counts stay
fixed. This closes the identified connection accumulation, not every potential
memory issue. Runtime: `4b858b6`; package:
`out/FloraGPA-qt-action-recovery-20261006/`. Only the GUI executable differs from
the preceding package. Four UI CTest suites, both optional original-capture UI
workflows and four packaged golden/negative checks pass. See the
[pinned baseline](qt-action-retention-baseline.json).
M3/M4/M5 and independent long-duration/clean-machine acceptance remain open.

The preceding [worker shutdown correction](WORKER_SHUTDOWN_AND_RETENTION.md)
prevents a second close of the Windows Job handle when waiting for the worker
reenters its completion callback. A before/after API-result observer verifies
the defect and fix; child termination, three CTest UI suites, both optional
original-capture UI workflows, four packaged goldens/negatives and five portable
shutdown checks pass. Runtime: `4495d73`; package:
`out/FloraGPA-worker-shutdown-20261006/`. Only the GUI executable changes from
the preceding package. The [baseline](worker-shutdown-retention-baseline.json)
also pins a 40-cycle heap investigation: QObject counts stay constant, while
heap busy allocations grow and clearing logs explains only part of the increase.
The Qt connection allocation source is now attributed and corrected above;
broader lifetime and longer-duration acceptance remain open. This is not a
declaration of leak-free behavior or a completed M5.

The preceding [capture-load recovery change](CAPTURE_LOAD_RECOVERY.md) makes the
container scan and full-file hash cancellable, discards cancelled queued results,
and preserves the current capture and its compatibility report after cancelled or
failed opens. Specific parser errors survive the asynchronous boundary. Runtime:
`3b45d40`; package: `out/FloraGPA-load-recovery-20261006/`. All 476 unique captures
retain identical complete preflight reports, and four packaged GF2/BF1 golden
and negative checks pass. A synthetic sparse file verifies record addressing
beyond 4 GiB and full-file SHA-256 against an independent oracle. This change
does not broaden replay compatibility or re-run the preceding full GPU matrix.
Six relevant CTest suites pass; both initially skipped optional UI workflows
also pass with their original fixtures. A 258.78-second portable test completes
40 cancel/retry/navigation cycles and 80 strict Final-image checks. Warmed handle
and GUI-object counts stay fixed, while private-memory growth remains an open
heap/retention investigation. See the [pinned baseline](capture-load-recovery-baseline.json).
M5 still needs longer runs, broader recovery coverage and independent deployment.

The preceding [normalized predication recovery](NORMALIZED_PREDICATION_AUDIT.md)
restores eight unchanged original captures that previously failed because the
Predicate descriptor was absent. A linked capture-marker record proves that GPA
normalized the saved comparison using a completed query. FloraGPA now replays
that recorded condition through native predication without inventing the absent
descriptor or historical query value. Preflight locates the proof; Qt displays
**Captured condition** and keeps the query result unavailable. Missing or
ambiguous proof still rejects. This is scoped to pinned GPA 2025 R1 evidence.

That batch had **486 registrations / 476 unique hashes**, with **460 default
replay-positive registrations (450 unique) and 26 rejection files**. This is a
sample matrix, not full API coverage. The 32 dynamic-CB loss cases still validate
only saved contents. Module counts remain 72 ported / 117 partial / 15 pending.
Package: `out/FloraGPA-captured-conditions-final-20261006/`; runtime: `c4e3146`.
The complete 28-suite package matrix passes: 972 ordinary attempts (920 positive,
52 located rejections), 668 control runs and 692 resource exports. Only the eight
recovered files change their preflight findings; all previous deterministic image
hashes remain equal. Helldivers retains its separate variability policy. All 16
Predicate files match producer pixels twice; the original player still fails to
open the eight missing-descriptor files and agrees on the other eight. No
matched-device equivalence is asserted. Another 32 hardware/WARP original runs,
48 inspections, five related CTests, three Qt workflows and four GF2/BF1
golden/negative checks pass. The producer passed 384 native/injected frames.
See the [pinned baseline](normalized-predication-baseline.json). M3/M4/M5 are not
complete: unproven Predicate normalization branches, broader resource/query
boundaries, long-duration recovery, large-file experience and independent
deployment remain open.

The preceding [Predicate boundary audit](INITIAL_PREDICATE_BOUNDARY_AUDIT.md)
added the 16 original files and distinguished replay baseline values from actual
query results. Its package `out/FloraGPA-predicate-boundaries-20261006/` (runtime
`140cec7`) had 452 positives and 34 rejections. Those historical rejection counts
are superseded above; its captured-result provenance remains in force.

The preceding [initial-counter correction](INITIAL_COUNTER_BOUNDARY_AUDIT.md)
rejects missing frame-before UAV counts at consumption. Buffer inspection keeps
available bytes and displays unknown counters as Unavailable; the existing editor
supports explicit initial values without supplying a guessed default. Four
original captures now reject normally and recover exact bytes/counts only with
producer-backed experiment values. The fresh original observer reproduces the
original player's incorrect zero initialization despite equal final pixels.

That batch had **470 registrations / 460 unique hashes**, with **444 default
replay-positive registrations (434 unique) and 26 rejection files**. Four historical
image-only positives are reclassified because their necessary initial counts are
not captured. Module counts stay 72 ported / 117 partial / 15 pending.
Package: `out/FloraGPA-resource-boundaries-20261006/`; implementation: `cabfb39`.
The complete 27-suite package matrix passes: 940 ordinary attempts (888 positive,
52 located rejections), 668 control runs and 692 resource-boundary exports.
Six related CTest suites, Qt failure/recovery and counter-history checks, four
goldens and a fresh 48-frame self-owned counter producer pass. The baseline
keeps these results separate from the earlier CPU-only comparison.
M4/M5 are not complete; predicate/frame-before history, broader resource boundaries,
long-duration/recovery/large-file testing and independent deployment remain open.

The preceding [saved Map write correction](MAP_WRITE_DATA_AUDIT.md)
shares subresource/data validation between preflight and replay and rejects a
successful Map with no saved data identity instead of silently skipping it.
Forty-four Map cases, seven CTest suites, 192 planar comparisons and four serial
GF2/BF1 checks pass. Four unchanged original Map captures also pass 16 strict
producer/native resource comparisons and eight fresh original-player runs.
All 460 registered unique captures retain their prior preflight findings. This
does not certify all GPU paths, unsaved pitches, M3 or M5 deployment/stability.

The preceding [texture initial-data correction](TEXTURE_INITIAL_DATA_AUDIT.md)
rejects empty, truncated or oversized saved single-sample initializers before
native texture creation, with texture/data identities in preflight diagnostics.
Fourteen focused cases, nine CTest suites and four serial GF2/BF1 golden/negative
checks pass. All 460 unique registered captures retain exactly the same preflight
findings as before; this is CPU coverage, not a full GPU corpus rerun. Inventory,
module ledger, UI and release package are unchanged. M4 and M5 remain incomplete.

The preceding [Execute dispatch search](EXECUTE_DISPATCH_SEARCH.md)
rules out the pinned player's literal Execute candidate as an internal MSAA
helper and identifies a shim forwarding thunk omitted by an unwind-only scan.
The known traditional Execute slots remain empty; operand binding and a qualifying
unmodified traditional-list capture remain unresolved. This is bounded static
evidence, not a whole-program absence proof. Five new scanner tests and all 48
related evidence tests pass; production code, corpus and package are unchanged.
A [complete registered-inventory audit](registered-list-inventory-baseline.json)
also verifies all 470 registrations / 460 hashes across 27 manifests and finds
zero traditional list resources or Execute records. This checks index contents,
not GPU behavior or external files. The M3/M4 priority exception was subsequently
authorized on 2026-10-06; traditional-list execution remains unaccepted.

The preceding [initial scheduling model](INITIAL_SCHEDULE_MODEL_AUDIT.md)
adds independent C++ callback-order reconstruction under explicit successful
initialization assumptions. All 19,387 real original callbacks across three
unchanged captures match, including ordering that differs from category/ID sorting.
Another 56 native CPU schedules, seven CTest suites, 43 evidence tests and four
serial golden/negative checks pass. This is a scheduling model, not traditional
list GPU execution. Corpus counts, UI and package remain unchanged. See the
[baseline](initial-schedule-model-baseline.json).

The preceding [traditional list dependency batch](LIST_DEPENDENCIES_AUDIT.md)
adds two C++ read-only collector rules: Command List parent and Execute owner.
All 80 native marker calls match C++; 126 preceding marker cases, six CTest
suites, 35 evidence tests and four serial golden/negative checks pass. These are
metadata results, not traditional-list GPU acceptance. Counts remain 470
registrations / 460 hashes; UI, GPU execution and package are unchanged. See the
[baseline](list-dependencies-baseline.json).

The preceding [original cache version study](INITIALIZATION_VERSIONS_AUDIT.md)
verifies nine real-player cache clones (58,161 copied nodes), 12 identical-payload
reloads and 18 native initialization callbacks across three unchanged captures.
Default pixels and complete ERG dispatch counts remain equal before/after.
Four CPU primitive cases and 35 evidence tests pass. This establishes selected
GPA playback/experiment version semantics, not captured temporal resource history
or traditional-list execution. Production code, corpus counts, UI and package
are unchanged. See the [baseline](initialization-versions-baseline.json).

The preceding [initial dependency graph recovery](INITIALIZATION_GRAPH_AUDIT.md)
adds the CPU-only `initialization-graph` command and 33 resource/state/data
collector layouts. All 19,387 initial nodes across three unchanged captures match
original ordered references and dependency sets; 126 native marker probes match
C++ as well. Six CTest suites, 23 evidence tests and four serial golden/negative
checks pass. These are initial metadata results, not traditional-list execution
or later-version scheduling acceptance. Inventory, GPU paths, UI and package are
unchanged. See the [baseline](initialization-graph-baseline.json).

The preceding [initial cache recovery](INITIAL_CACHE_AUDIT.md)
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

The preceding merged-list inventory contained **470 registered cases / 460 distinct hashes**:
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
| Pipeline, buffers and geometry | Captured versus native replay state, before/after boundaries, constants/counters, IA and supported post-shader output | Missing state retains provenance; SO cursors have no native getter; frame-before counters require evidence or explicit experiments. [Boundaries](BEFORE_BOUNDARY_MIGRATION.md), [geometry](POST_TRANSFORM_MIGRATION.md), [HS output](HULL_OUTPUT_MIGRATION.md), [initial counters](INITIAL_COUNTER_BOUNDARY_AUDIT.md), [created counters](BUFFER_VIEW_CREATION_AUDIT.md) |
| Experiments | Shader replacement, six-stage shader/IA/CB/SRV/sampler/output/SO/pipeline setter edits, buffer/texture edits and history | Event-scoped edits differ from persistent setters; consumer and format combinations retain their own limits. [Shader setters](SHADER_SETTER_MIGRATION.md), [event textures](EVENT_TEXTURE_MIGRATION.md), [reports](EXPERIMENT_REPORT_MIGRATION.md) |
| Coverage and Quad | Native execution, Qt controls, navigation, export and cancellation | Coverage is not an overdraw counter; Quad groups are not physical hardware quad counts. Sample/order/coordinate limits remain. [Coverage](COVERAGE_UI_MIGRATION.md), [Quad](QUAD_UI_MIGRATION.md) |
| Shader tools and debugging | DXBC/reflection, supported HLSL recovery, compilation/projects, source metadata and native GS/HS/DS checkpoint UI | Recovered HLSL is not original source; source availability and shader operations constrain debugging. [Recovery](HLSL_RECOVERY_MIGRATION.md), [projects](SHADER_PROJECT_MIGRATION.md), [checkpoints](CHECKPOINT_UI_MIGRATION.md) |
| RenderDoc analysis | Pixel History, VS/PS/CS recorded debugging, Replay Mesh and Replay Metrics | Requires compatible external RenderDoc 1.45 release DLL; unavailable values and backend limits remain explicit. [History](PIXEL_HISTORY_UI_MIGRATION.md), [debugging](REPLAY_DEBUG_UI_MIGRATION.md), [mesh](REPLAY_MESH_MIGRATION.md), [metrics](REPLAY_METRICS_MIGRATION.md) |
| GPU measurements | Native DX11 statistics/timing; Intel MD foundation, scheduled/uniform Qt collection and event-group CLI collection | Intel paths require supported hardware/driver. Event-group Qt/session integration, GTPin/Shader Profiler and further consumers remain incomplete. [Statistics](GPU_STATISTICS_MIGRATION.md), [timing](GPU_PROFILE_MIGRATION.md), [scheduled UI](MD_ITERATIONS_UI_MIGRATION.md), [uniform UI](UNIFORM_METRICS_UI_MIGRATION.md), [groups](MD_HOTSPOTS_MIGRATION.md) |
| Captured contexts and command lists | Identity/evidence inspection; verified expanded streams from two-context A/B/A and three-context merged-list workloads | Traditional list execution remains incomplete. Dynamic-CB merge captures can already contain wrong data; native/injected/original/independent results and restoration discrepancies remain separate. [Merge evidence](DEFERRED_MERGE_AUDIT.md), [deferred evidence](DEFERRED_VERSION_AUDIT.md), [context workflow](USAGE.md#contexts-and-pipeline-boundaries) |
| Resource minimum LOD | Native setters, initial getter evidence, frame-time creation defaults, ClearState preservation and disabled-setter experiments; bounded SM4.0 IF/SWITCH/LOOP proofs for unused queried dimensions | Full storage inspection/export and input clones preserve proved current MinLOD. Missing required initial state and wider UAV/output/interface dependencies remain boundaries. [Clone inheritance](LOD_CLONE_INHERITANCE.md). Unused static SRVs use program declarations. [Storage](LOD_STORAGE_READBACK.md). [LOD](RESOURCE_LOD_AUDIT.md), [shader usage](MIN_LOD_USAGE_AUDIT.md), [loop proof](SM40_LOOP_MIP.md) |
| Query and predication | Captured Query metadata/history; native predicate Begin/End, binding and Device5 frame-time creation | New predicates remain unissued until their recorded interval completes. Ordinary Query records may omit identities, intervals or full result bytes; pre-frame predicate history is not reconstructed. [Predicate creation](PREDICATE_CREATION_AUDIT.md) |

## Historical M2 registered-scope gate

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
| M3 — Deferred Context / Command List | Open; M4/M5 now take priority by user instruction on 2026-10-06. Require original retained-list captures proving identity, build order, resource versions, repeated execution, restore-state and event mapping before production support. |
| M4 — Resources and boundaries | Active, incomplete. Initial texture/Map data is checked; missing UAV counts reject on actual consumption, while proven counter-free indexed access is allowed. Verified mip-count-only reads, including proved unused SM4.0 dimension lanes across checked IF, SWITCH and LOOP blocks, do not consume missing MinLOD. Known nonzero-MinLOD storage copies and input-clone inheritance are accepted in their recorded scope. Per-slot counter dependencies and saved blt-model occluded Present TEST are checked; continue wider operand/control-flow dependencies, missing pitch/data, predicate/frame-before history, Query and other presentation boundaries. |
| M5 — Stable compatibility release | Incomplete. The 565-case registry, preceding committed-source build/package, load/semantic-audit cancellation, worker recovery, background common-report parsing, checked buffer acceptance and background replay/texture validation plus display preparation are established. Coverage/Quad attachments have checked background reading and diagnostic paint preparation. Thumbnail reports validate complete binding identity and the entire requested batch before publication, with 12 new isolated recovery cases. The preceding same-source 30-minute/248-cycle run passes 744 strict image checks; it does not certify the later GUI. Continue broader memory attribution, other specialized readers, analyzer/driver/storage failures, model/text publication and exports, large-file workflows, broader sustained testing and independent clean-machine deployment. [Thumbnail acceptance](THUMBNAIL_ACCEPTANCE.md), [latest package](BYTE_EXPORT_ACCEPTANCE.md), [source-build acceptance](CURRENT_SOURCE_RELEASE_ACCEPTANCE.md) |
| M6 — Analyzer and other versions | Incomplete. Continue remaining Qt consumers, advanced profiling/debugging and version-specific adapters after core replay gates. Existing analyzer features remain available. |

The original order was **M1 → M2 → M3 → M4 → M5**. On 2026-10-06 the user
authorized proceeding with **M4 → M5** for the supported scope while M3 retains
its original acceptance gate. M6 remains later work. Existing research/analyzer
components do not complete earlier compatibility gates.
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
