# Private primitive PS debug jobs and sessions

Implementation: `417f747263aa6f17cfca13db753505f6f6dff031` on
`feature/dx11-native-backend`.

This increment exposes the independent C++ pixel-quad executor through native
analysis jobs, verified sessions and the Native Execution view. Its explicit
`private-primitive` scope preserves unknown original fragment eligibility and
instance identity. It is not full PS debugging or a dependency-removal gate.
See [the usage guide](../NATIVE_PRIVATE_PIXEL_DEBUG.md).

## Execution and evidence boundaries

Private draw history supplies the selected geometry ordinal's measured quad and
current resources at its before-primitive checkpoint. A new observer runs while
that snapshot is valid. The debug action clones mutable buffer/texture storage
before executing the reversible journal; helper lanes retain the existing
cooperative semantics. Subsequent history observations use the untouched source
snapshot. The selected sample must belong to exactly one observed invocation.

The session verifies explicit scope, original unknowns, event/shader/ordinal,
resource/subresource/sample, measured input provenance, lane/coverage and actual
adapter against the private history and trace header. It also requires observed
storage restoration and the existing context/artifact integrity checks. It does
not infer original fragment identity from a private replay ordinal.

The common debugger displays raw bits, validity, source information when present,
breakpoints and watches and exports the session. Its summary identifies private
primitive replay. Changing selection invalidates the session. Unscoped pixel
debug requests continue to fail at the migration gate.

## Retained development failures

The initial new suite passed all 15 GPU cases before its failure-case fixture
reused an output directory: two serialized patches happened to have the same
string length. The fixture now assigns unique monotonic directory names. No
behavior assertion was removed. The final matrix also adds UAV atomic writes.

The first UI regression run passed the new PS path but failed all six existing
VS/CS main-window jobs. The preceding draw-history integration assigned
`prepareBeforeDraw` only for private history, overriding its default for other
before-event analyses. The unified dispatcher now prepares state for every
before-event action. The unchanged VS/CS main-window assertions pass after the
fix; failed logs remain in the evidence ledger.

## Synthetic validation

The 18 owned GPU jobs cover WARP, NVIDIA 4318 and Intel 32902, each with constant
outputs, derivatives, MSAA sample 2, discard, embedded source and UAV atomic
writes. The fixture uses three overlapping primitives and selects ordinal 1.
Expected output raw bits are authored independently of serialization. Atomic
storage begins at 1, reaches 2 and rewinds to 1; later private observations retain
their expected memory-change count. Six resealed metadata mutations per job are
rejected by the C++ session loader (108 controls).

Jobs also exercise missing primitive selection, trace-byte and step limits,
actual CLI parsing, terminal quad state, register rewind, configuration,
breakpoints, watches, source-file availability and ZIP export. Shared analysis
tests retain timeout, cancellation and crash-recovery checks. This does not claim
instruction-level cancellation at a particular GPU operation.

The final regression set passes 214 Qt checks without failures or skips: pixel
jobs 22, sessions 50, pixel traces 24, analysis 46, draw-history jobs 20, execution
UI 17 and native mesh UI 35. The execution UI includes direct PS Worker delivery,
stepping and stale-selection rejection plus nine main-window VS/CS/private-PS
adapter combinations, each checking cancellation and successful retry. The tab
is named `Native execution` to include its new explicitly scoped PS selection.
The earlier corrected 176-check run is also retained; only the changed UI and
mesh consumers were rerun after the final tab/test update.

The independent Python validator verifies saved manifests, request/context and
adapter joins, complete private history/shader joins, and the selected measured
invocation. It independently applies and reverses every journal delta and
compares initial, final and each selected-boundary state with the C++ reader.
These are serialization/continuity checks; they do not independently establish
every shader opcode's hardware semantics. Seven metadata/module mutations per
job exercise invalid context, eligibility, ordinal, sample, lane, boundary and
loaded-module provenance.

All 18 owned and nine real jobs pass this validator, rejecting 189 altered
results. All 27 C++ reader and session probes pass. The independent ZIP validator
also verifies CRCs, exact members, original artifact equality, extracted-session
reload and exact re-exported state for all 27 archives. Source-enabled owned
shaders map four selected steps and expose 36 source-value records per adapter;
unavailable real-capture source remains unavailable.

## Real captures

All nine CLI jobs use explicit adapters and D3D11 validation. Selections are GF2
event 113/resource 80/pixel (799,480)/ordinal 0; BF1 event 2010/resource 954/pixel
(751,433)/ordinal 2044; Helldivers event 200/resource 99/pixel (168,183)/ordinal 0.
Sample, mip, layer and slice are zero. Frame hashes are retained in each result.

| Case | Worker ms | Peak working set bytes | CLI wall ms | Global / selected steps | Trace bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| GF2 / WARP | 543 | 317,071,360 | 1,692 | 100 / 25 | 252,037 |
| GF2 / NVIDIA | 1,041 | 274,145,280 | 2,117 | 100 / 25 | 252,051 |
| GF2 / Intel | 881 | 388,382,720 | 1,965 | 100 / 25 | 252,056 |
| BF1 / WARP | 5,453 | 2,664,566,784 | 15,472 | 408 / 102 | 7,457,037 |
| BF1 / NVIDIA | 10,116 | 1,792,802,816 | 20,064 | 408 / 102 | 7,457,041 |
| BF1 / Intel | 7,331 | 2,293,243,904 | 17,119 | 408 / 102 | 7,457,043 |
| Helldivers / WARP | 5,873 | 2,378,371,072 | 17,034 | 1,316 / 329 | 260,297,249 |
| Helldivers / NVIDIA | 13,878 | 2,092,199,936 | 25,693 | 1,316 / 329 | 260,297,180 |
| Helldivers / Intel | 6,123 | 2,214,748,160 | 17,296 | 1,316 / 329 | 260,297,268 |

The machine uses NVIDIA driver 32.0.16.1742 and Intel driver 32.0.101.8826.
Adapter LUIDs are recorded per job, not assumed stable across system sessions.
Worker timing excludes parent hashing/serialization overhead. Payload bounds
are not peak process-memory limits. The large Helldivers journals make repeated
whole-resource digest auditing expensive; audit-probe timing must be distinguished
from Worker execution and interactive navigation. These runs are not a controlled
baseline comparison, memory-growth study or P8 performance acceptance.

Completion-time module snapshots contain neither RenderDoc nor private GPA
modules. Transient loads, imports, subprocesses, build graphs and clean package
audits remain separate P8 requirements.

The Helldivers C++ reader audits take 239,450 ms (WARP), 237,853 ms (Intel) and
234,689 ms (NVIDIA), dominated by whole-resource hashes at every selected
boundary. Corresponding session load times are 7,740, 7,737 and 8,113 ms; complete
load/step/rewind/export probes take 8,438, 8,491 and 8,900 ms. No smaller trace,
weaker boundary assertion or skipped real capture was substituted for this
expensive audit.

## Reproduction

Build `FloraNativePixelDebugTests`, `FloraNativeDebugSessionTests`,
`FloraDxbcPixelTraceTests`, `FloraNativeAnalysisTests`,
`FloraNativeDrawHistoryJobsTests`, `FloraNativeExecutionDebugUiTests`,
`FloraDxbcTraceReaderProbe`, `FloraNativeDebugSessionProbe`, and the application
in Release. Put Qt on PATH and run GPU suites serially. Set
`FLORA_PIXEL_DEBUG_EVIDENCE` to a fresh directory before the pixel job suite.

For each owned/real job, use the manifest's trace SHA-256:

```text
FloraDxbcTraceReaderProbe JOB/trace.jsonl TRACE_SHA256 READERS/CASE.reader.json --shader JOB/shader.dxbc
FloraNativeDebugSessionProbe JOB READERS/CASE.session.json READERS/CASE.session.zip
python tools/validate_native_pixel_debug.py OWNED --real REAL --readers READERS --out NEW_REPORT.json
python tools/validate_native_debug_session.py READERS/CASE.session.zip --original JOB --probe FloraNativeDebugSessionProbe --out NEW_ARCHIVE_REPORT.json
```

Owned directory names are `VENDOR-MODE`; real names are `gf2-VENDOR`,
`bf1-VENDOR`, and `helldivers-VENDOR`. Vendor 0 selects WARP. Raw binaries,
captures, traces and archives stay outside Git. The companion evidence ledger
pins reproducible sources and generated evidence. P1 engine recovery, complete
fragment qualification, full debugger parity and P7/P8 remain open.
