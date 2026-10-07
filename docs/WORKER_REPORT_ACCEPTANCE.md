# Background acceptance of common Worker reports

Reviewed 2026-10-08. Implementation: `5f1464e`; UI test synchronization:
`c4dd4d4`. This M5 change follows the checked image and buffer readers. The
common `report.json` was still read and parsed on the Qt event thread after the
Worker exited, and replay export parsed the same bytes again on that thread.

## Behavior

The common report reader now runs in the existing request-owned background task.
It reads in 1 MiB chunks, checks complete reads and unchanged length, requires an
object with a boolean `completed: true`, and preserves the original JSON's full
integer precision for replay exports. A prepared native JSON value is moved into
the accepted replay result, eliminating the later UI-thread parse and DOM copy.
Images and buffers still pass their existing integrity/request checks afterwards.

The report phase keeps the window busy and cancellable. Cancellation, selection
or capture revision changes, and window destruction prevent publication. The
job owns its temporary directory until its background reader finishes; it never
dereferences the window from that thread. Failed common reports use the same
per-view failure callbacks as failed Workers. The compact status is `Reading
report…`; no explanatory panel or new analyzer feature is added.

Qt JSON parsing itself is not interruptible within one call. Cancellation is
checked before and after that call, between read chunks, and periodically during
native JSON parsing. This improves event-loop availability without promising an
absolute cancellation, allocation, storage-I/O or publication latency bound.

## Verification

- 21 report-reader QtTest rows pass: complete metadata/replay/no-image reports,
  UTF-8 BOM and text, exact signed/unsigned 64-bit values, malformed UTF-8/JSON,
  scalar/array roots, completion-type errors, excessive nesting, all prefixes of
  a truncated fixture, changed file length, cancellation checkpoints and retry.
- All 43 isolated Worker scenarios pass, including nine new report cases. The
  five 64 MiB synthetic report workflows cover success, cancel, capture switch,
  close and destruction. They retain the previous image on failure/cancellation,
  verify the accepted replacement image, and retry with the real Worker. The
  parent CTest has 45 passes and two intentional child-entrypoint skips; the
  relocated parent-only selector has 45 passes and no skips.
- A continuous event-loop check during the 64 MiB report records 53 busy
  heartbeats, a maximum 15 ms interval and 565 ms completion in the focused run.
  The relocated Windows-platform run records 55 heartbeats, 13 ms and 563 ms.
  These are measured host observations, not universal limits or a before/after
  performance ratio. The padded report is a stress fixture, not a game capture.
- 24 distinct related CTest suites eventually pass. This includes 68 main-UI
  rows, resource/shader/geometry/analysis consumers, image/buffer validation and
  eight real GF2/BF1 recovery cycles with 24 strict image checks. Two optional
  Intel live-collection cases remain explicitly skipped; saved-result and other
  supported paths run. Per-suite rows and logs are pinned in the baseline.

Initial failures are retained. The first build could not generate the new test's
MOC metadata; ordinary escaped literals replaced the problematic raw literals.
The first broad CTest run passed 19 of 22 suites. One failed because the selected
counter fixture directory contained eight reports while the test requires nine;
the existing verified nine-report corpus passes on retry. Two import tests
triggered Open Experiment while an automatic preview was still busy. They now
wait for that existing prerequisite before clearing their completion spy.

A retry also exposed the old file-picker helper's `selectFile()` behavior with
an active filename editor. One identified test process was deliberately stopped
after waiting in that dialog; this is a failed run, not an acceptance result.
The helpers now enter the filename as the user would, select only visible
dialogs, and reject on a ten-second deadline. A reused Quad output directory was
also correctly refused; final retries use a fresh evidence directory. Final Quad
and GPU Profile suites pass with their full settings/export/cancel assertions.
No production import guard or validation threshold was relaxed.

## Package and replay scope

`out/FloraGPA-report-20261008/` contains 44 files. Only `FloraGPA.exe` differs from
`out/FloraGPA-sm40-mip-20261008/`; CLI, Worker and the other 41 companion files
are byte-identical. The preceding 35-suite / 517-registration replay matrix is
retained for those unchanged replay binaries, not claimed rerun in this batch.

Five relocated-package checks pass in a Chinese/space path with system-only
PATH: the report reader, all Worker fault scenarios, Windows report responsiveness,
and the actual shipping GUI opening GF2 and BF1 on the Windows platform. Both
GUI screenshots were reviewed. Four packaged golden/disabled-Draw runs retain
the preceding hashes and execution counts, with a loaded-module audit. A further
relocated recovery run passes four cycles and twelve strict image checks in
28.686 seconds. This short regression does not replace the previous sustained
soak or certify independent clean-machine deployment.

The [acceptance baseline](worker-report-acceptance-baseline.json) pins source,
package, test binaries, successful and failed logs, recovery records and reviewed
screenshots. Generated evidence and captures remain outside Git.

## Remaining boundaries

This accepts the common report envelope; it does not prove capture fidelity or
all payload fields. Specialized catalog/history/thumbnail reports and analyzer
payload readers retain their separate processing. Metadata/model/widget
publication, exports and large retained JSON values still require broader
latency and memory work. GUI publication and object destruction are not claimed
fully asynchronous. A fresh clean-source rebuild and sustained soak were not
repeated for this package. M3/M4/M5 and the 72/117/15 module ledger remain open.
