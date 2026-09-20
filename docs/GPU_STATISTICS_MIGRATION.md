# Native event and command-range GPU statistics

The recovered `gpu_statistics.py` and `statistics_ui.py` paths now run through
C++ DX11 queries and an isolated native Worker. The Qt **GPU Statistics** page
provides Event, Range and Frame measurements, API-selection endpoints, structured
details and JSON/CSV export. **Annotations > Range Metrics** transfers a closed
group's inclusive endpoints without starting a measurement. Hardware/WARP comes
from the existing application toolbar. No Python or GPA backend is used.

## Query and replay boundaries

- A single-event sample first replays preceding commands and prepares the selected
  draw/dispatch's edited inputs and pipeline. It then brackets submission. Input
  restoration follows the measurement. Disabled events yield a valid empty sample.
- A range starts before its first whole replay command and ends after its last.
  Binding, lazy uploads, experiment helpers, restoration and submission gaps are
  inside that interval. Frame measurement uses the first and last captured API IDs.
- Endpoints must be existing API commands in ascending order. Single-event samples
  require a Draw/Dispatch. Before-event and legacy aggregate-timing options cannot
  be combined with this query schedule.
- Queries cover all eleven pipeline counters, occlusion samples, legacy and four
  per-stream SO statistics, overflow, timestamp frequency/disjoint state and start/
  end ticks. Event-query completion precedes result reads, with bounded waits.
- Query scope closes on exceptions. A failed run invalidates the previous sample;
  only a completed replay publishes results. Reusing a Replay creates fresh queries
  and increments the reported replay generation.

Reports retain actual query values, device/adapter identity, feature level,
experiment provenance, predication or range membership, disabled commands,
DrawAuto reconstruction and reference-compatible replay counts. uint64 values
remain integers through JSON, the Qt details tree and CSV. CSV uses a UTF-8 BOM.
Qt rejects stale results after capture, event, range, device or experiment changes;
controls and export are disabled while the shared Worker is busy.

## Evidence and limits

`tools/validate_gpu_statistics_port.py` compares complete metadata and counters to
the Python consumer, verifies timestamp arithmetic and exported CSV, and audits
native loaded modules. Timing values themselves are independent samples and are
not expected to match between executions.

- Final independent-package matrix: **92/92 checks passed**, consisting of **78
  successful samples and 14 expected structured rejections**
  (`artifacts/statistics-package-final/validation.json`). Includes hardware/WARP,
  individual draws/dispatches, clears/copies, inclusive endpoints, disabled edits,
  all SO raster streams/no-rasterization, predication, DrawAuto and overlapping
  queries. Additional event/range cases cover scoped vertex-input edits, actual
  GS replacement with doubled output, disabled SO producers and output-binding
  experiments. The validator requires success for every positive case; agreement
  on an unexpected rejection is a failure.
- Earlier 72-check evidence overstated successful SO/predication coverage: 36
  legacy fixtures had 40-byte rasterizer descriptors tagged as DESC2 and were
  rejected by both implementations. The final validator creates separate copies
  with the matching DESC type tag, records their provenance, and retains explicit
  malformed-descriptor rejection cases. Original reference files are untouched.
  Those earlier artifacts are superseded by the final matrix.
- Real GF2/BF1 matrix: **21/22 strict comparisons passed**
  (`artifacts/statistics-real-v2/validation.json`). Both devices cover first/last
  work, every submission API present in those captures and full command ranges.
  The sole difference is BF1 hardware whole-frame `ps_invocations` (and its CSV
  cell); the other counters and metadata match. WARP whole-frame matches exactly.
- Three more complete-frame pairs per capture/device preserve the same outcome:
  **9/12 strict comparisons passed** (`artifacts/statistics-repeat-v1/validation.json`).
  BF1 hardware PS counts were Python 46,278,690 / 46,278,675 / 46,278,674 and
  native 46,278,666 / 46,278,698 / 46,278,697. Earlier pairs also varied. This is
  evidence of repeat-to-repeat variability in both implementations, not proof of
  bit-exact invocation parity. Strict comparison failures remain in the artifacts;
  no tolerance or counter substitution conceals them.
- `StatisticsTests` passes all six QtTest cases including setup/cleanup
  (`artifacts/statistics-tests-final.txt`). Analytic triangles verify event/range/clear
  boundaries and disabled draws on both devices, unchanged final pixels, repeated
  replay, exception cleanup/retry and invalid endpoints. Qt checks cover actual
  MainWindow/Worker range/frame and WARP-event samples, annotation handoff, stale
  results, busy controls, cancellation/retry and exact export above the JSON-double integer range.
  Rendered evidence: `artifacts/statistics-ui/statistics-main-window.png`.

Release passed all **38 CTest suites** (`artifacts/ctest-statistics.log`), including
the existing **53 Qt cases without failures or skips**
(`artifacts/statistics-ui-regression.txt`). The additional cancellation/retry
check then passed in the focused final statistics suite. Production binaries did
not change after the full CTest run.

The package also passed GF2/BF1 golden images and both suppressed-draw negative
controls (`artifacts/validation-statistics-package/validation.json`). Real-frame
package statistics repeat the **21/22 exact** outcome, with only BF1 hardware PS
variability remaining (`artifacts/statistics-real-package/validation.json`).
All 104 successful native reports use package-local Qt and load no Python, Tk,
GPA or RenderDoc modules (74 distinct module paths;
`artifacts/statistics-package-runtime.json`). This is an isolated runtime test on
the development host, not validation on a separate clean machine.

Portable application: `out/FloraGPA-statistics/FloraGPA.exe`.
CLI/Worker SHA-256: `58fd3d16b1b1803df1ba18d2723d6dae0f75e1cfa31dcf7bcd39bd5227af122e`.
GUI SHA-256: `ebebddfa6c2da5af18c400492b2c6b428a503380c50d159b7554e728494de25a`.

Single samples are replay-device measurements. WARP is software execution; SO
overflow can describe missing target capacity rather than an invalid memory
write. DrawAuto can include SO queries and synchronization. These timings must
not be interpreted as original-application timings or a sum of isolated events.

The complete Python migration is still unfinished. Private metrics, shader
instrumentation/debugging and pixel-history consumers remain separate work.
Annotation chart grouping is not present in the recovered Python panel and is
outside this migration's scope. The manifest stays partial; this batch does not establish
support for additional unrecovered capture command layouts or other GPUs.
