# Checked background buffer acceptance

Reviewed 2026-10-08. This M5 change checks the existing native buffer inspection
result before publishing bytes to the Qt table. It changes neither DX11 execution
nor the CLI artifact format. Implementation: `4fe2ee7`.

## Problem and contract

The Worker already saves `buffer.bin` with a SHA-256, resource identity, byte
range and event boundary. The GUI previously read the entire file on its event
thread, accepted incomplete or altered bytes without comparing that hash/length,
and trusted the report's identity and range for the table label.

The GUI now snapshots the requested resource, offset, resolved length and event
boundary. Range validation uses the captured buffer's UINT ByteWidth, including
an explicit zero-length read at the end. Background acceptance requires the
report to match that request exactly. IDs remain decimal strings, preserving
64-bit identities without floating-point conversion. Numeric ranges must be
integral and nonnegative; missing or wrongly typed fields reject.

The reader checks the file size before reading, reads/hashes at most 1 MiB per
chunk, checks for incomplete reads and size changes, and compares the SHA-256
before returning bytes. The model receives the same bytes that were hashed.
Empty results require a present empty file and its correct hash. There is no
truncation, padding, hash bypass or conversion of buffer contents.

Buffer and image validation share the existing owned background-job lifetime,
busy-state guards and cancellation/revision checks. The job owns its temporary
directory and captures no widget pointers. Cancelling, changing captures or
destroying the window cannot publish an obsolete result; the directory remains
alive until the job exits. Pending/failed buffer reads retain the existing empty
table policy. The compact status is `Validating buffer…`; no layout change is
introduced.

## Focused verification

The 29-row reader suite covers five valid layouts (unaligned range, before/after
event, empty end range, multiple chunks), 21 rejection cases and cancellation at
all five positions in a multi-chunk read. Every rejection/cancellation is followed
by a valid retry. Controls include missing/truncated/extended/changed files,
invalid hashes, wrongly typed/ranged metadata, event/identity mismatches and
resizing a file during reading.

Eleven new isolated Worker scenarios exercise the real MainWindow, a temporary
fault Worker and retry with the production Worker. Six cover valid and malformed
results; five use a 32 MiB buffer for cancellation, capture switching, closing,
destruction and full successful acceptance. A queued GUI heartbeat executes
while the validation request is still active. Successful byte ranges and
offsets are compared exactly, and cancelled/failed bytes never enter the table.
Destroying the window checks eventual directory cleanup. These controls use
synthetic captures and injected artifacts; they do not replace original-capture
replay acceptance.

Eight related CTest suites pass in one invocation: 67 main Qt rows, ten resource
rows, eleven frame-output rows, 29 buffer-reader rows, eleven display rows,
28 image-reader rows, eight recovery rows and 34 isolated Worker scenarios. The
Worker parent reports 36 passes and two intentional child-only-entry-point skips;
every launched child passes. The original GF2 full-buffer hash and unaligned
17-byte subrange regression pass; its buffer screenshot was inspected.

The package is `out/FloraGPA-buffer-20261008/` (44 files). Only `FloraGPA.exe`
changes from `out/FloraGPA-image-display-20261008/`; the other 43 files, including
CLI and Worker, are byte-identical. The preceding 511-registration replay matrix
is retained for these unchanged replay binaries, not newly rerun here.

Seven relocated-package checks pass in a Unicode/space directory with system-only
PATH: native Windows/offscreen display, image and buffer readers, all 34 isolated
Worker scenarios, and actual shipping-GUI GF2/BF1 startup and screenshots. The
parent-only recovery selector has 36 passes without skips. The two shipping-GUI
screenshots were inspected. Four packaged golden/disabled-Draw runs preserve all
four preceding raw-image hashes and execution counts, with loaded-module audits.
Another relocated production-Worker run passes four GF2/BF1 recovery cycles and
twelve strict image checks in 28.517 seconds. This short regression does not
replace the preceding sustained soak or establish clean-machine acceptance.

The [baseline](worker-buffer-integrity-baseline.json) pins implementation, package,
test logs, harnesses, golden reports, recovery journal and reviewed screenshots.
Generated artifacts and captures remain outside Git.

## Boundaries

This is artifact consistency and request matching, not proof of capture fidelity
or correct GPU execution. The production Worker and CLI are unchanged. Buffer
inspection's GPU readback and CSV generation still occur in the isolated Worker;
its execution is cancelled by the existing process cancellation path.

The validated byte array remains resident for inspection and export. Large
allocations/copies and OS reads have no absolute interruption-time bound. Report
JSON parsing, constant-binding/counter metadata, other analyzer payloads, model
publication and exports retain their existing behavior and are not certified by
this reader. General large-file and independent clean-machine acceptance remain
open. M3/M4/M5 and the module migration counts remain incomplete.
