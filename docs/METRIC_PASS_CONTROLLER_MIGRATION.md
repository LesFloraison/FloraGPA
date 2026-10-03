# Metric pass controller and callback consumer

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

`MetricPassController.h/.cpp` migrates `metric_pass_controller.py` (including
probe fanout) and `metric_consumer.py` into the native application library.
The controller uses the existing `MetricPriorityLock` interface, so real
`SharedMetricPriorityMutex` objects and injectable test locks share its path.
`MetricPassPublisher` and `MetricProbeBackend` expose the publisher boundary
needed by the remaining production MD transport and probe registry.

## Preserved behavior

- Constructor checks retain positive uint64 metric/consumer handles, strict
  descriptor indices, unique requested handles, uint32 device keys and pool
  capacities. Non-Intel plans require a secondary priority lock.
- Preparation copies requests, applies ordered compatibility grouping and
  forwards the exact device key to the configuration provider. Failed
  configuration retains the old key while committing the new plan. A provider
  exception retains the intermediate state and reported count. Empty requests
  return an empty plan and a reported count of one, preserving prior state.
- Selection retains the same-pass no-op, secondary priority 8 followed by
  primary priority 7, ignored acquisition return values, pass-zero consumer
  reset, and the 256-query capacity limit unless the pass contains the timing
  handle. Shared OS lock timeouts still throw through the existing lock API.
- Begin forwards only for the matching device key; End preserves the uint32
  completion-count wrap. Probe fanout visits nonzero handles in order and
  stops at the first failure, leaving earlier probes active for owner cleanup.
  Flush forwards even for empty requests. Finish can preserve subscriptions
  and current-pass identity; empty requests return before releasing locks.
  Unsubscribe return statuses are ignored, while thrown exceptions propagate.
- Callback accumulation advances a separate cursor per metric. Numeric
  callbacks accept masked uint64/double headers, retain `-1.0` for absent or
  mismatched numeric storage, initialize unfilled columns to zero, and preserve
  binary64 bits including signed zero, infinities and NaN payloads. The timing
  handle uses 52-byte records, truncates finite timestamp doubles into uint64
  and wraps negative duration differences. Earlier timing records survive a
  later malformed record. Absent callbacks bypass validation; failed roster
  resets preserve the previous state.

The recovered state machine intentionally does not add automatic GPU or lock
cleanup to early returns. Owners must close active queries before releasing
acquisition ownership. These compatibility rows also have no event identities;
they must not replace the existing identity-checked MD profile records.

## Evidence

The development and packaged probes passed **494 scenarios / 4,445 action
steps**, comparing results, errors, full state snapshots and call traces with
the original Python implementation. The same packaged run checked **1,908
saved original GPA observations**:

| Saved source | Sequences | Checked snapshots |
| --- | ---: | ---: |
| Numeric/timing callbacks | 102 | 504, including resets |
| Pass lifetime | 54 | 972 |
| Pass preparation/device key | 48 | 432 |

Preparation comparisons replay the recorded configuration provider status and
check manager state and output. They do not claim to validate the unported
native probe registry's internal maps. Lifetime tests include its recorded
publisher/lock calls. Source modules, saved manifests and the original binary
are hash-identified; no GPA code executes in these comparisons.

Additional cases cover callback bounds and header errors, partial timing
updates, probe ordering, every injected lifecycle failure, reconfiguration,
duplicate requests, lock acquisition returning false, retained pass identity,
empty requests and completion-count overflow.

The Intel hardware integration test connects the new controller and consumer
to `MdScheduledPool`, a real MD device and a private shared priority mutex:

1. Prepare two passes over RenderBasic and ComputeBasic.
2. Collect three Dispatch workloads per pass and verify all callback columns.
3. Confirm mismatched device keys do not begin a query and finished passes
   release ownership.
4. Inject a failure in the second probe after the first starts a real query;
   verify it remains active until owner cleanup, close it before releasing the
   lock, and successfully collect again with no owned/cached samples left.

Full Release compilation and **12 related CTest suites** passed. Existing
regressions passed 2,758 iteration comparisons plus 1,907 saved observations,
and 728 collector comparisons. The portable package passed four controller
Qt Test cases and five Intel integration cases without skips, using only
package-local Qt and Windows system paths. GF2/BF1 golden replays and their
two draw-suppression negative controls also passed.

Two initial failures were test-adapter mistakes: the recorded QueryPoolSize
header is `0x18400800`, and the lock audit exposes `depth`, not `owned`.
Both assertions were corrected and their affected tests rebuilt/rerun. The
final results above include these corrections; earlier artifacts are retained.

Local evidence (ignored by Git):

- `artifacts/build-pass-controller-release.log`
- `artifacts/build-pass-controller-integration-final.log`
- `artifacts/ctest-pass-controller-release.log`
- `artifacts/pass-controller-package-parity/validation.json`
- `artifacts/pass-controller-package-tests.txt`
- `artifacts/pass-controller-package-integration.txt`
- `artifacts/pass-controller-iterations-regression/validation.json`
- `artifacts/pass-controller-collector-regression/validation.json`
- `artifacts/pass-controller-package-golden/validation.json`
- `artifacts/pass-controller-delivery-audit.json`

The current portable application directory is `out/FloraGPA-pass-controller/`.
Test-only binaries are moved out after verification. The test publisher bridges
the native components on this host. Probe registration/configuration and callback
result assembly were subsequently migrated in [probe registry](METRIC_PROBE_REGISTRY_MIGRATION.md).
The complete production MD iteration transport and profiling worker/Qt flow
remain pending. This batch changes no analyzer controls and does
not imply that the entire Python profiling workflow is available in the UI.
