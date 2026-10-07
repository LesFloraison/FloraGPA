# Background acceptance of analyzer payloads

Reviewed 2026-10-08. This M5 iteration extends
[common report acceptance](WORKER_REPORT_ACCEPTANCE.md) to the larger JSON files
that were still read and parsed synchronously after `report.json` passed.
Implementation: `f70d65a`.

The subsequent [diagnostic attachment acceptance](DIAGNOSTIC_ATTACHMENT_ACCEPTANCE.md),
implementation `6b01ad8`, moves the remaining Coverage/Quad file reads, PNG
decoding and diagnostic image preparation into the same cancellable job.
The evidence and counts below describe the earlier JSON-only batch.

## Scope and contract

| Worker request | Prepared output | Consumer |
|---|---|---|
| `quad` | `quad.json` | Quad view |
| `coverage` | `coverage.json` | Coverage view |
| `timings` | `profile.json` | GPU timings |
| `statistics` | `statistics.json` | Pipeline statistics |
| `replay-pipeline` | `replay-pipeline.json` | Replayed pipeline state |
| `predicate` | `predicate.json` | Predication results |
| `geometry` | `geometry.json` | Input assembly geometry |
| `post-geometry` | `geometry-ui.json` | Shader output geometry |

The existing request-owned background job now reads the matching fixed filename
after validating the common envelope. Reads are chunked, complete length is
checked, and roots must be JSON objects. Native JSON preserves 64-bit integer
values and rejects nesting beyond 1024 parser levels. Cancellation is checked
between chunks, during native parser events, and after parsing. Qt geometry
parsing remains one noninterruptible call on the background thread.

Only prepared objects reach the existing consumers. Their request matching and
semantic checks remain in place. Errors identify the payload filename. The same
revision/cancellation guard prevents stale publication; the job owns temporary
files until reading completes, including after window destruction. An array-root
geometry payload previously became an empty object through Qt conversion; it is
now rejected before replacing the displayed geometry.

At this batch's revision, this was first-stage JSON preparation, not asynchronous
acceptance of every consumer artifact. `CoverageView::finish` reread `coverage.json` and three
attachments, compared the report and decoded the overlay on the UI thread.
`QuadView::finish` likewise reread `quad.json`, loaded six attachments and decoded
its preview synchronously. Both used the synchronous image setter. The geometry
heartbeat measurements below do not cover those paths; their later migration
has separate evidence linked above.

## Validation

The reader suite adds all eight output paths. Each checks missing files, every
truncated prefix, invalid roots/UTF-8/trailing bytes, excessive nesting, valid
recovery, and cancellation at every observed checkpoint of a larger payload.
The common report precision and integrity checks remain covered.

Nine isolated geometry workflows exercise missing/syntax/root failures, valid
replacement and 64 MiB success/cancel/switch/close/destroy paths. Each begins
with real Worker geometry. Failed or cancelled jobs preserve the prior geometry;
success must actually publish a distinguishable table. Retry restores the real
Worker's six rows. Switching captures and destroying a window check obsolete
publication and temporary-directory cleanup respectively. The large fixture is
padded test data, not a real 64 MiB game geometry workload.

Exact test, package and recovery results are pinned in the
[acceptance baseline](analyzer-payload-acceptance-baseline.json). Generated
captures, raw outputs, screenshots and logs stay outside Git.

The 29 reader rows and all 52 isolated Worker scenarios pass. The parent CTest
reports 54 passes plus three intentional child-entrypoint skips; the relocated
parent-only selector reports 54 passes without skips. Seven consumer CTest suites
also pass: Quad, coverage, shader output geometry, statistics, timings, main UI
and recovery. Main UI has 68 passing rows; recovery includes eight GF2/BF1 cycles
with 24 strict image checks. GPU tests ran serially. The focused two-suite run
took 75.05 seconds and the consumer seven-suite run took 273.18 seconds.

During successful 64 MiB geometry acceptance, the build-tree run recorded 21
busy heartbeats, an 11 ms maximum interval and 210 ms completion. The relocated
Windows-platform run recorded 19 heartbeats, 12 ms and 215 ms respectively. These
observations cover reading/parsing and publication of the small sentinel table;
they do not measure publishing millions of table rows.

The 44-file package `out/FloraGPA-payload-20261008/` differs from the preceding
report package only in `FloraGPA.exe`. Five system-PATH checks in a Chinese/space
directory pass, including the shipping GUI opening GF2 and BF1 on Windows; both
screenshots were reviewed. The unchanged CLI and Worker retain the preceding
517-registration replay matrix, without claiming it rerun for this UI batch.
Four packaged golden/disabled-Draw checks and their module audits are preserved
in the baseline; hashes and execution counts match the preceding package. A
separate relocated recovery regression passes four cycles and twelve strict
images in 28.940 seconds. It is not a sustained soak. The baseline verifies
283 local evidence files and the package/runtime manifests.

## Remaining work

This changes GUI result acceptance, not DX11 execution or capture fidelity.
Consumers still publish models/widgets and may retain or copy large objects on
the UI thread. Coverage/Quad report revalidation, binary/image attachment loading,
PNG decoding and display preparation remain synchronous, as described above.
Thumbnail/catalog/history, shader/checkpoint payloads, metric
collection acceptance and exports have separate readers. This batch does not
claim all analysis workflows asynchronous or memory-bounded. Cancellation cannot
interrupt a single Qt parse, allocation, file-system operation or native JSON
string token; observed heartbeat intervals are not absolute latency guarantees.

The existing replay matrix can be retained only after checking replay binaries
are byte-identical. A same-host relocated package is not independent clean-host
deployment. M3/M4/M5 remain incomplete, and module counts remain 72/117/15.
