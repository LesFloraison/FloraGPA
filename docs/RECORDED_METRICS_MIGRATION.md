# Native recorded counter sessions and publisher conversion

`MdRecordedQueries` ports `md_recorded_queries.py`: owned deferred command
lists, recorded counter scopes, repeated execution, collection, release, reports
and JSON/CSV export. It uses the installed Intel driver through the existing
native Metrics Discovery bridge. The original single-host-thread requirement
and lack of traditional frame-command-list parsing remain explicit.

`MetricPublisherValues` also ports the two publisher classes from
`md_publisher_values.py`, including calculated-value encoding, clock calibration
and refresh audits, timestamp/duration/busy transforms, recorded execution
identity, raw hashes, reports and CSV. That source module remains **partial**:
accepted-worker-sidecar loading/validation, profile projection and statistics /
requested-result analysis still depend on pending acquisition/planning modules.

## Recorded lifecycle

- A session retains independent active scopes per deferred context and deep
  copies each scope's metric metadata. Known-set metadata changes are rejected;
  later metric-set changes retain previous definitions and publication state.
- Finish owns the returned command-list reference and seals that context's
  recorded queries through the recovered notification lifecycle. Stable list
  IDs remain distinct from native command pointers.
- Execute drains a previous dirty execution before rearming its batches and
  sends precisely that list's tokens to the bridge. Poll results carry both
  token and execution identity. GPU submission order and collector delivery
  order need not coincide; both remain visible in the exported report.
- Release handles clean/unexecuted lists immediately and dirty lists through
  the pending queue. Recycled slots preserve their original insertion order,
  including empty slots, so native release and failure ordering remain stable.
- The audited drain overrides the existing query drain; notifications that
  trigger an implicit drain use the same audit hook. Shared core behavior is
  otherwise unchanged and covered by regression comparisons.
- Abort marks failure and releases every owned command/counter, preserving the
  first cleanup error. Close distinguishes completed and abandoned sessions.
  Failed or unfinished sessions cannot publish, while completed closed sessions
  retain their reports. Destruction adds best-effort native cleanup.

The default command-list operations call native DX11 FinishCommandList and
Release. Tests inject command-list operations without replacing the collector
algorithm. Transports, contexts and devices must outlive the session, and
callers must serialize its operations.

## Publisher and export semantics

The publisher preserves initial calibration, three-attempt failures, fixed clock
metadata, refresh-before-read, rollover-triggered recalibration and busy state
shared across the whole session. Results retain both original calculated values
and converted binary64 values with their exact 16-byte typed representation.
Unavailable report diagnostics remain unavailable. Null calculated values,
malformed raw hex and wrong recorded report lengths are rejected.

Exports retain `recorded-profile.json`, UTF-8 BOM CSV, CRLF records, quoting,
Unicode names, Python-compatible numeric text, field order and blank unavailable
values. Publisher CSV is present only when conversion is enabled. Existing
export directories are rejected. JSON is compared structurally; whitespace and
object key order are not part of its contract.

## Evidence

`tools/validate_recorded_metrics.py` executes the unchanged Python classes with
equivalent fake native transports and compares results after every operation.
The matrix covers 358 scenarios: 131 recorded sessions and 227 publisher cases.
It checks out-of-order list pointers, repeated executions, active contexts,
empty/unexecuted lists, clean/dirty release, metadata changes, failures,
abandonment, closed-session rules, exact CSV bytes and complete report/audit
content. Publisher cases include random uint64/float values, unavailable rows,
raw-byte validation, refresh failures, rollover and clock metadata changes.

`RecordedMetricsTests` additionally checks elapsed timeout cleanup and native
resource cleanup on destruction. `MetricsDiscoveryTests::hardwareLifecycle`
uses two real deferred contexts and two metric sets, with raw and converted
sessions, to verify 72 reports from repeated executions, CsThreads values,
publication identities, converted typed bytes, raw hashes, export and closed
report retention. Six native fault paths (Begin, End, Execute, Poll, timeout and
clock refresh) clear ownership and prevent failed publication; a fresh calibrated
session subsequently produces another report.

```powershell
python tools/validate_recorded_metrics.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraRecordedMetricsTests.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/recorded-metrics-new --isolated-env
$env:FLORA_TEST_INTEL_METRICS = '1'
$env:FLORA_TEST_REFERENCE_ROOT = 'D:/CDXrepo/FloraGPA'
ctest --preset release -R '^(recorded_metrics|metrics_discovery|metric_core|metric_collector)$'
```

No Python/GPA runtime is used by the native adapters. Tests on this host do not
establish reliability across every driver, capture or machine. Higher-level
profiling, remaining publisher analysis and Intel metrics Qt controls are still
pending; the existing UI is unchanged by this backend migration.

## Delivery acceptance

- Full Release build: `artifacts/build-recorded-metrics-release.log`.
- Eight relevant CTest suites: `artifacts/ctest-recorded-metrics-release.log`.
- Packaged 358-scenario parity:
  `artifacts/recorded-metrics-package-parity/validation.json`.
- Regressions: 728 collector scenarios in
  `artifacts/recorded-metrics-collector-regression/validation.json`, 6,961 shared
  core cases in `artifacts/recorded-metrics-core-regression/validation.json`, and
  5,100 MD cases (4,781 saved reports) in
  `artifacts/recorded-metrics-md-regression/validation.json`.
- Packaged native tests under Windows-only PATH passed without skips: four
  recorded test cases and five Intel integration cases in
  `artifacts/recorded-metrics-package-tests.txt` and
  `artifacts/recorded-metrics-package-integration.txt`.
- All four ordinary and disabled-draw GF2/BF1 controls passed:
  `artifacts/recorded-metrics-package-golden/validation.json`.
- Package: `out/FloraGPA-recorded-metrics/`. The audit at
  `artifacts/recorded-metrics-delivery-audit.json` checks all executable/bridge
  hashes against Release, package-local Qt loading and absence of Python/Tk/GPA
  or RenderDoc modules in ordinary replay. Temporary validation tools were moved
  into `artifacts/recorded-metrics-test-tools/`.

The ledger records 45 ported, 116 partial and 43 pending modules. These counts
describe module classification, not the percentage of engineering work done.
