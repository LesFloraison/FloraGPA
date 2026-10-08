# Background image and texture exports

Reviewed 2026-10-08. Display-image encoding and texture-cache export previously
ran on the Qt event thread. Texture export also read its complete source file
into a QByteArray. Both operations now use the existing background export job,
its cancellation token and independent exportFinished completion signal.
Implementation: `449e3d3`; deterministic overlap fixture: `80f48f1`.

## Ownership and publication

The displayed QImage snapshot is obtained on the GUI thread before the chooser;
encoding and writes run off-thread. A texture export retains the accepted cache
directory through the chooser and background copy, and resolves the source
filename from the metadata captured before the chooser. Neither job
reads the window or its current selection after launch. Changing capture,
closing or destroying the window cancels pending work.

Images retain suffix-based format selection. A QIODevice adapter forwards codec
writes to QSaveFile in bounded chunks and checks cancellation between writes.
It reports cancellation or a callback exception as a device error first, then
raises the exception after the codec returns; it does not unwind C codec
callbacks. Codec work between writes is cooperative, not forcibly preemptible.
The adapter records failed/short writes even if a codec were to ignore them.

Texture copies use a 1 MiB buffer, verify every read/write and recheck source
length before publication. Both paths keep QSaveFile direct-write fallback
disabled and check cancellation before commit. Failure or cancellation before
publication preserves an existing destination. Cancellation after commit starts
does not roll back a completed replacement. These are single-file exports.

## Verification scope

Writer tests compare exact bytes with the preceding QImage::save route for
PNG (including uppercase suffix), BMP, JPEG, ICO/CUR, PBM/PGM/PPM and XBM/XPM.
They check codec cancellation, callback exceptions, invalid images/formats,
empty and multi-chunk copies, cancellation checkpoints, missing/truncated/grown
sources, Windows destination locks and retry.

GUI cases use a 2048-square display image and a 64 MiB captured texture. Success,
cancel, capture switch, close, destruction, locked destination and same-action
retry verify pixels or every copied byte. The strengthened destruction test
waits for background tasks to finish before checking for late publication.
Success observes Qt timer activity while the export remains busy. These are
export/lifetime fixtures, not additional capture-format coverage.

Existing GF2 chooser-ownership checks and texture refresh, DDS, PNG, raw, luma
and mip-zero export controls await independent export completion. The first
full UI run found a real regression: a preview accepted during an export could
leave the texture export action disabled indefinitely. The old completion
handler restored only the asset/revision from the beginning of its job.
Availability now follows the current accepted texture and busy state. A new
case holds the registered export at its progress signal while a real texture
preview is accepted, then releases export launch and checks the old exported
image, new texture and restored action. The initial failing log is retained.

One subsequent CTest invocation omitted the specialized capture environment:
its UI result was 64 passed / 25 skipped, despite CTest reporting success.
That result is retained but does not count as full UI acceptance; the final UI
run loads the original capture environment before checking every row.
The first full-environment rerun then exposed a test timing assumption:
snapshot event processing started a queued preview, so the immediately
requested editor correctly refused a busy window. The test now awaits the
accepted preview before checking inherited editor options; the production busy
guard is unchanged. This separate failing log is also retained.
The first relocated Windows run found that the initial overlap fixture's
large-image encoding did not reliably establish the required ordering. The
final test uses the explicit ordering above instead of relying on codec/driver
speed; it also retains an observed overlap rather than overwriting it on a
later preview completion. Actual encoding responsiveness remains covered by
the separate large-image lifecycle cases. The failed portable run is retained.

The six relevant serial CTest suites finish with 272 top-level Qt rows and no
failures/skips: image writers 31, image/texture lifecycle 15, byte-export
lifecycle 16, complete UI 89, isolated Worker recovery 113 and persistent Qt
recovery 8. The complete UI total comes from the final full-environment rerun;
the image lifecycle total comes from the final ordered-fixture rerun, and the
four other final suite results are unchanged. The targeted editor timing
recheck also passes three rows. Successful image/texture jobs observe 45/12 Qt
timer ticks while busy on this host; these are responsiveness observations,
not general latency guarantees.

The 44-file `out/FloraGPA-image-export-20261008/` package passes six checks in
a relocated directory with spaces/Chinese characters and a Windows-system-only
PATH: Windows image lifecycle, image writer formats/failures, GF2 chooser
ownership/texture refresh, Qt recovery and shipping GF2/BF1 GUI startup.
The first four checks have 7/31/8/3 Qt rows without failures/skips.
Only `FloraGPA.exe` changes from the preceding CSV-stream package. The other
43 files, including CLI/Worker, are byte-identical. The preceding 565-registration
matrix is inherited, not rerun: 555 unique captures, 526 completed registrations
/ 516 unique completions and 39 located refusals. Replay completion remains
separate from fidelity and capture-side missing information.
Four isolated-environment GF2/BF1 golden/control replays also pass: strict
golden image hashes, execution counts and suppressed-Draw negative controls.
The [pinned baseline](image-export-baseline.json) records 342 local evidence
files, source/test/package hashes and predecessor identity. Captures, packages
and generated evidence remain outside Git. These are same-host checks, not
independent clean-host deployment or renewed long-duration acceptance.

## Remaining boundaries

The QPixmap-to-QImage snapshot still occurs on the GUI thread. Codec conversion
and buffering may allocate memory beyond the I/O chunk. Texture copies detect
length/read errors, but do not claim cryptographic validation against an accepted
hash for every cached DDS/PNG/raw asset; same-length external mutation is outside
this check. Worker texture preparation is unchanged. Other synchronous analyzer
exports, multi-file recovery, clean-host deployment and renewed long-soak
acceptance remain open. M3/M4/M5 and module migration totals are unchanged.
