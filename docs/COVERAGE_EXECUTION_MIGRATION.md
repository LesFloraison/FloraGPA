# Native coverage execution

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** The subsequent [Coverage UI batch](COVERAGE_UI_MIGRATION.md) connects the desktop workflow; [Quad UI](QUAD_UI_MIGRATION.md) has separate acceptance. Backend limits recorded below remain scoped to their cases.

`application/Coverage.cpp` executes the original fragment and geometry coverage
diagnostics with D3D11. The CLI and isolated native worker expose `coverage`.
The subsequent [Qt coverage migration](COVERAGE_UI_MIGRATION.md) connects its
controls, image navigation and exports to the analyzer. Quad consumers remain
to be migrated. This is not completion of the application.

```powershell
# Run from the repository root; set external paths for your environment.
$CaptureRoot = 'C:/captures'
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
.\out\FloraGPA-coverage\FloraGPA.Cli.exe coverage "$CaptureRoot/sample.gpa_frame" --id 430 --out "$ResultsRoot/coverage"
```

Options: `--coverage-mode fragment|geometry`, `--coverage-target auto|depth|rt0..rt7`,
`--coverage-layer <index>`, `--ignore-depth`, `--warp`, `--experiment <project>`
or `--disable <events>`. Coverage owns an inclusive replay boundary; it cannot
be combined with before-event, timestamp or suppressed-draw replay modes.
An explicitly disabled event is supported and produces an empty mask.

The output directory contains `coverage.json`, `coverage.png`, `after_draw.png`
and `overlay.png`, plus the CLI transport report. Reports preserve original
resource/view/event identities, target coordinates, instrumentation, original
and diagnostic submission counts, sample provenance and limitations.

## Execution and state preservation

- A spare RTV receives an additional output from the original pixel shader.
  Its discard, depth output and shader side effects execute once.
- Full MRTs, dual-source/logic blending, buffer RTVs, geometry mode and targetless
  draws use a separate diagnostic draw on private output copies. Original work
  still passes through the normal replay command once. Hidden UAV counters and
  aliased native resource owners are retained; diagnostic work does not write
  original SO buffers or advance their append cursors.
- Geometry mode uses a replacement pixel shader and read-only captured
  depth/stencil tests. Ignoring those tests affects the diagnostic only. Fragment
  mode retains the original experiment behavior, including its after-draw image.
- RTV/DSV selection follows edited bindings and native descriptors. Array layers,
  mips, volume slices, 1D resources, typed buffer ranges and MSAA are retained.
- Targetless draws use actual viewport coordinates and a neutral background.
  UAV relocation includes pre-raster VS/HS/DS/GS writers and high slots. Array
  selection retains shader work from nonselected indices, while MAX blending
  accumulates the selected pixel union.
- Private draws and readback helpers are excluded from captured predicate query
  intervals. Inspection does not add to normal draw counts or SO history.

The readback path retains actual COM output owners across scoped input edits.
MSAA storage reading now accepts those owners without changing the public
resource-ID inspection API. Non-RGBA8 display conversion uses a separate native
hardware preview device, matching the original Python behavior even during WARP
replay. An initial CPU conversion differed at WARP depth quantization boundaries;
it was replaced with the original GPU conversion rather than relaxing comparison.

## Verification

`tools/validate_coverage_port.py` runs the preserved Python GPU executor and the
native executor independently. It compares complete metadata, exact decoded
mask/after/overlay bytes, initialized output storage, per-sample MSAA storage,
UAV counters and SO history. Native baseline replays additionally check that
diagnostics preserve original counts and outputs when the requested mode does
not intentionally change original depth/stencil behavior.

- **560 synthetic cases / 4,900 checks** passed in
  `artifacts/coverage-executor-v2/validation.json`: hardware/WARP, four mode/depth
  combinations, sparse/full MRT, dual-source alpha-to-coverage, MSAA, null PS,
  1D/3D/buffer targets, targetless arrays and all four pre-raster UAV stages.
- **184 integration cases / 1,724 checks** passed in
  `artifacts/coverage-executor-integration-v1/validation.json`: SO append and
  DrawAuto, conditional rendering, dynamic linkage, scoped texture input edits,
  persistent output edits, disabled draws and explicit/rejected targets.
- **Five native test cases**, including setup/cleanup, passed in
  `artifacts/coverage-native-v2.txt`: target rejection before replay, retry after
  failure, image/report export, hardware/WARP predicates and disabled draws.
- **Eight real-capture cases / 80 checks** passed in
  `artifacts/coverage-executor-real-v1/validation.json`: GF2 draws 897/1508 and
  BF1 draws 15818/30112, each in fragment and geometry mode. Exact images and
  complete reports match. The probe executable hash matches final Release.
- Complete Release build and seven relevant CTest suites passed:
  coverage, DXBC coverage, texture inspector, frame output, predication,
  stream output and core. Logs: `artifacts/build-coverage-executor-full.log`
  and `artifacts/ctest-coverage-executor.log`.
- The new package passed **16 CLI/worker cases** under a Windows-only PATH,
  including rejected requests, all three exported PNGs and exact JSON transport
  (`artifacts/coverage-cli-final/validation.json`).
- All four golden/negative controls passed. GF2/BF1 retain their exact known
  replay image hashes (`artifacts/coverage-executor-golden/validation.json`).
- Ten loaded-module reports passed the runtime audit: package-local Qt, no
  Python/Tk/GPA/RenderDoc runtime dependency. All four product executable hashes
  match Release (`artifacts/coverage-runtime-audit.json`).

Backend-stage package: `out/FloraGPA-coverage/` (CLI/worker coverage only).
The subsequent Qt package is `out/FloraGPA-coverage-ui/`; its delivery evidence
is recorded in [COVERAGE_UI_MIGRATION.md](COVERAGE_UI_MIGRATION.md).

These results do not establish correctness for arbitrary captures or a separate
clean Windows installation. MSAA reports retain the original explicit statement
that pre-capture per-sample storage is not reconstructed. Buffer RTV coordinates
describe view elements and raster positions, not observed GPU write addresses.

At this backend stage the coverage module entries remained `partial` pending
Qt integration and the consumer/experiment audit, documented in the subsequent
Qt migration. Quad execution is not included in these claims. Python is used
only by development comparisons; neither product executable launches Python
for coverage.
