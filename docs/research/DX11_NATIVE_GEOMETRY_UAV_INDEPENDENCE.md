# UAV-independent native mesh instance boundaries

## Result

Commit `671dda1e8263843fd46f2b0dd885a5c3e673524d` permits per-instance native
HS/DS/GS mesh selection when a sufficient bytecode proof establishes that UAV
operations cannot affect geometry, and actual buffer ownership excludes writable
aliases in IA and constant-buffer inputs. The existing full-draw capture supplies
the selected bytes. Separate private prefix draws determine only their boundaries.
No application signature component or free UAV slot is required by this proof.

The original `HullCapture` now succeeds: its hidden-counter increment has an
unused return value, and its store does not feed geometry. A variable-output GS
fixture also succeeds, emitting one, two and three triangles for its three
instances while atomically incrementing a separate UAV word. The proof does not
authorize a default Replay Mesh backend switch or close the complete P3 gate.

## Sufficient proof and limits

`geometryUavIndependence` analyzes the effective shader bytecode used by replay.
It records the shader SHA-256, relevant instruction token offsets, opcodes, UAV
slots, result classification and bounded liveness work. Every executed geometry
stage must pass when any of those stages declares a UAV.

- UAV stores and atomics without observable results are permitted. A load or an
  atomic result that remains potentially observable rejects the proof.
- D3DCompiler can allocate a temporary even for an unused `IncrementCounter()`
  result. The analysis therefore tracks its component mask through a checked
  straight-line suffix. Reads are considered before writes, including nested
  relative indices. Partial overwrites kill only the written components.
- A result can die at an overwrite or the final return. Unresolved branches,
  loops, phase boundaries or calls in that suffix reject the proof. This is not
  a general interprocedural dataflow solver. Liveness scans share a budget of
  1,048,576 instructions; exhaustion leaves the shader unproved.
- SRV declarations, dynamic linkage, unsupported instruction extensions and
  unfamiliar resource semantics remain conservative failures. Literal payloads
  are not mistaken for operand tokens.
- Before using the proof, replay compares actual UAV resource owners with the
  selected state's index buffers, vertex buffers and constant buffers. Aliases
  reject instance selection, even when a bound input is unused by the shader.
  This uses COM resources rather than assuming that capture IDs identify current
  experiment storage.

Proofs are serialized under `instance_selection.uav_independence`. Pure-shader
prefix selection retains its existing behavior. Full-draw, single-instance and
empty-output cases retain their existing paths. Independent-write selection
continues to use private resource/counter copies and never substitutes prefix
draw bytes for the original full-draw capture.

UAV reads that happen not to affect counts are still rejected. Live atomic
results, aliasing, SRV reads, complex liveness and further real-capture cases need
additional proof or a different capture mechanism. Observing the same count in
two runs is not accepted as proof of independence.

## Tests and retained corrections

All GPU tests ran serially on WARP, NVIDIA vendor 4318 and Intel vendor 32902.

| Suite | Result | Scope |
| --- | --- | --- |
| `geometry_uav_dependencies` | 31 passed | Compiler probes, ten liveness mutations and twelve GPU rows |
| `native_analysis` | 43 passed | Worker and CLI regressions, including three new independent-UAV mesh rows |
| `native_replay_mesh_ui` | 26 passed | Three new original-HullCapture success rows and retained rejection rows |
| `post_transform` | 13 passed | Existing capture, isolation, SO cursor and UI regression |

The four suites completed in 38.16 seconds with 113 passes, no failures and no
skips. The final dependency suite took 2.11 seconds; native analysis took 16.32
seconds, mesh UI 6.23 seconds and PostTransform 13.48 seconds. These are scoped
regression timings, not a full baseline performance acceptance.

For each adapter, the GPU dependency suite checks both HS/DS instances and all
three GS instances. Selected bytes must exactly equal the corresponding ranges
of the full capture. Each diagnostic must preserve the original UAV bytes,
hidden counter and replay command counts. A subsequent original native draw must
match a fresh replay. Expected post-draw storage is independently checked as
`[42,43,52,53]` with counter 4 for the hull fixture and `[3,0,0,0]` with counter 0
for the GS fixture. UAV-read and legal IA/UAV alias fixtures must still reject
per-instance selection and recover identically.

The previous `ds-uav` rejection rows now use a DS that actually reads the UAV.
The original fixture was retained as new `ds-uav-independent` success rows rather
than dropped. No golden, skip or failure assertion was relaxed. The mesh validator
has an explicit `--uav-independent` option for the expanded matrix; its original
mode remains available for the earlier retained evidence.

Two preliminary failures are retained. The initial null-destination-only proof
failed the compiler's dead temporary output (7 passes, 2 failures), motivating
the liveness proof. The first alias fixture bound a structured buffer lacking the
required vertex-buffer binding; WARP reported device removal (27 passes, 1 failure).
The corrected fixture uses a separate R32_UINT buffer created with both vertex
and UAV binding flags. It exercises the same alias rejection on all three devices
without suppressing a failure or skipping WARP.

## Reproduction and evidence

This section records the matrix at the implementation commit above. Current
sources additionally support immutable resource reads; use the expanded commands
in `DX11_NATIVE_GEOMETRY_RESOURCE_READS.md` for a current-source rerun. The commands
below remain applicable to this commit and its retained evidence.

Build `FloraGeometryUavDependencyTests`, `FloraNativeAnalysisTests`,
`FloraNativeReplayMeshUiTests`, `FloraPostTransformTests` and `FloraGPA` in Release.
Use fresh evidence/output paths:

```powershell
$env:FLORA_GEOMETRY_UAV_EVIDENCE = "$PWD/artifacts/dx11-native/uav-instance-rerun"
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = "$PWD/artifacts/dx11-native/uav-mesh-rerun"
ctest --test-dir build/vs2022 -C Release --parallel 1 --output-on-failure `
  -R '^(geometry_uav_dependencies|native_analysis|native_replay_mesh_ui|post_transform)$'
python tools/validate_geometry_uav_independence.py $env:FLORA_GEOMETRY_UAV_EVIDENCE `
  --log build/vs2022/geometry-uav-dependencies-Release.txt `
  --out artifacts/dx11-native/uav-instance-rerun-validation.json
python tools/validate_native_replay_mesh_ui.py $env:FLORA_NATIVE_MESH_ARTIFACT_DIR `
  --log build/vs2022/native-replay-mesh-ui-Release.txt --uav-independent `
  --out artifacts/dx11-native/uav-mesh-rerun-validation.json
```

The first validator checks twelve retained cases and rejects 48 corruptions.
Its storage expectations are independently computed from fixture values; the
actual resource restoration and clean-draw assertions execute in the C++ suite.
The mesh validator checks twelve successful cases, three open-gate cases and 60
corruptions, including raw vertex/CSV/OBJ agreement and runtime module lists.
No successful mesh Worker result loaded RenderDoc or an Intel GPA installation
module. This observation does not certify the still-dependent product/package.

`dx11-native-geometry-uav-independence-evidence.json` pins the tested sources,
binaries, successful and preliminary logs, validators and retained reports.
Raw captures and reports remain outside Git. Full UAV instance support, complete
Pixel History, public PS debugging, the remaining GPA debugger research and
standalone P7/P8 acceptance remain open.
