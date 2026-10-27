# Immutable resource reads in native geometry instance proofs

## Implemented behavior

Commit `1856911f81d0deddc5e25cc0212ac8772df95115` extends native per-instance
mesh selection to geometry shaders that read SRVs or UAVs while writing other
resources. A read is permitted only after replay excludes its actual resource
owner from every UAV owner written by the executed geometry stages. Prefix draws
still determine boundaries only; selected vertex bytes still come from the full
capture. The Worker and Replay Mesh native preview use the same path.

This replaces the earlier blanket SRV/read rejection with a conditional proof:

1. DXBC analysis records SRV declaration slots, UAV read operands and UAV write
   instructions for every executed stage. SRV declarations are conservative:
   unused declared slots may still prevent a proof. Live atomic return values,
   unresolved liveness, dynamic linkage and unfamiliar operands remain rejected.
2. Known instruction extensions are parsed as immediate metadata. Supported
   fields describe offsets, dimensions/strides and return types; duplicate types,
   unknown types and reserved bits fail. Resource operands must retain literal,
   bounded slots. Operand payloads are not scanned as if they were instructions.
3. Replay unions the actual write slots across stages, resolves their view owners,
   and compares those owners with IA/CB inputs and all reported SRV/UAV reads.
   Two view IDs are not evidence of two resources. Unbound read slots have no
   owner and retain the API's unbound behavior.
4. The report includes `write_slots`, `resource_reads`, each read's stage/kind/slot
   and GPA view ID, and the completed alias-exclusion decision. Shader proofs with
   reads use scope `immutable_resource_inputs`; cases without reads retain the
   earlier scope. A static `proved` value alone is explicitly conditional on the
   runtime ownership check.

Read-only resources are not counted as written merely because their views are
bound as UAVs. Conversely, IA/CB inputs that alias an actually written UAV remain
rejected. The IA alias fixture now includes a real VS write to its extra UAV,
so that it continues to test the relevant hazard rather than an unused binding.
Diagnostic errors distinguish a static proof failure, an IA/CB alias and a
resource-read alias while retaining the existing migration message.

## Research basis

The stage-count investigation considered a terminal DS exemption. Microsoft
describes DS invocations in terms of tessellator output and places stream output
before rasterization. That alone does not exclude feedback into earlier stages.
No blanket terminal-stage exemption was implemented. The change instead proves
resource immutability across all executed stages. See the
[D3D11.3 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 2.4–2.7 and 14.

Extension bit fields come from the already pinned
[Microsoft token format](https://github.com/microsoft/DirectX-Headers/blob/461ef5138bd30922b44fbad77c4e47662812d2d5/include/directx/D3D12TokenizedProgramFormat.hpp),
SHA-256 `5862ef7c8b819f0d02ba73563a9df4dc0e89f11fe59d8f598def39122273c626`.
The relevant definitions are the three extended-opcode kinds and their immediate
field masks. No external header, interpreter or downloader becomes a build/runtime
dependency. See `../DX11_DXBC_FORMAT_RESEARCH.md` for the pinned-source workflow.

## Validation

GPU tests ran serially on WARP, NVIDIA vendor 4318 and Intel vendor 32902.
The final run completed in 42.03 seconds:

| Suite | Passes | Time | Added coverage |
| --- | --- | --- | --- |
| `geometry_uav_dependencies` | 53 | 4.63 s | Read dependency framing, seven compiler formats, 27 GPU cases |
| `native_analysis` | 46 | 16.82 s | Three GS/SRV Worker result and artifact cases |
| `native_replay_mesh_ui` | 29 | 7.63 s | Three GS/SRV native preview/export cases |
| `post_transform` | 13 | 12.92 s | Existing capture, isolation and UI regression |

All 141 checks passed with zero failures and zero skips. The compiler cases cover
structured, raw and typed buffers through both SRV/UAV operands and explicit-LOD
texture sampling. Each case also rejects an unknown extension kind and a reserved
extension bit: fourteen bytecode corruptions in addition to the liveness tests.

Per adapter, the GPU matrix retains the prior hull, geometry, read-feedback and
IA-alias fixtures and adds independent SRV reads, independent UAV reads, unbound
UAV reads, same-UAV read/write feedback and SRV/write-owner alias rejection.
Independent reads drive one, two and three output triangles, so the test checks
varying instance boundaries rather than only equal counts. Unbound reads produce
one triangle per instance. Selected bytes must equal exact full-capture slices.
All diagnostic paths preserve original UAV storage, extra read/input storage,
hidden counters and command counts; the next original draw must match a clean
replay. Rejection tests require the specific runtime alias diagnostic.

The dependency evidence validator accepts 27 cases and rejects 111 deliberate
corruptions. The mesh validator accepts 15 successful cases, records three
remaining open-gate cases and rejects 75 corruptions. It independently checks raw
vertex/CSV/OBJ agreement, request/context identities, adapter choice, proof fields
and module lists. No successful mesh Worker result loaded RenderDoc or an Intel
GPA installation module. These observations do not certify the complete product.

## Retained preliminary findings

The first run failed ten checks because compiler-emitted resource loads include
extended instruction metadata. The extension parser and corruption probes address
that format gap; the original 36-pass/10-failure log remains available.

The first read-feedback fixture bound two overlapping UAV views of the same
buffer. Although its recovery assertions matched a clean run, all three adapters
left the original storage unchanged. The independent storage validator rejected
the expected increment. The final fixture performs legal read/write feedback
through one UAV, avoiding that ambiguous binding setup. It is still explicitly
rejected for instance selection, and a subsequent original draw has the independently
expected storage `[3,0,0,0]`. Earlier reports and logs remain outside Git as evidence
of the correction; they are not counted as final acceptance.

## Reproduction and remaining work

Build `FloraGeometryUavDependencyTests`, `FloraNativeAnalysisTests`,
`FloraNativeReplayMeshUiTests`, `FloraPostTransformTests` and `FloraGPA` in Release.
Use fresh output paths:

```powershell
$env:FLORA_GEOMETRY_UAV_EVIDENCE = "$PWD/artifacts/dx11-native/resource-read-rerun"
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = "$PWD/artifacts/dx11-native/resource-mesh-rerun"
ctest --test-dir build/vs2022 -C Release --parallel 1 --output-on-failure `
  -R '^(geometry_uav_dependencies|native_analysis|native_replay_mesh_ui|post_transform)$'
python tools/validate_geometry_uav_independence.py $env:FLORA_GEOMETRY_UAV_EVIDENCE `
  --log build/vs2022/geometry-uav-dependencies-Release.txt --resource-reads `
  --out artifacts/dx11-native/resource-read-rerun-validation.json
python tools/validate_native_replay_mesh_ui.py $env:FLORA_NATIVE_MESH_ARTIFACT_DIR `
  --log build/vs2022/native-replay-mesh-ui-Release.txt --resource-reads `
  --out artifacts/dx11-native/resource-mesh-rerun-validation.json
```

The validators retain their earlier matrix modes for historical evidence. The
new `dx11-native-geometry-resource-reads-evidence.json` pins sources, executables,
raw reports, logs and validator results. Worker hashes in every retained mesh
context must match the tested executable. These are synthetic and integration
checks, not new GF2/BF1/Helldivers comparisons or a baseline performance audit.

Read/write feedback, observable atomic results, unsupported control flow/linkage,
resource subrange distinctions and the remaining real-capture matrix still block
complete instance parity. Default Replay Mesh cutover, full Pixel History/public
PS debugging, GPA debugger-engine closure and P7/P8 acceptance remain open.
