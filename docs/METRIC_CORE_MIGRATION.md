# Native publisher clock, report values and query drain

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

The shared computation and query lifecycle from these original modules now live
in C++:

| Python module | Native implementation |
| --- | --- |
| `metric_clock.py` | `application/MetricClock.h/.cpp` |
| `metric_report_values.py` | `application/MetricReport.h/.cpp` |
| `metric_report_postprocess.py` | `application/MetricReport.h/.cpp` |
| `metric_query_batch.py` | `application/MetricQueries.h/.cpp` |
| `metric_query_drain.py` | `application/MetricQueries.h/.cpp` |

These are shared components for the pending publisher and hardware collection
adapters. They do not add a hardware-metrics panel or claim that the full
profiling scheduler is migrated. The original independent implementation's
limits remain: no invented calibration or frequency source, no OA stream
splitting/repair, and no claim of complete original GPA scheduling.

## Clock and report semantics

Clock conversion preserves modulo-64-bit arithmetic, the original overflow in
ticks-to-nanoseconds conversion, three-attempt MD calibration, signed offset
comparison, the zero previous-offset sentinel and the strict 95%/5% rollover
thresholds. Calls to the provider occur in the same order, including the two
separate maximum-period reads in a successful rollover test. Signed arithmetic
uses unsigned intermediates and bit casts to avoid undefined C++ overflow.

Calculated MD values remain 16-byte records. The reader retains uint32, uint64,
float32, low-byte boolean and unknown-kind-zero behavior. A missing report is
distinct from an empty report. Writing to a metric preserves writes preceding
a missing field and publishes its key only on successful completion.

Postprocessing copies report records, calculates all aligned keys before
normalizing timestamps, truncates integer GpuTime from nanoseconds to
microseconds and shares Busy state across calls. Original headers and unused
payload bytes survive. Busy conversion preserves direct integer-to-float32
rounding for the denominator, the separate binary64-to-float32 numerator path,
unsigned key subtraction, division/rounding order and the MINSS behavior for
unordered input. Original raw driver values remain the source for diagnostics;
publisher binary64 values are a separate, potentially lossy representation.

## Query ownership and lifecycle

Queries and sinks use explicit C++ interfaces. Batches retain forward begin,
reverse end, the original four states, the finite `0xffffff` waiting bound,
first-attempt-only flushing, validity checks, category selection and completion
ordering. Errors preserve prior calls and the state reached before the error.
Null sinks are skipped; missing query objects fail at use rather than causing a
native dereference fault.

The pending pool preserves shared byte-sized failure state and wraparound.
Failed items can be skipped after the failure threshold, but an inspected prefix
is erased only if at least one item succeeded. Deferred lists retain dirty/arm
semantics. The collector refreshes its clock before reading, processes numbered
slots, visits deferred lists in sorted order and flushes sinks last. The public
slot map is named `bySlot` to avoid Qt's `slots` macro.

Integer IDs, counts, timestamps and flags have explicit C++ types. Python-only
dynamic type checks become caller-side typing; JSON/file entry points must still
validate inputs before invoking these native components.

## Verification

`tools/validate_metric_core.py` invokes the unchanged Python modules as the
reference and compares a native probe's outputs, exact float/record bytes,
provider calls, state transitions, partial writes, failure state and queue
contents. Coverage includes random and boundary input, integer rounding
midpoints, invalid report shapes/layouts, clock retry failure, rollover, null
queries/sinks, category mismatch, failures during callbacks, cross-slot failure
state, absent collectors and deferred-list rearming.

The validator additionally consumes three saved original GPA postprocessing
observations from the reference workspace: `report-postprocess-oracle-v2`,
`report-postprocess-mixed` and `report-postprocess-rollover`. It first verifies
that the Python reference still reproduces their recorded bytes, aligned keys
and Busy state, then compares the same **120 groups / 1,080 reports** with C++.
The rollover observations also retain original clock state and callback traces;
the two older evidence formats do not contain those fields. Neither comparison
process loads a GPA DLL.

`MetricCoreTests::finiteWaitBound` separately executes the entire unsuccessful
wait limit and verifies exactly one flushing request, without collecting millions
of trace entries. `MetricsDiscoveryTests::hardwareLifecycle` connects the native
batch and collector directly to the installed Intel driver's real compute
counter. It verifies refresh-before-read, delivery, recycling, clock calibration
and native postprocessing of the returned hardware report.

Initial validation exposed an empty-slot omission in the test fixture builder;
the fixture now preserves explicitly empty slots. A compile-time collision with
Qt's `slots` macro was resolved through the public `bySlot` name. The historical
GPA evidence loader now respects the fields actually present in each saved
format. These corrections are retained in the validation history rather than
counting failed attempts as successful checks.

Reproduce the comparison with external reference evidence:

```powershell
# Run from the repository root; set external paths for your environment.
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
$ReferenceRoot = 'C:/reference/FloraGPA'
python tools/validate_metric_core.py --reference "$ReferenceRoot" --exe build/vs2022/Release/FloraMetricCoreTests.exe --qt-bin "$QtRoot/bin" --out artifacts/metric-core-new
$env:FLORA_TEST_INTEL_METRICS = '1'
$env:FLORA_TEST_REFERENCE_ROOT = "$ReferenceRoot"
ctest --preset release -R '^(metric_core|metrics_discovery)$'
```

The reference files and development probe are not required by the application.

## Delivery acceptance

- Full Release build: `artifacts/build-metric-core-delivery.log`.
- **6,961 comparisons passed**, including 604 batch, 800 pending-pool and 350
  collector scenarios. Exact final results and probe hash are recorded in
  `artifacts/metric-core-delivery-parity/validation.json`.
- Five related CTest suites passed in `artifacts/ctest-metric-core-release.log`.
  The final package additionally passed all four core test cases and five Intel
  integration test cases without skips under Windows-only PATH:
  `artifacts/metric-core-delivery-tests.txt` and
  `artifacts/metric-core-delivery-integration.txt`.
- The package at `out/FloraGPA-metric-core/` passed all four ordinary and
  disabled-draw GF2/BF1 golden checks in
  `artifacts/metric-core-delivery-golden/validation.json`.
- `artifacts/metric-core-delivery-audit.json` confirms that all four executables
  and the bridge match Release. Ordinary replay contains no Python/Tk/GPA or
  RenderDoc runtime modules and loads Qt from the package. Temporary validation
  executables and Qt Test were moved into `artifacts/metric-core-test-tools/`.

The ledger now records 37 ported, 115 partial and 52 pending modules. These counts
do not measure the fraction of engineering work completed. Higher-level
publisher conversion/export, scheduling, acquisition arbitration, collections
and Qt consumers remain incomplete.
