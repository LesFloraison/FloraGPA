# Native Metrics Discovery foundation

This migrates `devices.py`, `metric_kinds.py` and `metrics_discovery.py` into
native C++ components. It does not complete Intel hardware profiling: the
publisher, collector, multi-pass scheduler and their Qt consumers still need
their own migrations. There is no new hardware-metrics UI in this change.

## Device and driver ownership

`replay/Device` creates the actual default hardware, WARP or explicitly selected
vendor device. It preserves the original feature-level negotiation and reports
adapter identity, memory, selected driver and measurement scope. `Replay` uses
this factory and exposes its borrowed device pointer so counter collection can
attach to the same device that replays the frame.

`application/MetricsDiscovery` owns a bridge session through RAII. The bridge
holds its own D3D11 references and matches the installed Intel MD adapter by the
DXGI device LUID. WARP and other vendors are rejected by the bridge. Driver
discovery requires exactly one `igdmd64.dll` beside a loaded `igd10iumd64.dll`
inside the canonical Windows DriverStore directory. Neither GPA nor Python is
part of this runtime path.

The default bridge is `FloraGPA.Metrics.dll` beside the executable. CMake builds
it from `src/metrics/Bridge.cpp`; packaging copies it and the Intel public header
license. The installed Intel driver binary is not redistributed. Normal replay
does not load Metrics Discovery.

## Preserved interface

- Annotated DX11 catalogs preserve names, descriptions, units, storage types,
  publisher kinds and aggregation categories. Missing historical kind metadata
  remains missing; no aggregation weights are invented.
- Begin/end, raw report decoding, clock pairs and split submit/poll/discard retain
  the original bridge state checks and error behavior.
- Independent samples preserve unique tokens, out-of-order reads, repeated
  reads, active/pending cleanup, 256-counter bounds and optional object reuse.
- Recorded counters preserve deferred contexts, execution identities, repeated
  command-list execution, stale execution rejection and explicit release.
- Optional APIs are enabled only when every export in their capability group is
  present. Historical bridges are tested separately.
- Results preserve uint32/uint64 storage, finite numeric values, booleans and
  unavailable scalars. Seven driver flags gate availability in the original
  order. A frequency-change flag alone does not invalidate a report. Raw bytes
  and SHA-256 provenance remain available.

The C++ API uses typed pointers, booleans, unsigned counts and uint64 tokens;
Python-only dynamic argument types have no C++ equivalent. Zero tokens, invalid
counts, duplicate executions and native state errors are still rejected.

## Source provenance

The bridge is the original standalone project's native bridge, reformatted for
maintenance without changing its implementation. Original source SHA-256:
`6883aadf88cf12a4f72c0eafc37a8220e08f85cc5ee7b34c47b6319abae2e034`.

The vendored Intel public interface is pinned at upstream commit
`b798d05c3c535d3840eccbba23670584c26dd1a9`. Header SHA-256:
`6cf0c5d6be3ac6c329f3e1241a2da520af89f3b37ac476fbab072c45ef8e9333`.
See `third_party/metrics-discovery/source.json` and `THIRD_PARTY.md`.

## Validation

`tools/validate_metrics_native.py` compares the original Python API to the native
test probe. The initial run passed **5,100 comparisons**, including the complete
annotated hardware catalog, device descriptions, descriptor special cases,
invalid result forms and **4,781 saved raw reports** from GF2/BF1. Every saved
report reproduces all values, storage types, availability reasons and raw bytes.
This is a raw-report decoding comparison, not a claim that a new frame profiling
session has been ported.

`tests/MetricsDiscoveryTests.cpp` exercises actual Intel compute work with
RenderBasic and ComputeBasic. It holds eight reports before reading in reverse
order, checks hardware SIMD thread counts, written buffer elements, stable repeated
reads and raw re-decoding. It also checks split completion, object reuse,
deferred execution, stale IDs, closed sessions, WARP rejection and old bridge
capabilities. Intel-specific tests are opt-in; generic scalar validation runs
without Intel hardware.

The original `validate_md_samples.py --reuse` passed **142 checks / 24 reports**
against the newly built bridge. The original recorded validator passed
**171 checks / 36 reports** with only its primary bridge path redirected to the
new DLL; its historical compatibility DLL remains unchanged. These validate the
native bridge ABI and behavior; the Python schedulers they use are still pending
native migration.

Initial evidence under ignored `artifacts/`:

- `metrics-native-parity-v1/validation.json`
- `metrics-native-tests.txt`
- `metrics-bridge-samples-v1/validation.json`
- `metrics-bridge-recorded-v1/validation.json`
- `validate-ported-recorded.py` (reference-test bridge-path adapter)

Reproduce native/reference comparisons with your local reference workspace:

```powershell
python tools/validate_metrics_native.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraMetricsTests.exe --bridge build/vs2022/Release/FloraGPA.Metrics.dll --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/metrics-comparison-new
$env:FLORA_TEST_INTEL_METRICS = '1'
$env:FLORA_TEST_REFERENCE_ROOT = 'D:/CDXrepo/FloraGPA'
ctest --preset release -R '^metrics_discovery$'
python tools/validate_metrics_bridge.py --reference D:/CDXrepo/FloraGPA --bridge build/vs2022/Release/FloraGPA.Metrics.dll --mode recorded --out artifacts/metrics-recorded-new
```

These evidence files establish behavior on this host's Intel adapter and
installed driver. Other Intel generations and drivers have not been tested.

## Release acceptance

- Complete Release build succeeded: `artifacts/build-metrics-release.log`.
- All 16 relevant CTest suites passed, including 54 original UI cases without
  skips and all five native metrics test cases without skips:
  `artifacts/ctest-metrics-regression.log` and
  `build/vs2022/metrics-discovery-Release.txt`.
- The package at `out/FloraGPA-metrics-foundation/` passed the same 5,100
  comparisons with isolated native child paths and all five metrics cases with
  Windows-only PATH: `artifacts/metrics-package-parity/validation.json` and
  `artifacts/metrics-package-tests.txt`. Temporary test tools were subsequently
  moved to `artifacts/metrics-package-test-tools/`; they are not distributed.
- Both ordinary GF2/BF1 golden frames and disabled-draw negative controls passed
  in the package: `artifacts/metrics-package-golden/validation.json`.
  Ordinary replay module inventories contain no Python/Tk/GPA/RenderDoc runtime,
  and every loaded Qt module is package-local.
- `artifacts/metrics-package-audit.json` records exact package/Release hash
  equality for all four executables and the bridge. The bridge SHA-256 is
  `58b6278f2a93e783b1e8bb1290e9b0e51745f1a36825f296584f52159fd7c6db`.

The migration ledger is now 32 ported, 115 partial and 57 pending modules out of
204. These are module counts, not percentages of remaining engineering work.
