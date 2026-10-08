# Intel metric catalog acceptance

This M5 change moves existing Intel catalog acceptance into the cancellable
Worker report job. It adds no counter collection algorithm or analyzer feature.

## Reproduced failure and correction

Previously, a successful catalog worker caused MainWindow to read and parse
`catalog.json` synchronously, then call the three metric views. Their setters
cleared and repopulated tables while accessing unvalidated fields. A later
malformed metric could throw after an earlier row had already replaced the
previous accepted catalog.

The preserved old-code control `artifacts/m5-catalog-old-publication.txt`
demonstrates precisely that failure: a malformed later metric leaves `NewMetric`
displayed where the test retained `OldMetric`. The initial control's setup used
a stale request serial and did not reach this path; that failure is preserved
separately, not treated as reproduction evidence.

MainWindow now hands the catalog directory to the same owned background job
used for checked reports. The catalog has its own fixed filename and no
`report.json` envelope. Chunked reading checks cancellation and final file size;
native JSON parsing retains integer precision, rejects malformed input and
limits nesting. The full catalog is validated before any view setter runs:

- `sets` and every `metrics` member must be arrays.
- Each set and metric must have a nonempty string name.
- Set names must be unique; metric names must be unique within one set.
- Each metric's display label and unit must be strings; empty strings are valid.
- A symbol may occur in multiple distinct sets, and an empty catalog is valid.

Errors identify `catalog.json` and, for structural fields, the offending set or
metric index. Additional catalog metadata is preserved. Collection-specific
plans, metric signatures, hardware availability and result semantics retain
their existing independent validation.

The existing request serial, frame revision, cancellation flag and shared
temporary-directory lifetime control publication. A cancelled, superseded or
destroyed request cannot publish its catalog. Catalog handling returns before
assigning the frame's replay report. Failed catalog loads retain prior tables
and frame output; corrected requests can retry.

## Scope of responsiveness

File reading, parsing and structural validation run off the event thread.
Individual JSON string-token parsing is not interruptible inside the parser;
cancellation is observed at subsequent callbacks and before publication.
Building Qt trees and copying accepted catalog metadata still occur on the UI
thread. This change does not claim bounded-time publication for arbitrarily
large catalogs, transactional recovery from process-wide allocation failure,
or completion of all M5 consumers. Scheduled/uniform result acceptance, history,
other specialized readers, large model publication and exports remain separate
work.

## Validation and test-driver correction

Implementation `b2d7562` passes the checked-reader suite and all 97 isolated
Worker recovery scenarios, including ten new catalog cases. The new cases cover
valid, missing, truncated, non-object and late-malformed catalogs; a 64 MiB
background read; cancellation; frame switching; window closing/destruction;
directory cleanup; corrected catalog retry; and real Worker replay afterward.
The successful large-file control records 45 heartbeat ticks with a maximum
12 ms gap during background acceptance on this run. This is an observation,
not a bound on all UI publication costs.

The reader control checks every proper truncated prefix, 13 structural
mutations, malformed UTF-8/trailing/deep JSON, integer fidelity, empty catalogs,
symbols shared between distinct sets, each observed cancellation point and retry.
An actual native catalog contains 27 sets and 595 metric definitions.

Two initial full metric-UI suites timed out in their existing experiment-file
dialog helper. A separate 45-second instrumented run reached real collection,
cancellation, retry and experiment saving/loading, then recorded the second
Open experiment dialog still visible. A bounded driver exposed both a rejected
selection and an accepted default filename instead of the requested path.
Simply retrying the old select/accept calls was insufficient.

The [matching Qt 6.11.2 source](https://raw.githubusercontent.com/qt/qtbase/v6.11.2/src/widgets/dialogs/qfiledialog.cpp)
leaves the filename editor unchanged when the visible dialog's editor has focus.
Test-only correction `6198c8d` releases that focus, verifies the selected full
path before requesting acceptance, waits for the accepted signal, and rejects
a still-visible dialog after ten seconds. It keeps exact path assertions.
The temporary probe was removed from test sources; its source and observations,
initial setup/build mistakes, timeout logs and unsuccessful driver attempts are
retained as evidence. No production dialog, counter value, acceptance threshold
or replay behavior was changed to pass these tests.

The final five relevant suites pass **150 top-level Qt rows, zero skips**:

| Suite | Rows |
|---|---:|
| Worker report | 30 |
| Worker recovery | 99 (97 isolated scenarios) |
| Recovery UI | 8 |
| Uniform metrics UI | 6 |
| Scheduled metrics UI | 7 |

The two metric suites include the real Intel bridge, saved-result identity
checks, displayed/exported values, failed bridge/cancel recovery and experiment
file round trips. Their final rerun takes 53.37 seconds. These assertions do not
independently calibrate counter accuracy or resolve remaining M6 analysis limits.
The original four-suite command remains failed and preserved; its successful
Worker/recovery results are combined with the two corrected metric-suite runs.

The 44-file package `out/FloraGPA-catalog-20261008/` changes only `FloraGPA.exe`
relative to the accepted switch-mip package. CLI, Worker and every other file
are byte-identical. The existing 533-registration replay matrix therefore
remains inherited through unchanged replay binaries; it was not rerun here.
Four GF2/BF1 golden and disabled-Draw controls pass. Seven relocated package
checks pass from a Unicode/space directory with system-only PATH, including four
Windows-platform catalog scenarios, four GF2/BF1 recovery cycles with twelve
strict image checks, and two shipping GUI screenshots. GF2, BF1 and the real
scheduled-metric UI screenshots were inspected.

The [baseline](metric-catalog-baseline.json) pins 101 evidence files, production
package hashes, source hashes and test executable hashes. This is same-host
package acceptance, not independent clean-host deployment or a renewed sustained
soak. Module bookkeeping and DX11 capture support are unchanged. M3/M4/M5 remain
incomplete.

## Reproduction

```powershell
cmake --build --preset release --parallel 4 --target FloraGPA FloraWorkerReportTests FloraWorkerRecoveryTests FloraRecoveryUiTests FloraUniformMetricsUiTests FloraScheduledMetricsUiTests
ctest --test-dir build/vs2022 -C Release -R '^(worker_report|worker_recovery|recovery_ui|uniform_metrics_ui|scheduled_metrics_ui)$' --output-on-failure -j 1
```

The saved-result and actual Intel collection cases require the environment
variables documented in their test sources. Run GPU checks serially. Recorded
fixtures are external developer data, not shipped captures or runtime Python.
