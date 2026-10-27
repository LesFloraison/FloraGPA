# Indexed native Replay Mesh presentation

## Compatibility defect and implementation

Commit `597e5b29e2ce14c638ca14cbbc33cdbc99a7c04a` fixes a concrete export mismatch:
GF2 event 113 previously displayed six native output references as six vertices,
where the frozen-main backend exported four vertices and six indices. Geometry
was visually equivalent, but CSV row identities and OBJ indices differed.

Native presentation now uses the validated input identity/variant tables. It
sorts vertices by instance, signed input vertex index and first-observed output
variant. Deduplication requires both the same input identity and identical full
output bytes. Equal positions alone never establish equality. Differences in
other attributes, signed zero or NaN payloads remain distinct. GS/DS output without
input identities retains its expanded representation.

The original Worker artifacts and expanded tables remain intact. The presentation
adds an owned `replay_mesh` schema-1 block with `vertex_sources` (compact row to
original output row), `reference_vertices` (original output row to compact row),
`vertex_stride`, `index_count` and the strategy. Its `post_vertices.bin` contains
the corresponding complete vertex records. The position table, CSV and OBJ use
the compact rows; topology references are remapped and cut markers remain cuts.
The UI reports vertex and reference counts separately. Sorting and remapping
include cancellation checks. No RenderDoc header, enum, interpreter or runtime
call is added to this implementation.

## Frozen-main comparison

The already retained NVIDIA recaptures from the counter study were read by the
separately built frozen-main CLI/Rdc executable. Their original capture reports,
the `.rdc` files, executable identities, analyzer library and output files are
pinned in the evidence ledger. The baseline reports identify local, non-degraded
NVIDIA replay; native comparisons explicitly request NVIDIA vendor 4318.

`tools/compare_native_mesh_baseline.py` reads saved files only. It does not load
RenderDoc, launch an analyzer, or require the external library. It checks original
GPA events, instances, actual reported vendor, strides, compact raw-record
provenance, index bounds, referenced records and exact binary/CSV/OBJ equality.

| Capture/event/instance | Baseline vertices | Native selected vertices | Exact binary, CSV and OBJ | Referenced records and indices |
| --- | --- | --- | --- | --- |
| GF2 / 113 / 0 | 4 | 4 | Yes | Exact |
| GF2 / 181 / 0 | 4 | 4 | Yes | Exact |
| BF1 / 876 / 0 | 3312 | 3312 | Yes | Exact |
| BF1 / 1415 / 0 | 2529 | 281 | **No** | Exact |
| BF1 / 1415 / 8 | 281 | 281 | Yes | Exact |
| BF1 / 2010 / 0 | 10348 | 10348 | Yes | Exact |
| Helldivers / 200 / 0 | 10008 | 834 | **No** | Exact |

Helldivers additionally confirms that sorting by input index matters: its first
references start with indices 39562, 39563 and 39564 before 39524. First-reference
ordering alone does not match the baseline compact buffer.

The two strict differences remain open. The baseline instance-zero buffer contains
the later instances too. This was checked independently, not inferred solely from
counts: additional native CLI jobs omitted `--instance`, retained all original
identities, and their sorted complete records exactly reproduced the baseline
2529-vertex BF1 buffer and 10008-vertex Helldivers buffer. Native selected-instance
exports contain only the selected 281 or 834 vertices. The baseline path requests
`GetBufferData(vertexByteOffset, vertexByteSize)`; its exported metadata does not
record `vertexByteSize`, so no specific size-member value is inferred here.

The saved-report comparator still exits **1** because strict export equality is
false. Matching referenced data and recovering all instances do not silently waive
the selected-instance export behavior difference. Baseline files and old goldens
were not rewritten. Default backend cutover remains pending.

## Verification and retained failures

- `native_replay_mesh_ui`: 35 passes, zero failures/skips, 7.65 seconds total CTest
  time. Six new CPU cases cover unsorted/repeated indices, equal positions with
  different identities, other-attribute variants, signed zero, distinct NaN
  payloads, instance separation and strip cuts.
- Its saved synthetic evidence contains 15 successful GPU cases and three existing
  explicit UAV-feedback gates. The independent validator rejects 75 corruptions.
- All 21 real selection/adapter combinations pass on NVIDIA, Intel and WARP:
  42 Worker jobs, 21 actual VS views, 84 rejected corruptions. The validator
  independently derives compact identities from raw bytes and checks every compact
  record, CSV value, OBJ coordinate/index and presentation mapping. The assembled
  input-to-final comparison still covers all 114570 original vertex references.
  Offline revalidation produces a byte-identical report.
- Two additional NVIDIA all-instance jobs match the complete baseline buffers.
  The 44 native Workers and 21 presentation probes have 65 completion-time module
  snapshots without RenderDoc or the checked GPA private modules. The seven
  separate baseline analysis jobs intentionally use RenderDoc only as the
  migration reference.

The first new CPU fixture omitted `identities.json`, which the native reader
correctly requires. The initial unhandled exception and subsequent six diagnostic
failures are retained. The fixture now supplies the same explicit identity artifact
as a Worker result; no product validation was weakened to accept missing identity.

## Reproduction

Build `FloraGPA`, `FloraNativeReplayMeshUiTests` and `FloraNativeMeshCaptureProbe` in
Release. Run the GPU work serially using fresh evidence directories:

```powershell
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = "$PWD/artifacts/dx11-native/indexed-ui-rerun"
ctest --test-dir build/vs2022 -C Release --parallel 1 --output-on-failure `
  -R '^native_replay_mesh_ui$'
python tools/validate_native_replay_mesh_ui.py $env:FLORA_NATIVE_MESH_ARTIFACT_DIR `
  --log build/vs2022/native-replay-mesh-ui-Release.txt --indexed-presentation `
  --out artifacts/dx11-native/indexed-ui-rerun-validation.json
python tools/validate_native_mesh_captures.py `
  --probe build/vs2022/Release/FloraNativeMeshCaptureProbe.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --captures D:/CDXrepo/captures `
  --out artifacts/dx11-native/indexed-real-rerun
```

For the saved baseline comparison, pass its directory with `--baseline`, the
native matrix directory with `--native`, and a new JSON path with `--out` to
`tools/compare_native_mesh_baseline.py`. The optional `--all-instances` directory
contains native `vs-inputs` jobs for BF1 event 1415 and Helldivers event 200, named
`bf1-1415-0` and `helldivers-200-0`, with NVIDIA selected and `--instance` omitted.
The names identify the baseline selection being investigated, not a native
instance filter. The current strict comparison is expected to report the two
differences and exit 1; this is a failed parity gate, not a required runtime test
that launches RenderDoc.

`dx11-native-mesh-indexed-presentation-evidence.json` records source/binary hashes,
raw reports, initial failures and final checks. P3's remaining geometry and export
gates, full pixel history/debugging, P1 engine recovery and P7/P8 remain open.
