# Metric iteration analysis and request planning

`MetricAnalysis` ports `metric_values.py`, `metric_passes.py` and
`metric_planner.py` into the native application library. It also implements
the profile projection and analysis helpers from `md_publisher_values.py`.
The latter module remains partial because its worker-sidecar loader and
priority-ownership validation are not yet migrated.

## Behavior

- Binary64 statistics retain sequential summation, stable signed-zero ordering,
  NaN filtering, even-median overflow, signed variation and zero-weight behavior.
  The empty vector and an all-NaN vector remain distinct. Iteration combination
  retains sum kinds 0/1/2/5, weighted means, empty-side copies and shape checks.
  Range aggregation combines iterations before computing statistics, preserving
  input order within each group.
- Pass assembly transposes iteration/metric/range vectors and validates metric
  identities and rectangular shapes. Profile matrices validate the complete
  set/range/iteration roster, accepted pass flags, event/interval/FrameFile
  boundaries and record identities. Reordering serialized records has no effect;
  missing entire ranges, sets or iterations cannot become a smaller accepted run.
- Publication uses null for unavailable, nonfinite or boolean statistical
  samples. Raw requested values retain uint64 precision and the reference's
  separate boolean availability policy. Statistics intentionally use binary64.
- Planning preserves request order and candidate order, wildcard compatibility,
  pass intersections and first-catalog-set selection. It is not a minimum-pass
  optimizer and retains the original metadata-consistency checks and limits.
- Publisher analysis first projects converted values onto the accepted source
  identities, changes GpuTime units to microseconds, then builds matrices,
  summaries and requested results. It preserves the raw availability flags and
  never rescales precomputed raw statistics or mutates the input profile.

Integers are compared without converting IDs to doubles or allowing signed/
unsigned wraparound. Frame-range rows retain the original numeric-equality
policy, including integral floats and boolean boundary aliases where Python
accepts them; explicitly typed identities still reject booleans.

## Evidence

`tools/validate_metric_analysis.py` executes 3,043 native/Python comparisons and
479 additional checks against saved original GPA observations (3,522 checks
total):

| Reference | Coverage | Comparison |
| --- | --- | --- |
| Python arithmetic | NaN/Inf, signed zero, overflow, sum/weighted kinds, aggregation, empty and malformed input | Exact binary64 bits; NaNs classified |
| Python profiles/plans | Three range modes, shuffled records, missing/duplicate identities, unavailable samples, uint64 boundaries, request assignments, converted publisher data | Exact output types, fields, ordering and finite bits; matching explicit rejection messages |
| GPA numerical oracle | 200 construction/combination observations | Relative/absolute tolerance 1e-14; NaN/Inf classified |
| GPA assembly oracle | 42 recorded matrix observations | Exact finite bits and integer identities; NaNs classified |
| GPA planner oracle | 237 ordered grouping observations | Exact pass membership and order |
| Saved capture profiles | Five GF2/BF1/edited/hotspot profiles, 32 raw records, three publisher sidecars; installed descriptor catalog | Exact Python output |

The validator pins source, fixture and oracle binary hashes. It reads saved
observations; it does not load GPA DLLs. Synthetic cases include the candidate-
order regression `[[2], [1], [1, 2]]`, complete-range omissions, altered accepted
flags, duplicate descriptors, invalid IDs and converted statistics distinct from
raw statistics. Seeds and probe inputs/outputs are retained with the reports.

The first comparison found uint64-max compatibility IDs being accepted as -1
through JSON signed/unsigned comparison. Explicit integer comparison fixed it;
additional large-range and integer/float identity cases now pass. The first
validator launch also exposed a case-sensitive lookup in a copied environment
dictionary; the runner now constructs a filtered Windows environment explicitly.
The final added publisher unit test initially had an initializer syntax error;
that build and its stale-probe comparison are not acceptance evidence. The
corrected full build, new probe and six-case native test run all passed. The
accepted numeric boundary aliases also have an explicit requested-result check.

Release build and nine relevant CTest suites passed. The initial CTest run skipped
two opt-in Intel checks; a subsequent packaged run enabled both historical bridge
compatibility and real Intel hardware lifecycle, passing all five cases without
skips. Existing recorded/publisher and clock/report/query comparisons passed
358 and 6,961 cases respectively. No replay implementation changed in this batch.

The new self-contained package is `out/FloraGPA-metric-analysis/`. With only its
own Qt runtime and Windows system paths, the new analysis matrix and six native
analysis test cases passed. Packaged Intel integration passed five cases without
skips. GF2/BF1 golden/negative replay results and final binary hashes are recorded
in the delivery evidence below. Test executables and Qt6Test are kept outside
the final distribution after validation.

Local evidence (ignored, not distributed):

- `artifacts/build-metric-analysis-verified.log`
- `artifacts/ctest-metric-analysis-final.log (plus the corrected metric_analysis rerun)`
- `artifacts/metric-analysis-package-verified-parity/validation.json`
- `artifacts/metric-analysis-recorded-regression/validation.json`
- `artifacts/metric-analysis-core-regression/validation.json`
- `artifacts/metric-analysis-package-verified-tests.txt`
- `artifacts/metric-analysis-package-integration.txt`
- `artifacts/metric-analysis-package-final-golden/validation.json`
- `artifacts/metric-analysis-delivery-audit.json`

## Remaining integration

These APIs are callable from C++ and introduce no Python runtime dependency.
They are not yet exposed as a complete Intel metric profiling workflow in the
CLI/worker or Qt analyzer. Priority arbitration, sidecar acceptance, higher-level
iteration/pass scheduling, hotspot consumers and the compact Qt metric controls
remain to be migrated and connected. Existing reference limitations concerning
driver formulas, special pass zero and full scheduling remain unchanged.
Saved observations and two golden captures do not prove arbitrary-capture parity.
