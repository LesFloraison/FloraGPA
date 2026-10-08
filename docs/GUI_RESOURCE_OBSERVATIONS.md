# Native GUI resource observations

Reviewed 2026-10-09. Harness implementation `5774174` adds opt-in current-process
GUI snapshots and a read-only evidence analyzer. Production code and the 44-file
`out/FloraGPA-map-nowait-20261008/` candidate are unchanged. This investigation
follows the unresolved GDI step in the [30-minute Windows run](NATIVE_WINDOWS_SOAK_ACCEPTANCE.md).
It does not establish a leak, identify an allocation owner, or close M5.

## What the new instrumentation establishes

`validate_recovery_soak.py --gui-resources` requires `--platform windows` and
`--retention-control`, and cannot be combined with allocation hooks. The test
saves immutable snapshots before constructing MainWindow, after every completed
cycle, at four clearing controls, and after destroying MainWindow. It records:

- Process GDI, USER and handle counts before and after each observation.
- Qt top-level widget classes, object names, visibility and creation state,
  plus Qt window classes, visibility, exposure and parent presence.
- Current-process top-level desktop window classes and handles, and loaded
  code-module names and paths, with explicit enumeration-completeness flags.

The probe does not call `winId()` or create native windows to inspect them. It
does not read window titles or other processes' contents. The snapshot field
`native_created` records `Qt::WA_WState_Created`; it is a QWidget creation-state
observation, not proof of an individually owned native handle. Child and
message-only Win32 windows, data-file modules and allocation ownership are
outside the inventory. Snapshots are sequential rather than atomic; handles
can be reused. The final observation occurs while QApplication is still alive,
not after all Qt/process globals have been destroyed.

The runner rejects incomplete inventories, mismatched platforms/processes,
invalid counts, missing/extra snapshot files and journal-summary mismatches.
`analyze_gui_resources.py` additionally checks accepted journal and snapshot
hashes before reporting chronological count, window-class and loaded-module
changes. It never labels a correlated module as the resource owner.

## Focused validation

- Native `gui_resources` CTest: six Qt rows pass, including a controlled four-pen
  GDI allocation/release, hidden-widget non-materialization, visible-widget
  destruction and rejection of evidence overwrite.
- Existing `recovery_ui` CTest: eight Qt rows pass, including original captures.
- Runner CPU tests: 22 pass; analyzer CPU tests: five pass, including modified
  metadata with unchanged counts, altered journals, missing files and rejected
  incomplete/unaccepted runs.

The first build failed for a missing QWidget include; the corrected build and
both CTests pass. The initial adapted no-workflow recheck used an incorrect
phase count; the corrected audit passes without changing or rerunning capture
evidence. Preliminary logs remain in local artifacts.

## Two short controls

Both runs use the same test executable and production package, alternating the
unchanged GF2 and BF1 originals four times each, on this host with the actual
Windows Qt backend. They run serially from relocated paths containing spaces
and Chinese characters with system-only PATH. Optional workflows include Query
inspection, API export and Contexts/Command Lists export. Their automated file
choosers use Qt dialogs, not OS-native file-picker dialogs.

| Observation | With optional workflows | Without optional workflows |
|---|---:|---:|
| Completed cycles | 8 | 8 |
| Duration | 91.552 s | 63.439 s |
| Strict per-cycle image checks | 32 | 24 |
| Post-clear image checks | 1 | 1 |
| Retained exports | 32 | 0 (not requested) |
| Immutable progress snapshots | 65 | 57 |
| GUI snapshots | 14 | 14 |
| Before MainWindow: GDI / USER / handles | 2 / 11 / 254 | 2 / 11 / 254 |
| First cycle: GDI / USER / handles | 56 / 53 / 468 | 20 / 47 / 407 |
| Last cycle: GDI / USER / handles | 56 / 52 / 460 | 24 / 49 / 414 |
| After pixmap-cache clear: GDI / USER / handles | 56 / 51 / 460 | 24 / 48 / 414 |
| After MainWindow destruction: GDI / USER / handles | 54 / 43 / 456 | 22 / 41 / 410 |

All image checks pass. All 32 exports match the preceding accepted workflow
control's per-capture bytes. Frozen sources, binaries, package files, immutable
progress and six complete address/heap snapshots per run are rechecked. All
28 GUI snapshots have identical resource counts before and after the probe;
this is a measured result for these observations, not universal zero overhead.
The [pinned evidence index](gui-resource-observations-baseline.json) records the
identities and compact analysis. Large generated evidence stays outside Git.

Both processes start with no Qt top-level widgets, report 146 at their first
completed cycle, and return to zero after MainWindow destruction. The native
top-level inventory goes from three to six to four: the MainWindow and its
title-bar window disappear, while Qt observer windows and input-method windows
remain. Those remaining windows do not account for or identify all GDI objects.

The no-workflow run changes from 20 to 24 GDI objects at cycle two, alongside
new codec/common-control module observations. The workflow run stays at 56
through all eight cycles and loads additional Shell-related modules before its
first cycle observation. Module changes are correlations only. These are single
short controls in separate processes; they do not isolate Query inspection,
file choosers, exports, input methods, timing or any individual module.

## Reproduction and remaining boundary

Build `FloraRecoveryUiTests` and `FloraGuiResourceTests` with the release preset,
then run GPU tests serially. For a new, non-existing output directory:

```powershell
python tools/validate_recovery_soak.py --package out/FloraGPA-map-nowait-20261008 `
  --test-exe build/vs2022/Release/FloraRecoveryUiTests.exe `
  --qt-test-dll D:/Qt/6.11.2/msvc2022_64/bin/Qt6Test.dll `
  --captures D:/CDXrepo/FloraGPA --out 'artifacts/new GUI control' `
  --seconds 0 --pairs 4 --platform windows --workflows `
  --retention-control --memory-maps --gui-resources
python tools/analyze_gui_resources.py 'artifacts/new GUI control'
```

Omit `--workflows` for the second control; use a different output directory.
For sustained evidence, use a suitable nonzero minimum duration. These pilots
do not replace the preceding 30-minute run or reproduce its late `56 -> 65`
step. A renewed sustained observation and narrower operation-level controls
remain necessary before changing production resource handling. QApplication
global caches and process-exit reclamation are not measured by destroying
MainWindow alone; retained counts must not be presented as a confirmed leak.

No full compatibility matrix, original-player comparison, clean-host acceptance
or new source-archive build is claimed in this batch. Existing replay evidence
is inherited solely through the unchanged package identity. M3/M4/M5 and the
72 ported / 117 partial / 15 pending ledger remain incomplete.
