# Using FloraGPA

Native scope reviewed on 2026-11-03. See [closure and limitations](NATIVE_GPA_CLOSURE.md)
for limits and the [root README](../README.md#build) for building and packaging.
Linked migration/audit pages provide batch evidence; old pending statements
in those records are not the current feature inventory.

## Replay and inspection

Open a `.gpa_frame` / `.gpaframe` through **File > Open Capture…** or pass it to
the GUI. **Preflight** inspects known structural issues without executing the GPU.
Use **F5** for Replay, **F6** for Collect GPU Metrics, and **Escape** to cancel.
Select an API event to inspect output and pipeline. Choose hardware or WARP
according to the selected feature's device requirements.

CLI examples run from the repository root using a deployed package:

```powershell
$CaptureRoot = './captures'
$Frame = Join-Path $CaptureRoot 'sample.gpa_frame'
$Cli = './out/FloraGPA/FloraGPA.Cli.exe'
& $Cli --help
& $Cli validate-frame $Frame --out ./artifacts/preflight-new
& $Cli replay $Frame --out ./artifacts/replay-new
# Optional replay arguments: --warp, --experiment <project.json>
```

Use a new or empty output directory for each invocation. Examples with numeric
event/resource IDs require IDs from your capture; they are not universal.

### API Log and annotations

Search calls by terms including decode status. **Captured fields** and
**References** retain scalars, arrays, byte offsets, raw bits and resource IDs.
Double-click references to navigate; export filtered JSON/CSV through the context
menu. Partial/unknown records retain undecoded bytes. Decoding is not execution.

**Annotations** shows Begin/End/Marker groups, linked work and context evidence.
Open/interrupted groups and unresolved identities remain explicit. **Range Metrics**
transfers a closed group's inclusive endpoints to GPU Statistics.
See [annotations](ANNOTATION_MIGRATION.md).

```powershell
& $Cli commands $Frame --out ./artifacts/api-new
& $Cli command-state $Frame --event 430 --before --out ./artifacts/state-new
```

### Contexts and pipeline boundaries

**Analysis > Contexts & Command Lists** provides inventory, recovery evidence,
navigation and export. Pointer candidates and resource IDs remain distinct.
This does not enable captured ExecuteCommandList replay. Only specifically
checked metadata/no-op paths are accepted; unresolved deferred execution is rejected.

**Pipeline > Captured State** reconstructs original before/after state with source
events and uncertainty; experiments do not apply. **Snapshot** shows selected
draw/dispatch bindings including persistent setter edits. **Replay State** reads
actual D3D11 bindings through the worker, including supported experiments.
SO live write cursors have no native getter and remain unknown.

Ordinary before-draw output stops before preparing that draw's snapshot; input
and pipeline inspection can prepare bindings before submission. See
[boundary semantics](BEFORE_BOUNDARY_MIGRATION.md) and [output bindings](OUTPUT_BINDING_MIGRATION.md).

```powershell
& $Cli contexts $Frame --out ./artifacts/contexts-new
& $Cli command-lists $Frame --out ./artifacts/lists-new
& $Cli replay-pipeline $Frame --event 430 --before --out ./artifacts/pipeline-new
```

`initialization-graph` exports original initial dependency metadata without GPU,
GPA or Python. It accepts only `--out`; inspect node statuses, missing-ID issues
and `dependency_graph_complete` in `initialization-graph.json`. A completed export
does not establish an execution schedule, valid GPU objects or later-version
support. For a complete dependency graph, `modeled_initialization` predicts the
initial callback order assuming all initializers succeed. Its explicit assumptions
and `gpu_execution_verified: false` distinguish prediction from execution. Cycles
and excessive modeled work return an unavailable model with a reason. See
[initial graph boundaries](INITIALIZATION_GRAPH_AUDIT.md) and the
[scheduling model](INITIAL_SCHEDULE_MODEL_AUDIT.md).

```powershell
& $Cli initialization-graph $Frame --out ./artifacts/initial-graph-new
```

## Resources and geometry

Resources combines draw input/output thumbnails and texture inspection. The
shared viewer selects presentation/RTV/DSV, layers, supported resolve/sample
modes, channels and display ranges. Output pixels navigate to API/resource;
buffer RTV pixels open element bytes. See [draw resources](DRAW_RESOURCES_UI.md),
[frame output](FRAME_OUTPUT_MIGRATION.md) and [saved settings](OUTPUT_SESSION_MIGRATION.md).

Texture supports mip/layer/slice, typed formats, Y/UV planes and DDS/RAW/PNG
exports within format/device limits. Legacy P010/P016 initial data can expose
recovered Y only. Planar WRITE/READ_WRITE and DISCARD have different preservation
semantics; unsaved contents are not inferred. See
[texture inspection](TEXTURE_INSPECTION_MIGRATION.md) and [planar writes](PLANAR_WRITE_MIGRATION.md).

Buffer reads initial/before/after bytes as hex, ASCII or 32-bit words.
The Qt inspector requests raw bytes and a report without generating a CSV.
CLI `buffer` still exports `buffer.bin`, `words.csv` and `report.json` by default;
CSV text is written in bounded chunks. Use `--no-buffer-csv` to omit only
`words.csv` when reading large buffers.
The option applies to `buffer` only and leaves range/error checks unchanged.
**Constants** uses reflected types and CB1 windows; stripped variable names can
leave no fields. **UAV Counters** inspects supported hidden counts. Missing
frame-before bytes/counter values remain unknown unless explicitly supplied
as an experiment; those supplied values are not recovered capture data.

```powershell
& $Cli texture-storage $Frame --event 430 --id 20 --out ./artifacts/texture-new
& $Cli predicate $Frame --event 430 --id 60 --out ./artifacts/predicate-new
& $Cli class-linkage $Frame --id 62 --out ./artifacts/classes-new
```

`texture.bin` packs non-MSAA subresources in array-layer/mip order, including
volume slices. Predicates report this replay's query result, not original CPU
control flow; active/hint-only queries may have no readable result.
Class linkage exposes saved names, descriptors and owning linkage.

Geometry includes IA and supported native VS/DS/GS/final output, stream/instance
selection, typed attributes and wireframe views. Specialized views expose VS
identities and HS control-point/patch-constant output.
Exports retain validity/provenance, with OBJ when positions are valid.
See [post-transform](POST_TRANSFORM_MIGRATION.md), [VS identities](VS_IDENTITY_MIGRATION.md),
and [HS output](HULL_OUTPUT_MIGRATION.md). Instruction-level write/emission traces
are retired product entries; their internal instrumentation remains shared.

## Experiments and edits

Experiments are frame-bound JSON projects with undo/redo. Reports retain the
active revision and applied/pending operations. Supported editors include:

- **Compile & Apply**, event enable/disable, **Edit Clear Values** and
  **Replace Update Source** for recovered command families.
- Buffer **Edit Bytes**, **Import Patch**, reflected **Edit Value** and
  **Edit Counter**. Exact scalar bits can be entered as `bits:HEX`.
- Texture **Import Input… / Import Output…** for packed RAW subresources;
  MSAA imports require an explicit sample.
- **Edit Setter** for six-stage shaders/class instances, IA, CB/CB1, samplers,
  SRVs, output/SO and supported pipeline setters.
- Per-draw depth/stencil, rasterizer, viewport/scissor, blend/sample, sampler
  and SRV descriptor edits; **Edit View Resource** applies frame-wide view edits.

Event input clones are scoped to one command. Submitted output edits persist;
previews and disabled commands have restoration rules. Setter edits persist
until applicable later setters or ClearState, with family-specific slot rules.
Native D3D11 still validates device/format combinations. An available editor
does not prove every diagnostic consumer supports every edit combination.

See [shader setters](SHADER_SETTER_MIGRATION.md), [IA](IA_SETTER_MIGRATION.md),
[CB/CB1](CONSTANT_BUFFER_SETTER_MIGRATION.md), [pipeline setters](PIPELINE_SETTER_MIGRATION.md),
[texture lifetime](EVENT_TEXTURE_MIGRATION.md), [view edits](VIEW_MIGRATION.md)
and [experiment provenance](EXPERIMENT_REPORT_MIGRATION.md).

## Shader tools and diagnostics

Shader views expose available source, DXBC and reflection. **Recover HLSL**
reconstructs supported bytecode, explicitly labelled as recovery. **Shader Project**
supports virtual includes, macros and compiler settings with saved-source identity
verification. **Import ASM / Assemble & Apply** needs a selected external tool.
See [recovery](HLSL_RECOVERY_MIGRATION.md), [projects](SHADER_PROJECT_MIGRATION.md)
and [external tools](EXTERNAL_SHADER_MIGRATION.md).

Shader execution debugging and GS/HS/DS checkpoint controls have been retired.
Available source metadata remains part of shader inspection.

Coverage provides fragment/geometry diagnostics, overlays, navigation and exports.
Quad provides serialized diagnostic-group counters/previews. Coverage is not an
overdraw count; Quad results are not physical GPU quad invocations. See
[Coverage](COVERAGE_UI_MIGRATION.md) and [Quad](QUAD_UI_MIGRATION.md) for sample,
ordering and coordinate limits.

```powershell
& $Cli coverage $Frame --id 430 --out ./artifacts/coverage-new
& $Cli quad $Frame --id 430 --quad-depth prepared --out ./artifacts/quad-new
```

### Native DX11 analysis

Pixel History provides GPA event diagnostics: one row per event, post-event pixel
bytes, diagnostic status and count. Counts describe GPA diagnostic passes, not
individual writes or fragments. The supported scope is single-sample 2D color
render targets at layer zero; MSAA, volume and depth targets are rejected.
Replay Mesh and Replay Metrics use the isolated native Worker directly.

```powershell
& $Cli analyze $Frame history --history-scope gpa --resource 5 --x 4 --y 4 --out ./artifacts/history-new
& $Cli analyze --help
```

Choose the actual resource and coordinates from the loaded capture. No RenderDoc
runtime, recapture, external replay ABI or GPA installation is used. Old
`rdc-analyze`, `--renderdoc` and `shader-checkpoint` requests fail explicitly.
Storage snapshots remain an internal validation facility, not a product mode.
Large Intel Helldivers analyses can exceed the default 180-second Worker deadline;
CLI validation uses `--timeout 600`. A timed-out job is a failure until a fresh
explicit retry succeeds. See [native acceptance](NATIVE_GPA_CLOSURE.md) for evidence
and release limits.

## Measurements

**GPU Statistics** measures an event, inclusive range or frame through native
DX11 queries: pipeline activity, occlusion, SO and elapsed time. **GPU Timing**
adds repeats, warmup and distributions. Measurements describe this replay;
range timings include relevant bindings/uploads/helpers and are not golden hashes.
See [statistics](GPU_STATISTICS_MIGRATION.md) and [timing](GPU_PROFILE_MIGRATION.md).

```powershell
& $Cli timings $Frame --samples 5 --warmup 1 --out ./artifacts/timing-new
```

Intel Metrics requires supported Intel hardware/driver and the packaged
`FloraGPA.Metrics.dll` bridge. Catalog discovery, scheduled and uniform Qt
collection/export are available. Event-group CLI collection exists; its Qt/session
owner remains pending. WARP/non-Intel devices do not provide Intel MD counters.
On the validated Intel `8086:7d67` driver, collection succeeds but the `CsThreads`
numerical qualification fails; available records are not proof of correct values.
Native DX11 statistics remain available. See the [failure ledger](NATIVE_GPA_CLOSURE.md#historical-problems-and-follow-up).
See [foundation](METRICS_DISCOVERY_MIGRATION.md), [scheduled UI](MD_ITERATIONS_UI_MIGRATION.md),
[uniform UI](UNIFORM_METRICS_UI_MIGRATION.md) and [groups](MD_HOTSPOTS_MIGRATION.md).

Internal metric/diagnostic use of self-created native command lists does not
implement captured Deferred Context / Command List replay. Shader Profiler/GTPin
and additional analyzer integration remain incomplete.

## Development validation

Run GPU work serially and preserve each run in a fresh output directory:

```powershell
ctest --preset release --parallel 1
```

Some tests generate fixtures; others require external captures and environment
variables. GF2/BF1 checks use the CMake cache variable `FLORA_TEST_CAPTURE_DIR`.
Other fixture/evidence variables are documented by their batch/test sources.
Skipped external checks are not acceptance.

The optional [persistent recovery runner](PERSISTENT_QT_SOAK.md) exercises one
Qt window for a requested duration and saves immutable progress snapshots.
Its default 30-minute workload is explicit and is not part of ordinary CTest.

Python comparison tools are development oracles, not application backends.
The recovered reference workspace and original captures are not supplied.
If available, configure paths explicitly:

```powershell
$ReferenceRoot = 'C:/reference/FloraGPA' # External workspace with standalone/ and tools/.
$CaptureRoot = 'C:/captures'
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
$Cli = './out/FloraGPA/FloraGPA.Cli.exe'
python tools/validate_native.py --exe $Cli --captures $CaptureRoot --out ./artifacts/golden-new --isolated-env
python tools/validate_geometry.py --reference "$ReferenceRoot/standalone" --exe $Cli --captures $CaptureRoot --qt-bin "$QtRoot/bin" --out ./artifacts/geometry-new
python tools/validate_corpus.py --manifest docs/capture-corpus.json --captures-root $CaptureRoot --exe $Cli --out ./artifacts/corpus-new --repeat 2
```

The manifest requires its recorded relative fixture layout and hashes;
arbitrary captures fail identity checks. Original-kernel comparison additionally
uses `--oracle-tools "$ReferenceRoot/tools"` and the pinned installed GPA build
expected by that adapter. Python parity alone is not original-player equivalence.
See [acceptance infrastructure](COMPATIBILITY_BASELINE.md).

Historical provenance text and JSON retain original workstation paths. Set the
external roots in command examples for your environment. Helpers differ in whether
`--reference` expects the workspace or `standalone/`; consult each helper's
`--help`. Local `artifacts/`, `out/` and
`build/` records and hashes identify evidence not distributed with a clone.
Clean-machine deployment remains an open acceptance item.
