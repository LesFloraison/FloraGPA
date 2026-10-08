# Background geometry directory publication

Reviewed 2026-10-08. Geometry export previously created its public event
directory before synchronously copying each file on the event thread. A failed
copy could leave a partially exported directory. It now uses the existing
background export job and publishes the complete group by a same-parent Windows
directory rename after every copy has finished.
Implementation: `7f224c4`.

## Ownership and output contract

The accepted geometry cache owner, numeric event identity, filenames and lengths
are captured before opening the directory chooser. A capture/inspection change
inside that nested event loop does not change the export request. Missing or
length-changed requested assets fail explicitly rather than disappearing from
a newly enumerated list. A geometry.json report is required; optional stage
assets retain the preceding export allowlist: CSV files, OBJ, vertex/unique-vertex
storage and validity, and patch-constant storage and validity. Worker report.json
and unrelated files are not exported.

The selected parent must already exist. A new `.FloraGPA-Geometry-*` staging
directory is created there. Each file is copied through the shared checked
1 MiB/QSaveFile writer. Cancellation is checked between files, within copy
chunks and before publication. The completed group is renamed to
`FloraGPA-Geometry-<event>`, retaining numeric suffixes for collisions. Windows
MoveFileEx runs without replacement or copy fallback; at most 10,000 candidate
names are tried. Existing files/directories are never overwritten. A staging
failure or pre-publication cancellation cannot publish a partially named final
directory. Once the rename starts, cancellation does not roll it back.

The publication-lock test also proves a cleanup boundary: a third party holding
the staging directory without delete sharing blocks both rename and removal.
The export fails and the previous export remains unchanged, but an unpublished
staging directory can remain. Forced process termination can likewise leave
staging data. This is not crash recovery, a durability guarantee across power
loss, or automatic cleanup of arbitrary old staging directories.

## Verification

The 22-row writer suite covers complete allowlisted sets, exact file bytes,
existing file/directory collisions, four cancellation boundaries, missing,
shortened and grown sources, Windows read/publication locks, invalid destination,
malformed inventories, a missing report and retry. Input inventories reject
traversal, absolute paths, alternate data streams, case-insensitive duplicates
and negative lengths before staging.

Eight new isolated Worker recovery scenarios exercise the actual geometry UI:
success, cancel, capture switch, close, destruction, missing/grown source and
exclusive read lock. Each surviving window retries the same action. The fixture
uses an actual native IA geometry report plus an additional 64 MiB vertex asset
to stress export only; it is not new capture-format or GPU geometry coverage.
Files are compared byte-for-byte, existing exports must survive, cancelled and
failed jobs must not leave public export directories, and destruction waits for
background work before checking for late publication/cache retention.

The existing original-GF2 chooser-switch test now awaits independent geometry
export completion and compares every file and event directory with its baseline.
Six serial CTest suites pass 286 top-level Qt rows without failures/skips:
geometry export 22, shared image/copy writer 31, image/texture lifecycle 15,
complete UI 89, isolated Worker recovery 121 (119 scenarios plus setup/cleanup)
and persistent Qt recovery 8. The focused geometry UI run also passes all eight
new scenarios (10 top-level rows). All GPU/GUI checks run serially.
The 44-file `out/FloraGPA-geometry-export-20261008/` package passes six checks in
a relocated directory with spaces/Chinese characters and a Windows-system-only
PATH: geometry lifecycle (10 rows on Windows), writer failures (22), original-GF2
chooser ownership/texture refresh (8 on Windows), Qt recovery (3), and shipping
GF2/BF1 GUI startup. Four isolated-environment GF2/BF1 golden/control replays
also pass strict image hashes, execution counts and suppressed-Draw controls.

Only `FloraGPA.exe` changes from the preceding image-export package; CLI/Worker
and the other 41 files are byte-identical. Its preceding 565-registration /
555-unique-capture matrix is inherited, not rerun: 526 completed registrations /
516 unique completions and 39 located refusals. Completion remains distinct
from application fidelity and captured-information limits. The
[pinned baseline](geometry-export-baseline.json) records 186 local evidence
files, source/test/package hashes and predecessor identity. Captures and
generated artifacts remain outside Git. These same-host checks do not establish
independent clean-host or renewed long-duration acceptance.

## Remaining boundaries

Length/read checks do not authenticate same-length external mutations against
accepted asset hashes. The export inventory is the accepted cache's supported
file set, not a semantic proof that every stage-specific optional artifact was
generated. Worker geometry generation/model publication and other analyzer/API
log exports remain separate work. In particular, the API-log chooser still
reads the live frame/filter after closing and its exporter materializes the
complete JSON/CSV on the event thread; ownership and bounded/cancellable
generation are the next identified export task. Original replay semantics and module totals
are unchanged; M3/M4/M5, independent clean-host deployment and renewed long-soak
acceptance remain open.
