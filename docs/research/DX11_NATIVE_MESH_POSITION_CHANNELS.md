# Native mesh position channels

Implementation: `96375148116485f42a5bb5fd399a586d29fe22a6`.

The native Replay Mesh presentation previously rejected noncontiguous position
components after decoding and validating the retained geometry. It now preserves
packed storage order and records each component's semantic axis. A position with
X/Z/W data is displayed and exported as X/Z/W, without reinterpreting Z as Y or W
as Z. This removes a presentation restriction; it is not evidence that every
driver accepts arbitrary sparse shader signatures.

`ReplayMesh::componentChannels` defaults to X/Y/Z/W for existing consumers. The
table and CSV headers follow the declared channels, while values and binary
exports keep their original order. Presentation metadata contains
`position_components`, so exported JSON retains the mapping. Duplicate,
out-of-range and noninteger channel descriptions are rejected.

Preview and complete OBJ coordinates are looked up by semantic axis. Reordered
complete records, including Z/X/Y and W/Z/X/Y storage, therefore produce X/Y/Z
geometry with the existing perspective-divide convention. A sparse record missing
X, Y or Z has no three-dimensional preview or OBJ export. Its CSV, JSON and binary
data remain available. Attempting that OBJ export reports the missing-coordinate
limitation before opening or overwriting a destination file. A stale incoming
derived OBJ cannot substitute for an unavailable new result.

Existing contiguous-prefix exports, including short position records, remain
unchanged. Signed zero, nonfinite raw values and the existing zero-W convention
are preserved. This change does not alter Worker analysis, shader execution,
adapter selection, capture identities or the default backend. Analysis identity
remains `dx11-native/8`; presentation is rebuilt from validated retained artifacts.

## Evidence

The new owned storage/UI matrix has 34 cases: all 15 nonempty channel masks plus
two reordered complete layouts, each with ordinary and nonfinite data. Every
case checks packed bytes, component names, table cells, preview availability,
CSV/JSON/OBJ behavior, stale derived-file removal and protected existing exports.
The same layouts also check empty geometry and malformed channel rejection.

`tools/validate_native_mesh_channels.py` reconstructs the expected byte records
independently, verifies context/storage digests, parses exported CSV and OBJ,
checks semantic coordinates and rejects 204 corrupted observations. It explicitly
reports `scope=owned_storage_and_ui` and does not claim sparse GPU signature
qualification or full mesh parity.

The existing three-adapter GPU matrix still verifies 15 successful native cases
and three retained HS/DS/GS UAV dependency rejections. Its independent verifier
rejects 75 corrupted observations. The new `--position-channels` switch requires
all 34 added Qt rows and the complete existing matrix. Earlier verifier modes
retain their exact historical expected counts and assertions.

GUI, CLI, Worker and four test targets build in Release. Final serial regression:

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Native Replay Mesh UI | 69 | 0 | 0 |
| Legacy Replay Mesh UI | 3 | 0 | 3 |
| Native analysis | 46 | 0 | 0 |
| Replay Mesh model executable | Passed | 0 | 0 |

The Qt total is 118 passed, zero failed and three skipped. The legacy skips
require the original comparison-report collection and `shader_sources/source.gpa_frame`
for hardware/WARP recapture. Those fixtures were unavailable in this run; no
test was edited to skip, reduce its required collection size, or weaken its
assertions. This is not a full legacy workflow acceptance result. An initial
test build used an unavailable `QMap::at` member; the corrected final source uses
`value`, and the final build and runs pass.

An additional retained-baseline comparison runs 136 deterministic contiguous
model cases through both `build/dx11-native-baseline/Release/FloraReplayMeshTests.exe`
and the current executable. Results agree exactly, including exported strings,
counts and errors. Cases cover component counts, index widths, triangle topology,
base vertices, signed zero, nonfinite/random float bits and malformed inputs.
`tools/compare_replay_mesh_baseline.py` records both executable hashes. This is
model compatibility evidence, not a substitute for missing GUI workflow fixtures.

Final local evidence is under `artifacts/dx11-native/mesh-channels-release*`.
`dx11-native-mesh-position-channels-evidence.json` pins sources, binaries, logs,
raw records and verifier results. Owned raw evidence and executable snapshots
remain outside Git.

## Reproduction and open gates

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FLORA_NATIVE_MESH_CHANNEL_EVIDENCE = '<new-channel-root>'
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = '<new-gpu-root>'
build/vs2022/Release/FloraNativeReplayMeshUiTests.exe -o <new-qt-log>,txt
python tools/validate_native_mesh_channels.py <new-channel-root> --out <new-channel-ledger.json>
python tools/validate_native_replay_mesh_ui.py <new-gpu-root> --log <new-qt-log> --position-channels --out <new-gpu-ledger.json>
python tools/compare_replay_mesh_baseline.py --baseline build/dx11-native-baseline/Release/FloraReplayMeshTests.exe --current build/vs2022/Release/FloraReplayMeshTests.exe --out <new-baseline-comparison>
```

Actual sparse-signature GPU capture/replay is not qualified by the owned records.
Geometry instance dependencies, complete format/stream/deferred-context coverage,
real-capture workflow parity and missing legacy fixtures remain separate work.
This change does not switch the default mesh backend or close P3, P5/P6, P7 or
standalone P8 acceptance. Earlier shader-feedback hardware conflicts are also
unaffected.
