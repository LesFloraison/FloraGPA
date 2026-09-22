# Native synchronous and FIFO counter acquisition

Three existing Python acquisition policies now have C++ implementations in
`src/application/MdSamplePool.h/.cpp`:

| Original module | Native adapter | Policy |
| --- | --- | --- |
| `md_query_drain.py` | `MdCounter` | One active counter; wait and deliver after End |
| `md_sample_pool.py` | `MdSamplePool` | Bounded asynchronous FIFO; release delivered samples |
| `md_reusing_pool.py` | `MdReusingPool` | FIFO delivery with LIFO reuse of batches and native Counters |

They use the previously migrated `MetricQueryBatch`, `MetricQueryDrain` and
`MetricQueryPool`. `MetricsDiscovery` implements the sample and split-counter
transport interfaces directly. Tests supply deterministic implementations of
those interfaces, without replacing the collection algorithm.

## Behavior preserved

`MdCounter` submits the active query before refreshing the publisher clock and
waiting for its result. It retains raw diagnostic availability independently of
transport readiness. Its audit records the publication index, clock refresh
range, flush/read attempts, reset batch state and recycled count. Both successful
and failed End calls discard the native counter; discard failure retains the
same unfinished lifecycle and exception precedence as the original code.

The FIFO policy inspects at most one oldest sample per drain. After Submit it
tries without waiting; at capacity or Finish it waits. Report delivery preserves
callback order, mutable decoded result storage, publication index, drain index,
poll traces and state zero after dispatch. The synchronous End convenience method
still drains earlier outstanding work before returning its own result.

The reusable policy additionally allocates capacity-sized batches, pairs native
Counter and query identities, takes free batches LIFO and waits for the oldest
pending batch at exhaustion. It resets stale tokens before Begin and registers
ownership before allocation so a later info/identity failure can release the
new sample. Counter reuse occurs after dispatch has reset the batch to state
zero. Unlike the scheduled collector adapter, exhaustion goes through the
normal clock-refreshing drain. Each policy retains its original audit strings
and `complete_original_scheduling=false` declaration.

Close attempts all owned releases, clears local ownership and permits later
reuse. The reusable variant then clears cached native objects and records pool
statistics. Release/cache/statistics errors preserve their original precedence;
a native release failure can leave native ownership outstanding. Explicit Close
reports errors. C++ destructors add best-effort resource cleanup. Calls must be
serialized, and the borrowed transport and publisher must outlive each adapter.

Fixed-width C++ arguments encode the original integer domains; capacity and
timeout bounds and bridge capability requirements are validated explicitly.
These adapters do not change the original bridge's hardware or report validity
restrictions.

## Validation

`tools/validate_metric_collector.py` compares unchanged Python modules with the
native probe after every action. The expanded matrix contains 728 scenarios:
the previous collector tests plus synchronous, FIFO and reusable policies,
consumer mutation, identity mismatch, capability rejection, failure and retry.
Complete callback traces, result bytes/values, ownership, native cache state,
clock refreshes and report audits must match.

`MetricCollectorTests::adapterTimeoutCleanup` separately verifies real elapsed
timeouts and subsequent retry for all three adapters. A timing-dependent number
of unsuccessful polls is not treated as a deterministic parity requirement.

`MetricsDiscoveryTests::hardwareLifecycle` runs nine varied Dispatch workloads
through each adapter using the installed Intel driver. It verifies publication
order, actual `CsThreads` counts, raw decode equality, bounded ownership,
state-zero delivery and native Counter identity/use counts. Both pool policies
also execute another workload after Close. The existing scheduled policy and
recorded bridge tests remain in the same integration test.

```powershell
python tools/validate_metric_collector.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraMetricCollectorTests.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/metric-adapters-new --isolated-env
$env:FLORA_TEST_INTEL_METRICS = '1'
$env:FLORA_TEST_REFERENCE_ROOT = 'D:/CDXrepo/FloraGPA'
ctest --preset release -R '^(metric_collector|metric_core|metrics_discovery)$'
```

## Delivery acceptance

- Full Release build: `artifacts/build-metric-adapters-release.log`.
- Seven relevant CTest suites passed: `artifacts/ctest-metric-adapters-release.log`.
- Packaged exact parity: 728 scenarios in
  `artifacts/metric-adapters-package-parity/validation.json`.
- Regressions: 5,100 MD cases, including 4,781 saved reports, in
  `artifacts/metric-adapters-md-regression/validation.json`; 6,961 shared core
  comparisons in `artifacts/metric-adapters-core-regression/validation.json`.
- Packaged tests under Windows-only PATH: five collector cases and five Intel
  integration cases passed without skips in
  `artifacts/metric-adapters-package-tests.txt` and
  `artifacts/metric-adapters-package-integration.txt`.
- All four GF2/BF1 ordinary and disabled-draw controls passed:
  `artifacts/metric-adapters-package-golden/validation.json`.
- Application: `out/FloraGPA-metric-adapters/`. The delivery audit in
  `artifacts/metric-adapters-delivery-audit.json` checks all executable/bridge
  hashes against Release and package-local Qt loading without Python/Tk/GPA or
  RenderDoc modules during ordinary replay. Temporary test tools were moved to
  `artifacts/metric-adapters-test-tools/`.

The ledger now records 44 ported, 115 partial and 45 pending modules. These
classifications are not a percentage of engineering work.

## Remaining integration

The recorded-command adapter, publisher conversion/export, acquisition
arbitration, iteration/pass planning and full profiling entry points are still
separate pending modules. Intel profiling UI controls are not exposed yet. This
batch completes three acquisition-policy modules; it does not establish parity
for every Python module or every capture.
