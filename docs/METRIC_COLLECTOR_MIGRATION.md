# Native publisher subscriptions and scheduled counter pool

The recovered collector now has native provider binding, context slots,
allocation/reuse and deferred notifications. Its scheduled Intel Metrics
Discovery adapter uses the same C++ collector and the existing native bridge.

| Original Python module | C++ implementation |
| --- | --- |
| `metric_query_pool.py` | `application/MetricCollector.h/.cpp` |
| `metric_deferred_queries.py` | `application/MetricCollector.h/.cpp` |
| `metric_subscriptions.py` | `application/MetricCollector.h/.cpp` |
| `md_scheduled_pool.py` | `application/MdScheduledPool.h/.cpp` |

`MetricSampleTransport` exposes the existing bridge operations for both real
hardware and deterministic validation. `MetricsDiscovery` implements it directly.
`MetricPublisherObserver` supplies clock refresh and record/refresh counts; the
full `md_publisher_values.py` conversion, analysis and export adapter remains a
separate pending module. No Python runtime or GPA DLL is used by this C++ code.

## Preserved behavior

- Query pools refill complete batches, skip null providers/results, preserve
  partial allocation on a factory error and use free-list LIFO / pending FIFO.
  Appending a null batch does not create a slot. The uint32 refill wrap guard
  remains in place. Context zero resolves to the first known immediate context.
- Subscriptions compare object identity, preserve ordered provider alternatives,
  use unsigned compatibility intersections and preferred categories, and retain
  only the last special category `-1` during pool construction. Existing-category
  subscription does not rebuild; removing an unused provider does.
- Collector Begin reuses a free batch or directly dispatches the oldest pending
  batch. That fallback does not refresh the clock, and a failed dispatch does
  not pretend to start a fresh measurement. End operates on the last batch in
  the matching context. Optional deferred reads preserve the original timing.
- Notification kinds 2/3/0 seal, arm and release deferred batches. Duplicate keys
  consume the new recording list but preserve the previous sealed list. Dirty
  release moves work to pending; clean release recycles it. Context slots and
  sealed lists survive pool rebuilds.
- Scheduled samples preserve token/report/drain attribution, readiness versus
  diagnostic availability, bounded waiting, delivery in state 3 and stable
  native Counter identity across batch reuse. A consumer receives mutable result
  storage, as in the reference. Close releases owned samples and clears the
  native cache, while allowing later reuse of the scheduler.
- Error paths preserve cleanup order and exception precedence. A native release
  failure can leave driver-owned samples outstanding and prevent cache clearing;
  the adapter reports that failure rather than claiming cleanup succeeded.

Calls must be serialized. Transport and observer lifetimes must exceed the
scheduled pool's lifetime. Generic collector rebuilds discard pool ownership
without guaranteeing delivery of outstanding results, matching the reference;
native adapters are responsible for their resource cleanup. Destruction performs
best-effort cleanup; explicit `close()` exposes cleanup errors.

## Evidence

`tools/validate_metric_collector.py` executes the unchanged Python modules and
the native probe with equivalent providers, clocks, sinks and sample transports.
It compares **535 scenarios** after every operation, including complete traces,
batch keys/states, provider identity, contexts, queues, callbacks, records,
audits, errors and cleanup state:

| Area | Scenarios |
| --- | ---: |
| Provider ordering | 7 |
| Pool allocation/reuse | 165 |
| Context mapping | 5 |
| Subscriptions, collection and deferred notifications | 276 |
| Scheduled samples and failure/retry paths | 82 |

Failures cover allocation, native Begin/info/submit/poll/recycle, clock refresh,
consumer callbacks and release/cache/statistics cleanup. Explicit scenarios
check failed fallback dispatch, uint64 keys, uint32 tags, duplicate seals and
rebuild with retained sealed lists. Dynamically invalid Python scalar types are
represented by the C++ API's fixed-width scalar types rather than a new JSON API.

`MetricCollectorTests` separately exercises timeout cleanup and retry, plus
empty-query batch identity. `MetricsDiscoveryTests::hardwareLifecycle` connects
the same scheduled pool to the installed Intel driver. Nine varied Dispatch
workloads verify callback order, actual `CsThreads` results, raw report decoding,
stable batch/Counter pairing, reuse counts, released ownership and restart after
close. Hardware completion timing is not treated as a deterministic poll count.

```powershell
python tools/validate_metric_collector.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraMetricCollectorTests.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/metric-collector-new --isolated-env
$env:FLORA_TEST_INTEL_METRICS = '1'
$env:FLORA_TEST_REFERENCE_ROOT = 'D:/CDXrepo/FloraGPA'
ctest --preset release -R '^(metric_collector|metric_core|metrics_discovery)$'
```

The reference files and test executables are development dependencies only.

## Delivery acceptance

- Full Release build: `artifacts/build-metric-collector-release.log`.
- Five related CTest suites passed, including the full main-window UI suite:
  `artifacts/ctest-metric-collector-release.log`.
- The delivered package passed all 535 exact comparisons under a Windows-only
  PATH, using its own Qt DLLs:
  `artifacts/metric-collector-package-parity/validation.json`.
- Shared clock/report/query regression: 6,961 comparisons passed in
  `artifacts/metric-collector-core-regression/validation.json`.
- Existing bridge and decode regression: 5,100 comparisons passed, including
  4,781 saved reports, in
  `artifacts/metric-collector-md-regression/validation.json`.
- Packaged native tests passed without skips: four collector cases in
  `artifacts/metric-collector-package-tests.txt` and five Intel integration cases
  in `artifacts/metric-collector-package-integration.txt`.
- All four GF2/BF1 ordinary replay and disabled-draw controls passed in
  `artifacts/metric-collector-package-golden/validation.json`.
- `out/FloraGPA-metric-collector/` contains the application package.
  `artifacts/metric-collector-delivery-audit.json` verifies all four executable
  and bridge hashes against Release, package-local Qt loading and absence of
  Python/Tk/GPA/RenderDoc modules during ordinary replay. Temporary test tools
  were moved to `artifacts/metric-collector-test-tools/`.

The migration ledger records 41 ported, 115 partial and 48 pending modules.
These are module classifications, not a percentage of engineering work.

## Remaining integration

This completes the four listed source modules, not the full metrics feature.
The older `md_sample_pool.py` / `md_reusing_pool.py` policies, split-drain and
recorded adapters, publisher value conversion/export, acquisition arbitration,
iteration/pass planning and higher-level profiling remain separate migration
work. Intel profiling controls have not been added to the Qt interface yet.
Deferred notifications do not imply general command-list replay from frame
files. The original scheduler's explicit `complete_original_scheduling=false`
and single-immediate-context policy remain unchanged.
