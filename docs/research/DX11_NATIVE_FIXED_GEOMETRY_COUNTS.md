# Native fixed geometry emission counts

## Scope and implementation

Implementation `fcab4d420725ba04be41f916ced3caf852107481` resolves instance
boundaries for a terminal GS whose emit/cut schedule is straight-line, even when
its output values read a written UAV or consume a live atomic return value.
Analysis contexts advance to `dx11-native/12`; the GPU helper remains
`dxbc-gpu/30`. This extends native Replay Mesh preview and private pixel geometry
partitions. It does not establish original fragment eligibility or switch the
default mesh backend.

The proof records actual DXBC emission tokens, opcodes, streams and the final
return boundary. Only known value/declaration instructions and fixed stream
operations qualify. Branches, loops, calls, early returns and unknown operations
retain the preceding dependency proof. All normal bytecode operand/resource
checks still apply. SM4.0, SM4.1 and SM5.0 profiles remain supported.

Every written resource must remain excluded from IA/constant-buffer inputs and
upstream resource reads. The terminal GS exception concerns counts, not values.
Selected vertex bytes are sliced from the complete private capture; prefix
queries supply only boundaries. Consumers validate the scope, pipeline placement
and schedule representation, including integer ranges before narrowing.

Microsoft documents GS invocation and emission behavior in the
[geometry shader stage](https://learn.microsoft.com/en-us/windows/win32/direct3d11/geometry-shader-stage)
and complete-primitive capture before rasterization in the
[stream-output stage](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-stream-stage).
Our inference is that a valid, completed straight-line GS with a fixed emission
schedule has value-independent output counts when upstream dependencies remain
excluded. The retained infinity probe checks the pre-rasterization boundary.
Repeated matching query results alone do not establish this proof.

## Capability and evidence matrix

| Scenario on WARP, NVIDIA and Intel | Previous result | Current result |
| --- | --- | --- |
| Fixed GS output with aliased UAV read | Full geometry available; instance partition unavailable | All three instance slices resolved |
| Fixed GS output with live `InterlockedOr` return | Full geometry available; instance partition unavailable | All three instance slices resolved |
| Fixed GS output with positive infinity position | Full geometry available; instance partition unavailable | Raw infinity retained; all instance slices resolved |
| UAV-dependent conditional return | Instance partition unavailable | Explicit alias rejection retained |
| VS reading a GS-written resource | Instance partition unavailable | Explicit upstream alias rejection retained |
| Worker/UI selection for read and atomic fixtures | Not covered by these new fixtures | Instance 1, table and CSV/OBJ/JSON exports verified |

The pre-change engine is `4e82fc25cbb04ec557116f539cde28d69d760ffa`.
Its retained focused run has eight passes and nine failures. Every failed case
already passed the full nine-vertex value checks and failed specifically because
the expected partition was unresolved. The original fixture, test source and
executable are retained. An initial test-compilation failure is also retained;
it is not counted as a product regression.

The owned probe seeds a buffer with 42. Atomic cases use `InterlockedOr` with an
already-set bit, keeping the live return deterministic, while every GS increments
a separate word. Three explicitly emitted vertices per invocation have x values
42, 43 and 44 and the original instance ID in y. The nonfinite fixture retains
positive infinity in x and 42 in z. No probe assumes nondeterministic atomic
ordering. Diagnostic calls preserve original resource bytes, hidden counters
and command counts; a subsequent draw matches clean replay.

The independent validator decodes the retained DXBC, verifies frame/shader hashes,
checks raw vertex words against this formula, reconstructs the emission schedule,
checks instance ranges and recomputes every selected slice hash. Its 15-case
matrix has nine resolved and six rejected cases and rejects 102 corruptions.
The preceding 42-case matrix still passes with 291 rejected corruptions.

| Release suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| GeometryUavDependency | 85 | 0 | 0 |
| NativeReplayMeshUi | 105 | 0 | 0 |
| NativeAnalysis | 61 | 0 | 0 |
| PostTransform | 13 | 0 | 0 |
| GeometryExport | 22 | 0 | 0 |
| Total | 286 | 0 | 0 |

GPU suites ran serially. CPU checks retain SM4/4.1 support and reject 63 inserted
control/unknown instructions across three profiles. The nine positive GPU cases
also exercise five malformed consumer reports each. All products and test targets
build in Release.

Independent UI validation accepts 24 jobs and three explicit upstream rejection
cases, rejecting 120 corruptions. All successful Worker module records exclude
RenderDoc and the GPA installation. Historical terminal-domain evidence still
passes its unchanged verifier mode: 42 geometry cases and 18 UI successes with
three rejection cases, with 291 and 90 corruption controls respectively. Old
reports and expected values were not rewritten.

For the six new UI jobs, elapsed Worker time is 38–258 ms and peak working set is
22,712,320–108,142,592 bytes. Each instance-1 job records one full-capture attempt
and two prefix-query attempts. Across 18 shared frame hashes, the historical
terminal-domain run has median 143.5 ms and this run 178 ms. These are separate
single runs of different implementation revisions, not a controlled performance
acceptance comparison or evidence of bounded memory over sustained operation.

## Reproduction

Build `FloraGPA`, `FloraGPA.Cli`, `FloraGPA.Worker` and the five corresponding
`Flora<Suite>Tests` targets in Release. Put Qt on `PATH`, select fresh output
directories and execute GPU tests serially. Read each Qt log after termination.

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FLORA_FIXED_GEOMETRY_EVIDENCE = '<fixed-cases>'
$env:FLORA_GEOMETRY_UAV_EVIDENCE = '<existing-cases>'
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = '<ui-cases>'
build/vs2022/Release/FloraGeometryUavDependencyTests.exe -o <geometry-log>,txt
build/vs2022/Release/FloraNativeReplayMeshUiTests.exe -o <ui-log>,txt
build/vs2022/Release/FloraNativeAnalysisTests.exe -o <analysis-log>,txt
build/vs2022/Release/FloraPostTransformTests.exe -o <post-transform-log>,txt
build/vs2022/Release/FloraGeometryExportTests.exe -o <export-log>,txt
python tools/validate_fixed_geometry_counts.py <fixed-cases> --log <geometry-log> --out <fixed-validation.json>
python tools/validate_geometry_uav_independence.py <existing-cases> --terminal-domain --fixed-geometry --log <geometry-log> --out <existing-validation.json>
python tools/validate_native_replay_mesh_ui.py <ui-cases> --fixed-geometry --log <ui-log> --out <ui-validation.json>
```

The [evidence ledger](dx11-native-fixed-geometry-counts-evidence.json) pins source
Git content, binary snapshots, baseline/final logs and retained raw evidence
through an artifact index. Local captures and executable snapshots remain outside
Git under `artifacts/dx11-native/fixed-geometry-counts/`.

## Open acceptance gates

Arbitrary data-dependent GS emission and upstream dependencies remain unresolved.
These synthetic probes do not establish complete real-capture, stream, deferred
context or source-primitive identity parity. Original fragment qualification,
complete Pixel History and VS/PS/CS debugger parity remain open.

The GPA debugger request at RVA `0x902b0` and host field `+0x3c0` still lack a
verified concrete engine construction/module path. This increment uses SDK
semantics and owned probes, not a newly recovered original GPA implementation.
The Rdc Worker and runtime/build/package dependencies remain present. This machine
has GPA and RenderDoc installed; native module evidence is not independent clean
host acceptance. P7/P8 and the final portable package remain unfinished.
