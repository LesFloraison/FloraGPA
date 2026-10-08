# Checked background output-storage export

This M5 correction builds on [export asset ownership](EXPORT_ASSET_OWNERSHIP.md).
It applies to **Export Output Storage**. Texture/DDS, buffer, PNG and geometry
exports retain their previous ownership fix but are not converted to background
operations in this batch.
Implementation: `49d9084` (`Validate and stage output storage exports in background jobs`).

## Contract

Previously, the GUI copied `output_storage.bin` with `readAll()` and only then
opened its metadata. It did not verify the storage byte count or hash already
present in the accepted replay report. Missing metadata could leave a replaced
binary, and short/corrupted storage could be reported as successfully exported.

The exporter retains the selected directory and native `output_display` object
before opening the chooser. A background job then:

1. Checks the count and SHA-256 fields, and rejects destinations overlapping
   either source artifact.
2. Checks the sidecar against the exact serialization emitted by the current
   Worker from that same native display object. This is a private artifact
   contract, not a general-purpose JSON importer.
3. Opens both destination staging files without direct-write fallback.
4. Copies storage in 1 MiB chunks, checking length, read/write results, final
   size and SHA-256 before publication.
5. Checks cancellation, then publishes both staged files.

Validation/read/staging failures and cancellation before publication leave
existing destination files unchanged. This includes a missing or inconsistent
sidecar and a file modified while being read. Successful exports preserve the
original raw bytes and sidecar, without reconstructing or padding storage.

The two final file replacements are **not an atomic pair**. Cancellation stops
being observed once publication begins. A failed binary replacement leaves the
old pair intact; if the binary succeeds and the metadata replacement fails, the
error explicitly reports that partial publication. Retrying can complete the
pair after the filesystem obstruction is removed. A process crash or storage
failure between renames is not a rollback guarantee.

## Qt lifecycle and evidence

The background operation owns its directory, report and cancellation token; it
never accesses widgets. Cancel, opening another capture, close and destruction
cancel pending work. An independent `exportFinished` signal prevents export
completions from masquerading as replay completions. Existing accepted pixels
remain available; failure does not poison a subsequent real-worker replay/export.

The focused native suite passes 29 Qt rows. It covers empty/small/multi-chunk
files, missing/short/long/corrupt data, metadata identity and length errors,
invalid count/hash types, source aliases, destination staging failure, truncation,
growth and changed bytes during reads. Seven cancellation checkpoints preserve
both prior outputs. Windows handles that deny delete-sharing independently test
binary and metadata publication failure, their actual resulting files, and retry.

Eleven isolated GUI scenarios cover success, five malformed output controls,
cancel, capture switch, close, destruction and background success. A test-only
64 MiB Worker artifact exercises responsiveness and lifetime; it is not evidence
that a new captured texture layout is supported. Its first successful run records
47 UI heartbeat ticks. Every surviving window retries through the real worker;
the destruction case waits for temporary source-directory cleanup.

The original GF2 export/capture-switch test compares all exported bytes and the
metadata sidecar with its baseline. It now waits for the independent completion
signal. The other four export-ownership cases and the existing texture-refresh
control remain unchanged.

The final five CTest suites pass serially in 495.32 seconds: 88 UI, 12 draw-resource,
29 output-export, 113 Worker recovery (111 isolated scenarios plus setup/cleanup),
and 8 persistent recovery rows. All 250 top-level Qt rows pass without skips.
Nested child setup/cleanup counts are not added again. Final logs use
`artifacts/m5-storage-export-final-*.txt`.

The 44-file `out/FloraGPA-storage-export-20261008/` package passes six serial
relocated/system-PATH checks: Windows export/selection/project controls (10 Qt
rows), the native export suite (29), all eleven export recovery scenarios (13
top-level rows), ordinary GF2/BF1 recovery (3), and shipping-GUI GF2/BF1 launches.
Four golden/control CLI replays pass strict image, execution-count and loaded-module
checks. Only `FloraGPA.exe` changed from the preceding package; all 43 other files
are identical. The 557-registration / 547-unique-capture replay matrix, with 521
completions and 36 located refusals, is inherited rather than rerun or reclassified.

The [baseline](storage-export-baseline.json) pins eleven source/build/test files,
test executables, all package files and 188 local evidence files. Original
captures, binaries and artifacts are outside Git. This is same-host relocation,
not independent clean-host deployment or renewed long-soak acceptance.

## Remaining scope

Other export paths, whole-report semantic certification, general analyzer export
validation, transaction recovery, broader large-file stress and independent
clean-Windows deployment remain open. No replay implementation, capture format,
game shader or compatibility policy is changed. M3/M4/M5 and the module ledger
remain incomplete.
