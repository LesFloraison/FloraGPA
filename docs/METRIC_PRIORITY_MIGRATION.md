# Metric priority arbitration and publisher sidecar acceptance

The native library implements `metric_priority.py`, `metric_priority_win32.py`
and `md_acquisition_priority.py`. The remaining `md_publisher_values.py` loader
is also migrated, completing that module with its existing native conversion,
CSV and statistical analysis APIs.

## Behavior

- `MetricPriorityTable` preserves the v2 byte layout: 16 resources, 1,024 process/
  thread entries per resource, original magic/version and LUID names. Allocation,
  duplicate/capacity rejection, signed priorities, GLOBAL/UI rules and ownership
  match the reference. Dead-process cleanup leaves owner TID and cached maximum
  stale; an explicit priority update may be needed after a higher-priority owner
  dies. Repeated ownership is not recursive: one release clears it.
- `SharedMetricPriorityMutex` uses the same Windows kernel mutex and Documents/GPA
  file namespace. Every table operation holds the guard. It preserves bounded
  acquisition, timeout withdrawal, registering-thread restrictions, abandoned
  guard recovery and last-user cleanup. Corrupt files are never replaced silently;
  deletion failures are audited. C++ destruction adds best-effort cleanup; normal
  scopes use explicit close to preserve reported failures.
- `MetricAcquisitionPriority::run/replay` raises priority to 7 per uniform pass,
  records acquisition/completion/failure, and atomically replaces the audit file.
  A replay exception closes begun counters before withdrawing priority and
  releasing ownership. The native Metrics Discovery overload and injectable
  transport/save interfaces use the same coordinator.
- Priority audit acceptance preserves optional legacy profiles, canonical file
  names, LUID identity, final ownership state and exact pass/priority rosters.
  Publisher acceptance validates source identities, metric/information fields,
  names, units and binary64 equality with typed bytes. Boolean scalars, signed-zero
  changes and nonfinite typed values are rejected. The reference's recorded-
  iteration mode continues to bypass the uniform priority audit.

## Evidence

| Check | Result |
| --- | --- |
| Native/Python comparison | 350 scenarios; 5,860 table operations with full-buffer SHA-256 after every step |
| Acquisition failures | Factory/save/priority/acquire/work/release/device-close/lock-close failures, nested replay and active close; matching callback order and audit reports |
| Publisher files | Saved GF2/BF1/edited results, altered references/identities/fields, required audits, signed zero and nonfinite typed encodings |
| Actual processes | 83 checks with C++/C++, C++/Python and C++/original GPA peers; timeouts, nonpreemption, GLOBAL/UI, crashes, stale maxima and abandoned guards |
| Packaged native tests | 6 priority and 5 Intel integration cases, no skips |
| Intel integration | Three arbitrated Dispatch samples; injected failure after Begin closes device and ownership; a fresh device/session retries successfully |
| Regressions | 3,043 analysis comparisons plus 479 saved GPA checks, 358 recorded/publisher comparisons and 10 relevant CTest suites |
| Packaged replay | GF2/BF1 golden frames and both negative controls |

All checks passed. Process tests use unique private files/mutexes and do not
touch GPA's default coordination file. Lock-only tests perform no GPU work.
The optional pinned original GPA peer runs in a separate research process;
none of the 13 native peer processes loaded Python or GPA modules. Inventories
are retained. The production implementation loads no GPA code.

Initial comparisons passed; later cases added typed-value and deletion-failure
coverage. A test variable shadow warning was corrected, followed by a rebuild
and rerun of Intel integration. OS errors retain native Windows/Qt diagnostics;
protocol rejections and injected failure records match the reference.

Local evidence (ignored, not shipped):

- `artifacts/build-priority-release.log`
- `artifacts/build-priority-final-test.log`
- `artifacts/build-priority-integration-final.log`
- `artifacts/ctest-priority-release.log`
- `artifacts/priority-package-parity/validation.json`
- `artifacts/priority-package-processes-final/validation.json`
- `artifacts/priority-package-tests.txt`
- `artifacts/priority-package-integration-final.txt`
- `artifacts/priority-analysis-regression/validation.json`
- `artifacts/priority-recorded-regression/validation.json`
- `artifacts/priority-package-golden/validation.json`
- `artifacts/priority-delivery-audit.json`

The self-contained package is `out/FloraGPA-metric-priority/`. Tests use its Qt
DLLs and Windows-only system paths. Test executables and Qt6Test are removed
from the final distribution. This is host-local validation, not evidence of
execution on a separate clean Windows installation.

## Remaining integration

Higher-level pass/iteration scheduling, profiling worker/controller integration,
hotspot consumers and Intel metric Qt controls remain incomplete. They must be
connected before the complete Python profiling workflow is available in the
analyzer. This batch migrates dependencies and changes no analyzer layout.
