# Qt action connection retention

Reviewed 2026-10-06. This M5 change addresses one measured source of memory
retention during repeated capture recovery. It does not expand replay support,
modify shaders, change capture contents, or certify the application leak-free.

## Cause and counterexample

An opt-in allocation observer in the recovery test records UCRT heap allocations,
frees, reallocations and allocation stacks. It changes only the running test
process's import slots, restores them afterward, and is absent from production
executables. A malloc/realloc/free self-check passes. The fixed tracking table
reports no dropped records in either comparison run.

The growing group contains `Qt6Widgets.dll + 0x232351`, in
`QToolButton::setDefaultAction`, with Qt connection creation above it. The
[official Qt 6.11.2 source](https://raw.githubusercontent.com/qt/qtbase/v6.11.2/src/widgets/widgets/qtoolbutton.cpp)
explains the result: refreshing the same default action can add another private
`QAction::changed` connection, while the disconnect path requires a different
default action. The standalone negative control confirms that 100 disable/enable
pairs add 200 connections without creating additional QObjects.

| Recovery observation | Before: live blocks | After: live blocks | After: bytes |
|---|---:|---:|---:|
| 2 | 1,398 | 304 | 29,184 |
| 6 | 3,662 | 304 | 29,184 |
| 10 | 5,926 | 304 | 29,184 |

These counts describe the selected 96-byte allocation stack, not every related
allocation or all process memory. Legitimate retained model vector capacity and
static lookup tables also appear in the trace; old allocation age alone is not
classified as a leak. Cross-API frees and allocations outside the observed UCRT
imports limit the observer. Its reserved tracking storage also makes process
private-byte totals incomparable with uninstrumented runs.

## Scoped workaround

`installToolButtonConnectionGuard` is enabled only for the verified Qt 6.11.2
runtime and only for the MainWindow widget subtree. Action updates schedule one
coalesced cleanup per button and event-loop turn. After the original signal and
menu handling finish, detaching and restoring the current default action removes
the accumulating Qt-private connections through public Qt APIs. Application
signal connections remain attached. There is no production import hook, private
Qt API dependency or Qt DLL modification.

Waiting until the event loop resumes matters: an earlier immediate cleanup
failed the dynamic-menu test and was replaced. The accepted tests preserve late
menu attachment, explicit popup mode, action replacement/removal, text, tooltip,
enabled and checked state, user callbacks, click delivery and destruction with a
cleanup queued. Unrelated windows remain outside the guard. Bursts can still
create temporary connections until control returns to the event loop; the guard
does not claim a fixed allocation bound while the UI thread is blocked.

## Separate heap confirmation

A subsequent **40-cycle, 258.864-second** run uses the native Windows Qt plugin
from a relocated Unicode/space QA package directory with system-only PATH.
It does not enable the allocation-stack hook. The test executable contains the
new UI code; its Worker and Qt dependencies are identical to the release package.
All **80 strict Final-image checks** pass. HeapWalk completes for every snapshot.

| BF1 observation | Before fix | After fix |
|---|---:|---:|
| First heap busy bytes | 50,629,936 | 50,524,918 |
| Last heap busy bytes | 53,943,146 | 51,246,928 |
| Growth across 20 BF1 observations | 3,313,210 | 722,010 |
| Final bytes after isolated log clear | 53,198,760 | 50,502,702 |

In the after run, clearing the log alone releases 742,808 bytes. This accounts
for the scale of the remaining byte increase in this workflow; the final cleared
value is slightly below its first BF1 observation. It is not proof that every
remaining allocation is required or that other paths cannot leak. Production
log retention is unchanged and no log clearing is used to pass image assertions.

Qt child counts remain 4,556 by the same class distribution in every observation
(the additional object is the guard). After warm-up, process handles remain 324,
GDI objects 11 and USER objects 34. These results supersede the previously
unexplained connection-related retention in the shutdown audit while preserving
its independent shutdown correction and its other limitations.

## Regression and package scope

All four relevant CTest suites pass: main UI (57 passed, two optional fixtures
initially skipped), recovery UI (7 passed), Qt compatibility (6 passed) and
compatibility UI (4 passed). The two initially skipped original-capture workflows
also pass separately with the omitted-CB and Helldivers fixtures (4 rows including
setup/teardown, zero skips). Four fresh packaged GF2/BF1 golden and suppressed-Draw
checks pass with strict pixel, command-count and dependency assertions.

Runtime commit: `4b858b6`. The normal package is
`out/FloraGPA-qt-action-recovery-20261006/`; its 45-file inventory differs from the
preceding worker-shutdown package only in `FloraGPA.exe`. CLI, Worker and all
other packaged files are byte-identical. The complete GPU corpus was not rerun
for this GUI-only change. Inventory remains 486 registrations / 476 hashes,
460 positive registrations (450 unique) and 26 explicit rejections. The module
ledger remains 72 ported / 117 partial / 15 pending.

## Reproduction

Build `FloraQtCompatibilityTests` and `FloraRecoveryUiTests` using the Release
preset. `ctest --test-dir build/vs2022 -C Release -R '^qt_compatibility$'
runs the isolated behavior checks. For the allocation trace, set
`FLORA_TEST_CAPTURE_DIR` to the original GF2/BF1 capture directory,
`FLORA_RECOVERY_ITERATIONS=5`, and `FLORA_HEAP_TRACE_DIR` to an external output
directory, then run `FloraRecoveryUiTests originalCaptureRecovery -o <log>,txt`.
The optional trace can be large and stays outside Git. For the separate HeapWalk
run, omit `FLORA_HEAP_TRACE_DIR`, set `FLORA_RECOVERY_HEAP=1`, and use 20 iterations
(40 capture cycles). Every cycle checks cancellation, successful retry, event
navigation and two strict Final-image hashes.

The [pinned baseline](qt-action-retention-baseline.json) records the evidence hashes and validation scope. M3/M4/M5
remain incomplete; broader resource/query boundaries, longer-duration and broader
recovery paths, and independent clean-machine build/deployment remain open.
