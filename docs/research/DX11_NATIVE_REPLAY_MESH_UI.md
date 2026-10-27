# Native Replay Mesh preview

## Scope and decision

Implementation commit `90c9318b0c9ed0ce7f2ab68cbf942961bf9166c4` adds an explicit
**Native preview** mode to Replay Mesh. It sends the GPA frame, experiment and
selected adapter through `NativeAnalysisController` and `FloraGPA.Worker`.
The default remains the existing backend until the P3 parity gates close.
Native failures never switch backends automatically.

VS selection uses `vs-inputs`; GS/DS selection uses `final`, stream zero, and
rejects a final-stage result that only contains VS output. Event and instance
identities remain GPA identities. Frame, experiment, backend and adapter context
validation precedes presentation; changed selections reject stale results.
Cancellation and retry use the existing isolated Worker lifecycle.

The controller validates retained artifacts and reconstructs presentation off the
UI thread. Native decoding verifies vertex hashes, identities, canonical tables
and generated artifacts. The view owns retained bytes after the temporary Worker
directory disappears. The existing position table, CSV columns and OBJ perspective
divide are preserved; native JSON exports additionally include the full native
presentation tables. The reusable `ReplayMesh` position container and formatters
are used without calling its RenderDoc decoder or translating through its ABI.
Lines and points are retained in the preview and OBJ export. Incomplete vertex
tails remain available in native tables. A preview requires at least three finite
position components; raw values and exports remain available otherwise.

## Validation

The serial Release runs used WARP, NVIDIA vendor 4318 and Intel vendor 32902.
Exact device, driver and LUID values are in the retained result contexts. This
increment adds synthetic fixture evidence; it does not claim a new real-capture
comparison or complete mesh parity.

| Check | Result | Evidence limits |
| --- | --- | --- |
| `native_replay_mesh_ui` | 23 passed, no failures or skips | Eight CPU export rows, nine successful GPU rows, three explicit open-gate rows, one MainWindow flow, init/cleanup |
| `native_analysis` | 40 passed, no failures or skips | Existing native Worker analysis regression |
| `native_execution_debug_ui` | 13 passed, no failures or skips | Existing native execution UI regression |
| `native_counter_ui` | 7 passed, no failures or skips after correcting a stale expectation | Supported VS success plus reserved PS error propagation |
| `replay_mesh_model` | Passed | Existing standalone model assertions |
| `replay_mesh_ui` | 3 passed, 3 existing skips | Optional recorded reports and source capture were unavailable; test source is unchanged |
| Retained evidence validator | 9 successful cases, 3 open gates, 45 rejected corruptions | Independent artifact, context, adapter, module, CSV and OBJ checks |

CPU cases cover one through four contiguous float components at a nonzero byte
offset, signed zero, infinity, zero W and perspective divide. GPU cases cover VS,
GS and pure HS/DS output on each adapter. The MainWindow flow verifies read,
cancel and retry. Tests also reject stale requests, changed context, corrupted
artifacts and cancellation during decoding, and verify exports after the original
temporary vertex file has been replaced.

Each successful Worker result records loaded modules. None contains RenderDoc or
an Intel GPA installation module. This is a scoped runtime observation, not a
standalone build/package certification: the application still contains legacy
consumers and dependencies.

The final synthetic Worker observations were 34–40 ms and 20.85–21.30 MiB peak
working set on WARP, 221–243 ms and 96.75–97.67 MiB on NVIDIA, and 100–145 ms and
48.93–50.55 MiB on Intel. These are single-run Worker measurements; they exclude
GUI reconstruction and are not a baseline performance comparison or a replay-count
audit. The complete new UI suite took 5.39 seconds in its final run.

The first mesh run incorrectly treated the original `HullCapture` as a pure HS
fixture. Its shader increments and writes a UAV, and the existing per-instance
gate correctly rejected it. The diagnostic rerun recorded all three adapter
failures. The final matrix retains that fixture as a negative gate case and adds
a separate pure HS fixture, using the established native analysis test shader,
for successful DS selection. No existing golden, assertion or skip was weakened.
The original failed logs remain in the evidence ledger.

The initial counter regression still expected VS debugging to be unimplemented,
although preceding commits implemented it. The corrected test checks a successful
VS debug session and its modules, then checks the same strict error and Worker-log
assertions using the still-reserved PS action. The successful rerun is recorded
separately from that failure.

Selected GF2, BF1 and Helldivers VS results now also exercise the actual native
view and exports. See `DX11_NATIVE_MESH_REAL_CAPTURES.md` for the separate
same-adapter assembly comparison, zero-W finding and remaining parity gates.

## Reproduction

These commands describe the initial preview commit. For the current expanded
matrix, use `DX11_NATIVE_GEOMETRY_RESOURCE_READS.md`; the original validator mode
remains available for the retained initial evidence.

The current compact presentation adds six CPU cases. Use
`DX11_NATIVE_MESH_INDEXED_PRESENTATION.md` and `--indexed-presentation` when
validating a fresh current-source UI run.

Use the configured Visual Studio Release build with Qt available. Build
`FloraGPA`, `FloraNativeReplayMeshUiTests`, `FloraReplayMeshUiTests`,
`FloraNativeAnalysisTests`, `FloraNativeCounterUiTests` and
`FloraNativeExecutionDebugUiTests`. Run GPU suites serially:

```powershell
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = "$PWD/artifacts/dx11-native/mesh-ui-rerun"
ctest --test-dir build/vs2022 -C Release --parallel 1 --output-on-failure `
  -R '^(native_replay_mesh_ui|replay_mesh_model|replay_mesh_ui|native_analysis|native_counter_ui|native_execution_debug_ui)$'
python tools/validate_native_replay_mesh_ui.py $env:FLORA_NATIVE_MESH_ARTIFACT_DIR `
  --log build/vs2022/native-replay-mesh-ui-Release.txt `
  --out artifacts/dx11-native/mesh-ui-rerun-validation.json
```

The validator requires a fresh output path and exactly the expected matrix. It
recomputes context and artifact hashes, decodes raw vertex positions independently,
compares CSV/OBJ values and topology, checks explicit adapter choice and modules,
and rejects five deliberate corruptions per successful case. Retained negative
cases must include the specific instance-boundary diagnostic and no fallback.
Raw evidence stays outside Git. `dx11-native-replay-mesh-ui-evidence.json` pins
sources, binaries, logs and the retained validation report.

## Remaining gates

At the initial preview commit, instanced HS/DS/GS with UAV access lacked verified
per-instance boundaries. Subsequent independent-write support is documented in
`DX11_NATIVE_GEOMETRY_UAV_INDEPENDENCE.md`; the original negative fixture is now
retained as an explicit success case, with actual UAV reads retaining rejection.
The initial three negative rows were not three supported cases. Noncontiguous position
components are explicitly rejected by this presentation path. Full real-capture,
format, stream, deferred-context and workflow parity remains to be established.
The artifact byte budget does not bound JSON/model overhead or total peak memory.

This increment does not close P3, replace public Pixel History/PS debugging, remove
the Rdc Worker or certify P7/P8. Default cutover remains blocked until successful
baseline capabilities are preserved.
