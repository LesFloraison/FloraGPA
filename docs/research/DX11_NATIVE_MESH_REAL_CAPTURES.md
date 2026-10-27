# Native Replay Mesh validation on retained game captures

## Scope and independent comparison

This report describes the initial expanded presentation. The subsequent compact
identity implementation and frozen-main export comparison are documented in
`DX11_NATIVE_MESH_INDEXED_PRESENTATION.md`. Raw Worker data remains unchanged;
current presentation files include an explicit compact-row mapping.

`FloraNativeMeshCaptureProbe` runs two isolated native analysis jobs for each
selection: `vs-inputs` and final pre-rasterization stream output. The first result
passes through `NativeReplayMesh`, the actual `ReplayMeshView` position table and
its CSV, OBJ and JSON export methods. Final VS output passes through the model and
exports, but is not presented as GS/DS output in the UI. The probe isolates its
settings in a temporary directory and requires a new evidence directory.

`tools/validate_native_mesh_captures.py` pins the three frame hashes, checks request
and context identities, verifies every Worker artifact and audits actual adapters.
Its independent triangle-list reconstruction consumes the VS input-reference
sequence, then assembles full vertex records and compares every byte with final
SO storage on the same adapter. It also checks position CSV values including
signed zero, the existing OBJ perspective-divide rule, face indices, UI export
retention and JSON presentation. Deliberately changed vertex bytes, reference
identities, topology and faces must fail the comparison.

This is a consistency comparison between two native execution paths. It does not
establish absolute shader correctness or RenderDoc parity: a defect shared by both
paths could escape it. The validator requires VS-only triangle-list geometry with
no reported pre-raster UAV effects. It does not silently skip GS, DS, feedback,
unsupported topology or failed jobs.

The matrix uses NVIDIA vendor 4318, Intel vendor 32902 and WARP vendor selection 0:

| Capture | GPA event | Selected instance | Purpose |
| --- | --- | --- | --- |
| GF2 | 113 | 0 | Indexed draw and multiple output attributes |
| GF2 | 181 | 0 | BaseVertex 4 and StartIndex 6 |
| BF1 | 876 | 0 | 19,290 indexed references and StartIndex 19,290 |
| BF1 | 1415 | 0 and 8 | First and last of nine instances |
| BF1 | 2010 | 0 | 15,960 references including zero-W output |
| Helldivers | 200 | 0 | Real minimum-precision shader output |

## Verified results

Implementation commit: `f9ea9c76d5d7fe2bfe45062b290e523ee5ed8279`.
All 21 case/adapter combinations pass, comprising 42 isolated Worker jobs and 21
VS view presentations. The comparison covers 114,570 vertex records, 38,190
triangles and 4,851,216 bytes across the three adapters, with exact same-adapter
agreement. All 84 deliberate comparison corruptions are rejected. The offline
revalidation report is byte-identical to the execution-time validation report.

All 63 end-of-process module snapshots (42 Workers and 21 presentation probes)
exclude RenderDoc and the checked GPA private modules/install location. This is
scoped runtime evidence, not an assertion about every product process or transient
module loads. The existing `native_replay_mesh_ui` suite additionally passes 29
checks with zero failures or skips; its CTest run takes 8.05 seconds overall.

The reports record 72 geometry stream-output draw attempts, including bounded
buffer-growth retries, and no instance-prefix queries. This does not count every
API call replayed while preparing an event. Each case uses two separate analysis
jobs; it does not reuse a resident replay or a result cache.

| Requested adapter | Two-job probe elapsed (s) | Individual Worker elapsed (s) | Maximum Worker working set (MiB) | Maximum probe working set (MiB) |
| --- | --- | --- | --- | --- |
| NVIDIA 4318 | 2.411–23.678 | 0.303–1.539 | 1716.6 | 114.2 |
| Intel 32902 | 2.239–26.798 | 0.220–1.615 | 2144.4 | 114.2 |
| WARP | 2.291–25.028 | 0.124–1.874 | 2532.9 | 114.1 |

Probe elapsed time includes request/frame digest preparation, both Worker jobs,
artifact validation, model construction and exports. It excludes the Python
runner's initial capture-hash checks and independent comparison. Worker elapsed
time covers its own analysis interval, not the parent process's request preparation.
Memory figures are individual process peaks, not summed simultaneous peaks or GPU
allocation measurements. These runs do not establish a performance improvement
or a non-regression against the old backend.

`dx11-native-mesh-real-captures-evidence.json` pins the source, tested binaries,
reports, raw artifacts, initial failures and final verification logs. Original
captures, executable snapshots and raw output remain outside Git.

## Retained preliminary finding

The initial matrix completed with 18 passes and three validator failures. The
initial validator incorrectly required the canonical geometry preview's faces
to equal the Replay Mesh OBJ faces in every case. BF1 event 2010 exposes the
difference: 4,572 of its 15,960 position records have W equal to zero, while all
position components are finite. `postTransformTables` intentionally disables its
projected preview in this case and retains raw attributes. The Replay Mesh
presentation retains its existing zero-W-as-one division behavior and exports
the original topology. These are separate representations with separate rules.

The corrected validator checks the preview's explicit unavailable reason and
empty projected geometry against raw position values. It still reconstructs and
checks every Replay Mesh face, raw vertex record and exported coordinate. No
product behavior or captured data is changed to make the comparison pass. The
original validator, probe and failed reports remain outside Git for inspection.
The first build with process-audit support also exposed Windows/Psapi include
ordering after formatting; the corrected include groups compile successfully.

## Reproduction

Build the explicit `FloraNativeMeshCaptureProbe` target in the configured Release
build. It is excluded from the default build and requires the GUI/testing build
configuration; it is not a product runtime dependency. Run:

```powershell
python tools/validate_native_mesh_captures.py `
  --probe build/vs2022/Release/FloraNativeMeshCaptureProbe.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --captures D:/CDXrepo/captures `
  --out artifacts/dx11-native/mesh-real-rerun
```

The runner executes GPU work serially and sets an outer 420-second deadline per
case; each Worker job has a 180-second deadline and the existing job-object cleanup.
Each case retains both job requests, results, raw artifacts, model exports and
probe logs. A failed case is recorded and does not become an accepted skip.
Add `--validate-only` to the same command to recheck existing evidence without
starting GPU work. This writes a new `revalidation.json`, requires the matching
tested probe/Worker and verifies the pinned capture files again.

## Remaining acceptance gates

These selected real draws extend P3 evidence. They do not close the complete
real-capture matrix, arbitrary GS/DS instance boundaries, nonzero SO streams,
deferred-context coverage, partial position signatures or experiment workflows.
No baseline timing comparison, clean standalone environment, default backend
cutover or full RenderDoc removal is claimed. P1 debugger-engine recovery,
P5/P6 public fragment history and PS debugging, and P7/P8 remain open.
