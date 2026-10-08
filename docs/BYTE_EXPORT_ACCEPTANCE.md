# Background buffer and resource byte exports

Reviewed 2026-10-08. Buffer and raw-resource exports previously wrote the entire
payload on the Qt event thread. Resource export also copied its full mapped span
into a QByteArray first. Both paths now use a background job and bounded 1 MiB
QSaveFile writes. No resource layout, replay operation or captured bytes change.

## Ownership, publication and cancellation

Buffer export retains the accepted implicitly shared QByteArray. Resource export
retains the immutable Frame that owns the mapped or normalized bytes; an edited
shader retains its own effective bytecode vector. Selection and capture changes
inside the save dialog therefore preserve the original requested asset. The
background writer never reads widgets or the current selection and does not
copy a whole mapped resource to prepare a write.

The shared export job manager owns its cancellation token and operation. It is
also used by the existing checked output-storage exporter. Completion uses
`exportFinished`, independently of replay completion. Cancel, capture changes,
close and destruction cancel pending work. Only one of these background export
jobs runs per window at a time.

The byte writer stages a new file, checks cancellation before each chunk and
again before commit, and leaves QSaveFile's direct-write fallback disabled.
Cancelled or failed staging preserves an existing destination; a failed final
replacement also preserves it. Once commit starts, later cancellation cannot
undo a completed replacement. The two-file output-storage export retains its
separately documented partial-publication boundary.

An export temporarily disables texture export through `setBusy`. Completion now
restores a previously enabled texture action only when its asset and revision
still match and no other work is active. It does not restore stale selection or
capture state. Progress and completion messages remain compact English strings;
the workspace layout is unchanged.

## Verification scope

The native byte suite checks empty, small, exact-chunk and multi-chunk data,
replacement of existing files, cancellation at all six checkpoints of a four
chunk export, removal of staging files, an obstructing destination directory,
Windows delete-sharing locks and subsequent retry.

GUI controls use 64 MiB captured resource data and accepted buffer data. They
exercise success, cancellation, capture switching, close, destruction, locked
publication and retry through the same export action. Destruction also verifies
that the background operation releases its mapped capture owner. Separate
controls verify texture-action restoration after resource and output-storage
exports. These synthetic fixtures exercise I/O and lifetime; they are not new
capture-format compatibility evidence. Existing original-GF2 ownership tests
compare exported bytes before and after a capture switch in the chooser.

The initial focused UI run was deliberately interrupted after identifying
redundant large-buffer reads in test setup. The interruption and process snapshot
are retained. Revised preparation keeps full 64 MiB buffer inspection but avoids
reading it twice; resource-export tests inspect 16 bytes while still exporting
and checking the complete 64 MiB mapped resource.

Implementation: `d8f13a1`. Six serial CTest suites pass 270 top-level Qt rows,
without failures or skips: byte writer 15, byte-export UI 16, output-storage
writer 29, complete UI 89, isolated Worker recovery 113 (111 scenarios plus
setup/cleanup), and persistent recovery 8. The 64 MiB successful exports record
UI timer activity and compare every byte.

The 44-file `out/FloraGPA-byte-export-20261008/` package passes six relocated
checks under a Windows-system-only PATH, including real Windows GUI lifecycle,
export ownership, recovery, GF2 and BF1 display. Four golden/control replays
retain strict GF2/BF1 image hashes and execution counts, with independently
suppressed Draw controls. The GF2 shipping-GUI screenshot was visually checked.
Only `FloraGPA.exe` differs from the preceding loop-mip package. Its other 43
files, including CLI and Worker, are byte-identical; the 565-registration /
555-unique-capture matrix is inherited, not rerun: 526 completed registrations /
516 unique completions and 39 located refusals. Replay completion does not imply
capture fidelity; the preceding fidelity and missing-information classifications
remain authoritative.

The [pinned baseline](byte-export-baseline.json) records package/source/test
hashes, exact test totals and 203 local evidence files. These include the initial
interrupted run, corrected final tests, frozen sources/test executables, portable
checks and goldens. Generated evidence remains outside Git. The package is a
same-host relocation check, not independent clean-host or renewed long-soak
acceptance.

## Remaining M5 work

The Worker still generates a complete `words.csv` for buffer reads, although the
Qt inspector uses the raw bytes and report instead. A 64 MiB buffer produces
16,777,216 data rows; source inspection and the test preparation expose this
unnecessary GUI-path work. Avoiding that work while preserving explicit CLI CSV
exports is the next large-buffer improvement. This batch does not change Worker
or CLI output contracts.

PNG encoding, texture-asset copies, geometry export, other analyzer exports,
multi-file recovery and independent clean-Windows deployment remain separate
work. This batch does not complete M3/M4/M5 or alter module migration totals.
