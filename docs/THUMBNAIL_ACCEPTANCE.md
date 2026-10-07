# Background thumbnail acceptance

Reviewed 2026-10-08. This M5 change moves the existing resource-thumbnail report,
PNG validation and paint preparation into the request-owned background job.
It adds no capture format, replay command or analysis feature.
Implementation: `41d5a18`.

## Reproduced failure and correction

The old consumer read the report and PNGs on the UI thread and published each
icon immediately. A preserved CPU reproduction requests two thumbnails: the
first PNG is valid, the second is missing. The old consumer throws after already
changing the first icon. That partial result could also enter its cache.

The new reader prepares the whole output before UI publication. The consumer
then validates every requested binding and allocates its paint objects before
changing any item. Missing requested rows, inconsistent identity or invalid
images reject the batch. Explicit per-binding `preview_error` remains a valid
Worker result and displays the existing warning icon. Unrequested bindings may
legitimately have neither a preview nor an error.

Identity includes the event, resource, view, binding slot/stage/role, format,
dimensions, mip/layer/slice ranges, sample selection and boundary. The report
keeps native 64-bit integers. Matching resource IDs alone cannot authorize a
thumbnail for a different subresource. Duplicate binding keys and conflicting
preview/error fields reject. PNG names must be local basenames; headers and
decoded dimensions must agree and fit the existing 96 by 96 limit. Shared PNGs
are decoded once per batch. Prepared premultiplied pixels retain alpha.

The existing request revision, cancellation flag and owned temporary directory
remain in force. Cancellation is checked during chunked reads and parser events,
between bindings and around image operations. Individual Qt decode/conversion
calls are not interrupted internally. A cancelled or stale job cannot replace
the new selection; destruction retains the directory until the reader releases
it. Automatic thumbnail completion does not overwrite the main replay report
or emit a frame replay completion signal. Successful retry restores the normal
binding tooltip.

## Validation design

CPU tests cover native integer identity, shared previews, explicit errors,
unrequested rows, malformed envelopes, every truncated prefix of a report,
invalid numeric fields, unsafe names, missing/corrupt/truncated PNGs, 96/97-pixel
boundaries, exact prepared pixels and cancellation at each observed checkpoint.
The old partial-publication reproduction is now a regression test.

Twelve new isolated Worker scenarios exercise valid replacement, missing PNG,
wrong resource identity, wrong mip identity, omitted/duplicate binding, oversized
PNG, and background success/cancel/capture-switch/close/destruction. Each starts
with the real Worker; fault injection changes only a child-owned copy of its
output. Failure checks unchanged icons and main-frame pixels; retry checks both
icons against real-Worker output. Identity controls corrupt the last key in
consumer order so a one-pass publisher would expose an earlier row.

The background controls use a synthetic 64 MiB JSON padding field. This tests
report acceptance and event-loop progress, not a claim that the tiny DX11 source
capture produces a report of that size. BF1's real resource-navigation test also
checks multiple render targets, inputs, depth, history and coverage navigation.

Initial test-development failures are retained: an arbitrary minimum parser
checkpoint count was replaced by checking every observed checkpoint, and QColor
object equality was replaced by exact packed RGBA8 equality. Qt can retain
higher internal precision after unpremultiplication even when the reported
RGBA8 pixels are identical. No image tolerance or runtime rejection was relaxed.

## Scope and remaining work

This correction does not change the native CLI/Worker replay semantics or the
registered compatibility scope. The existing 33 registered refusals still
cover initial MinLOD (11), unproved context identity (7), missing initial UAV
counters (4), missing discard targets (9), and lost rectangle-pointer presence
(2). These are suite/case registrations, not a count of all outstanding M4
problems. Their queue review does not prove that other M4 fixes are impossible.

UI pixmap/model publication, text layout and exports still execute on the event
thread. Other specialized readers, broader memory attribution and independent
clean-machine deployment remain open. The preceding package's 30-minute soak
does not certify this GUI. M3/M4/M5 remain incomplete; module counts remain
72 ported / 117 partial / 15 pending.

## Integration results

Seven related CTest suites pass with 222 top-level Qt rows, no failures and no
skips: draw resources 11, main UI 69, recovery UI 8, Worker recovery 89,
thumbnail output 6, diagnostic output 10 and common/analyzer reports 29.
Worker recovery includes all 87 isolated scenarios, including the 12 new cases.
The build-tree recovery suite checks four GF2/BF1 cycles and twelve strict images.
The BF1 resource and coverage screenshots were visually reviewed.

The 44-file package `out/FloraGPA-thumbnail-20261008/` differs from the preceding
diagnostic package only in `FloraGPA.exe`. Four packaged golden/disabled-Draw
checks preserve exact pixels and execution counts. The byte-identical CLI and
Worker inherit the existing 525 registrations: 492 completed and 33 located
refusals, covering 515 unique captures. This batch does not claim a full-matrix
rerun or new original-player evidence.

Seven relocated checks pass from a Chinese/space directory with system-only
PATH: native Windows background success, wrong-resource and wrong-mip refusal,
window destruction, four GF2/BF1 recovery cycles with twelve strict image checks,
and both shipping-GUI startup/replay screenshots. The screenshots were reviewed.
This is same-host relocation, not independent clean-machine deployment.

| Synthetic 64 MiB report acceptance | Busy timer ticks | Maximum interval | Observed completion |
|---|---:|---:|---:|
| Build, offscreen | 46 | 11 ms | 425 ms |
| Relocated, Windows | 39 | 16 ms | 415 ms |

These observations show event-loop progress for the tested workload, not a global
latency bound. The [acceptance baseline](thumbnail-baseline.json) pins the source,
package, captures, queue review and 256 local evidence files. Captures, generated
outputs and test fixtures remain outside Git. Existing offscreen font/plugin
warnings remain visible in the logs.
