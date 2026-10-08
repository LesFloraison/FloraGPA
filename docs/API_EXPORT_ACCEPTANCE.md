# Streamed background API-log exports

Reviewed 2026-10-08. API-log export previously read the live capture/filter after
its nested directory chooser and generated the complete command array, selected
array and JSON/CSV text synchronously. It now retains the requested capture and
filter before either chooser or overwrite confirmation, and runs generation in
the existing cancellable background export job. The layout is unchanged.
Implementation: `ae36217`.

## Output and cancellation contract

`commands.json` and `commands.csv` preserve the preceding format and bytes,
including indentation, UTF-8 escaping, CSV BOM/CRLF and filtering. Query history
is interpreted incrementally in capture order **before** filtering. A filtered
GetData never borrows future GetDesc metadata. Existing malformed/unknown query
and initializer diagnostics remain visible; decoding remains distinct from GPU
execution support.

The exporter computes the initial cache once, decodes one command at a time and
serializes it through separate buffers of at most 1 MiB. The adapter uses the
bundled nlohmann serializer so escaping/formatting follows `dump(2)`; upgrading
that dependency must retain the byte-equivalence tests. The initial index,
Query history, individual command, CSV row and initialization-cache report still
allocate memory. This bounds accumulated serialization text, not total process
memory or the size of a single decoded command.

Cancellation is checked during cache enumeration, between records, at writer
flushes and before publication. It cannot preempt a single decoder operation.
Each output uses QSaveFile staging with no direct-write fallback. Failure or
cancellation before publication leaves both prior files intact. Publication
commits JSON then CSV without a cancellation point between them. They are two
independently named files, not an atomic group: if CSV publication fails after
JSON succeeds, the error explicitly reports the partial publication. Retry
replaces both. No crash rollback or power-loss durability is claimed.

## Verification

Seven serial CTest suites pass 271 top-level Qt rows without failures/skips:
API export 21, API export UI 13, full UI 89, isolated Worker recovery 121,
persistent recovery 8, API commands 8 and initialization graph 11. The last
two suites were repeated with explicit filenames because their CTest stdout
contained no Qt rows; the repeats also pass. They are not counted twice.

The new writer tests check nine format/filter profiles, capture-order Query
interpretation, malformed getters/UAV records, wide IDs, five cancellation
boundaries, invalid staging, Windows publication locks and retries. GUI tests
change the capture or filters inside both the directory chooser and overwrite
confirmation, with a required nonempty GF2 Map/resource-6 result. BF1 lifecycle
checks cover success, cancel, capture switch, close, destruction, JSON/CSV
publication locks and retry, with event-loop heartbeats and output hashes.

Earlier test-driver failures remain in local evidence: Windows staging-file
length observation could not locate cancellation reliably; QMessageBox::done
was not equivalent to clicking its standard Yes button; the initial GF2 Map
filter used resource 104 and selected no rows. The driver now injects at
measured cancellation checkpoints, clicks the button, uses captured resource 6
and requires a nonempty selection. The obsolete active GUI run was terminated
after diagnosing the driver. Its failures are not counted as passing results.

Nine serial old/new CLI comparisons cover API/query fixtures, 5,008-command
output, Query/empty filters, complete GF2/BF1 and nonempty GF2 Map/resource-6
filtering. JSON and CSV are byte-identical in all nine pairs. The final same-host
BF1 run takes 1.620 s / 1.086 s with peak working set 702,251,008 / 41,914,368
bytes (about 670 / 40 MiB). These are observed single-run measurements, not a
general latency or constant-memory guarantee. Initial measurements performed
while a GUI test run was active are preserved separately.

The 44-file `out/FloraGPA-api-export-20261008/` package passes six relocated
checks under a directory containing spaces/Chinese characters and a
Windows-system-only PATH: the complete API lifecycle suite (13 rows on Windows),
writer tests (21), existing chooser ownership/texture refresh (8 on Windows),
Qt recovery (3), and shipping GF2/BF1 GUI startup. Four isolated-environment
GF2/BF1 golden/control replays pass strict image hashes, execution counts and
suppressed-Draw controls. GUI, CLI and Worker differ from the predecessor;
41 other files are identical. This remains same-host relocation, not an
independent clean-Windows deployment.

All 40 compatibility-matrix suites were rerun because the command/initial-cache
code is shared by CLI, Worker and GUI. The gate contains 565 registrations /
555 unique captures: 526 completed registrations / 516 unique completions and
39 located refusals. It performs 1,130 ordinary attempts, 732 control runs and
1,032 resource exports. All 565 complete preflight reports, execution counts
and deterministic output images match the preceding CSV-stream matrix. Only
registered Helldivers image variation remains.

Completion is separate from application fidelity: the unchanged capture
assessments are 94 passed, 32 capture-side mismatches, 16 missing-information
cases and 423 unassessed registrations. Of completed registrations, 35 retain
known capture limitations. This rerun does not add original-player comparisons,
new accepted DX11 semantics or a claim of universal compatibility.

The [pinned baseline](api-export-baseline.json) records source, test, executable,
package and 955 local evidence-file identities. Captures and generated outputs remain
outside Git.

## Remaining scope

These changes improve export ownership, memory use and responsiveness. The
first GetData lookup in CaptureModel still calls full inspectCommands on the
event thread, and capture-structure JSON export still serializes/writes
synchronously; these are identified follow-up paths, not fixed by this batch.
This batch adds no DX11 record semantics or capture-format coverage. GPU equivalence still
requires its own image/resource/state evidence. Other analyzer exports, GUI
model/text publication, broader resource dependencies, independent clean-host
deployment and renewed long-duration acceptance remain open. M3/M4/M5 and the
72 ported / 117 partial / 15 pending module ledger are unchanged.
