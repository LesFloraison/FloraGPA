# Persistent Qt recovery soak and current-source packaging

Historical batch: the newer [same-source release acceptance](CURRENT_SOURCE_RELEASE_ACCEPTANCE.md)
records the 525-case matrix and a renewed 248-cycle / 744-image recovery run
using tests built from the release archive. The 246-cycle results below remain
evidence for this preceding batch.

Reviewed 2026-10-08. This M5 batch extends recovery evidence to sustained use of
one Qt process/window and verifies the current production source from a fresh
archive. It does not change production replay code, shaders, formats or UI.

## Workflow and evidence ownership

`FloraRecoveryUiTests::originalCaptureRecovery` alternates the original GF2 and
BF1 captures in one MainWindow. Each completed cycle checks immediate open
cancellation, successful retry, a deliberately truncated-file failure preserving
the current capture/image, immediate replay cancellation, event navigation and
return to Final. Full-image hashes are checked after opening, after the failed
open retains the image, and after Final replay. The failed-open check adds no
modified game capture to the compatibility corpus.

The development runner relocates an unchanged release package beside the Qt
test executable and Qt6Test.dll. Only Windows system directories are on PATH.
The window is shown using Qt's offscreen plugin; production Worker processes
perform serial hardware replay. This tests the production UI library inside a
QtTest process, not a continuously running unmodified FloraGPA.exe. Separate
startup checks exercise the actual production GUI executable.

`--seconds` specifies a minimum duration, and `--pairs` a minimum number of
GF2/BF1 pairs. Both conditions must be met; the last pair finishes normally.
Every cycle preserves private bytes, working set, process handles, GDI/USER
counts, QObject counts by class, log size, elapsed time and reported adapter.
These are observations, not a leak-free or arbitrary-latency certification.
The test itself retains journal rows and QSignalSpy history; allocator capacity
and the application's retained log also affect process memory.

The runner freezes harness sources, executable/package hashes, original capture
identities, environment, logs and results. It refuses existing output directories.
Failures are not resumed or converted into successful cycles. Python is only the
development orchestrator and is not shipped in the application.

The subsequent runner-only correction `552d14b` reuses source-build validation's
process-tree timeout cleanup. Three CPU controls preserve zero/nonzero exit codes
and verify a timed-out parent and child both terminate. The sustained run uses
its original frozen runner; a separate short integration run verifies the revised
wrapper. The process helper's source is included in future frozen evidence.

## Journal failure and correction

The initial intended 30-minute run stopped after 87 complete cycles, at about
706 seconds. The assertion was a failed progress-file save before the next
truncated-file check; no preceding image mismatch was reported. The run remains
failed and its evidence is retained. It overlapped a separate CPU source build.
The initial helper did not retain the Qt error string, so the exact original I/O
cause is unproven. Free space was approximately 51 GB after failure, and no
replay/test child was left alive.

A deterministic negative control holds an old snapshot open without Windows
delete-sharing and reproduces a failed QSaveFile replacement with access denied.
This establishes a relevant observer-lock failure class, not the exact cause
of the first failure. The revised writer commits a new numbered snapshot for
each phase; readers can keep old snapshots open. It never replaces them. The
full `journal.json` is written once on completion. Compact progress snapshots
exclude accumulated history and retain the last observation. Disk usage therefore
grows with the number of operations rather than repeatedly storing all history.

The writer checks and reports [QSaveFile commit errors](https://doc.qt.io/qt-6/qsavefile.html#commit)
and refuses duplicate outputs. Unit controls verify old-file locking, new
snapshot/final acceptance, duplicate rejection and a blocked next destination
preserving earlier evidence. Final publication plus a successful Qt exit is
required; a progress file alone does not prove the test process completed.
The complete recovery suite passes eight rows without skips after this change.
Harness revisions are `0db936e` and `d656e29`; the latter replaces the initial
progress writer without changing production binaries.

## Current-source build

Source revision `0db936e` is exported with `git archive`, then configured with
`BUILD_TESTING=OFF`, compiled and packaged in a new Unicode/space directory with
an isolated process environment. The archive contains 1,049 source files and
excludes captures, AGENTS.md and generated solution/project files. CMake generates
the VS2022 solution. The 44-file package is
`out/FloraGPA-clean-build-20261008/`.

Four GF2/BF1 golden and disabled-Draw checks pass. The six-case mip-count gate
passes four strict native outputs and two exactly located missing-LOD rejections.
Actual production GUI/Worker startup, replay and window export pass for GF2 and
BF1; both screenshots were inspected. The existing TextureCopies.cpp C4018
warning remains. This is not a warning-free build or an independent clean host.

Production sources and packaging are unchanged between that archive revision
and the later journal correction. The preceding full 511-registration matrix
remains evidence for the unchanged implementation; it is not claimed as newly
rerun on the fresh binary set.

## Sustained-run result

The corrected run completes **246 cycles in 1,805.080 seconds** (30 minutes,
5 seconds): 123 GF2 and 123 BF1 cycles, **738 strict full-image hash checks**.
Each cancellation, failed-open retention, navigation and Final replay operation
passes. Qt reports three passed rows, zero failures/skips, and process exit zero.
All 1,723 immutable snapshots have consecutive identities, the expected phase
order and cycle observations identical to the final journal. Package, executable,
frozen-source, capture and final-report hashes are independently rechecked.
No recovery-test or Worker process remains after completion.

| Observation at completed cycle boundaries | GF2 | BF1 |
|---|---:|---:|
| Cycle time, median / maximum | 7.562 / 7.818 s | 7.104 / 8.419 s |
| Private bytes, first / last | 49.7 / 73.6 MiB | 43.9 / 62.9 MiB |
| Process handles, first / last (range) | 248 / 248 (248–250) | 248 / 248 (248–250) |
| GDI objects, range | 0 | 0 |
| USER objects, first / last (range) | 29 / 29 (29–30) | 29 / 29 (29–30) |
| Changed QObject class counts, first versus last | None | None |
| Retained log blocks, first / last | 4 / 980 | 8 / 984 |

Private bytes rise by approximately 24 and 19 MiB from the first respective
capture observations. After omitting each family's first two cycles, the last
five versus first five medians rise by 20.4 and 17.1 MiB. This run neither
attributes all growth to logs/test evidence nor proves a production leak.
Separating retained journal/log/test history, allocator capacity and live
application allocations is investigated in the subsequent
[retention controls](QT_RETENTION_CONTROL.md): test history/logs and bounded Qt
icon-cache ownership explain the measured short-run heap growth, without claiming
to account for every private byte in this 30-minute run. The prior
[allocation-stack and log-clear control](QT_ACTION_RETENTION.md) remains separate
evidence; its short native-window results are not substituted for this run.
The offscreen plugin explains why these GDI/USER counts do not represent a normal
Windows desktop window. Whole-cycle times are not cancellation-latency bounds.

The [baseline](persistent-qt-soak-baseline.json) pins the successful run, failed
initial run, resource observations, source build and focused package checks.
This is sustained functional recovery acceptance on this workload, not completion
of all M5 stability gates.

## Reproduction and remaining scope

```powershell
cmake --build --preset release --parallel 4 --target FloraRecoveryUiTests
python tools/validate_recovery_soak.py --package out/FloraGPA-clean-build-20261008 --test-exe build/vs2022/Release/FloraRecoveryUiTests.exe --qt-test-dll D:/Qt/6.11.2/msvc2022_64/bin/Qt6Test.dll --captures D:/CDXrepo/FloraGPA --out "artifacts/recovery-soak QA" --seconds 1800 --pairs 20
```

Use a new output directory for each run. During execution, inspect the latest
numbered file in `journal.json.progress`; older snapshots are immutable. The
final `validation.json` also requires a successful test exit and exact hashes.
The default recovery CTest stays short; it does not silently perform a 30-minute
soak on every build.

This workflow covers two deterministic games, one hardware adapter and one host.
It does not certify every analyzer workflow, driver/storage failure, device,
multi-hour session or clean Windows installation. The existing host has no
Windows Sandbox executable or active hypervisor; isolated PATH and fresh source
do not remove installed system components. New independent game captures and
other-device/clean-machine evidence remain necessary. M3/M4/M5 are incomplete,
and the 72/117/15 module ledger is unchanged.
