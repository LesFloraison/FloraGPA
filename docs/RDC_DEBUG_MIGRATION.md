# Native RenderDoc shader debugging

`FloraGPA.Rdc.exe --job <job.json>` now implements the reference worker's
`debug-pixel`, `debug-vertex` and `debug-thread` branches through the selected
RenderDoc **1.45 release** DLL. This is the backend portion of the migration.
The existing Qt GS/HS/DS checkpoint views are unaffected; connecting these new
PS/VS/CS traces to Qt is still required.

## Job and output

All actions accept `capture`, `renderdoc`, a new/empty `out` directory, and either
`gpa_event` or `eid`. Original GPA IDs are resolved through the same exact
provenance index as Pixel History. An omitted event selects the last recorded
draw/dispatch. Pixel and vertex debugging require a draw; thread debugging
requires a dispatch. Disabled, ambiguous and unrepresented selections fail.

- `debug-pixel`: `x`, `y`, `sample` (defaults zero).
- `debug-vertex`: `vertex`, `instance` (defaults zero), optional explicit `index`.
  Automatic index selection includes the bound index-buffer byte offset, draw
  index offset, index width and signed base vertex; non-indexed draws include
  the start vertex. Invalid/short index reads and uint32 overflow are rejected.
- `debug-thread`: three-element `group` and `thread` arrays (defaults zero).

The result includes complete `trace`, `steps`, `step_count`, `source_debug`,
`disassembly` and GPA provenance maps. Vertex jobs also return
`actual_vertex_index`. `debug_shader.asm` is written atomically. The worker
preserves input/constant/resource/sampler trees, instruction/source mappings,
callstacks, flags, and before/after changes for forward and reverse stepping.
Each shader value retains all sixteen lanes in all eleven public numeric views:
half/float/double and signed/unsigned 8/16/32/64-bit integers. NaN and infinity
use the original worker's string representation; 64-bit integers remain JSON
integers. The original 250,000-step limit is retained, with trace cleanup on
success and exceptions.

## Upstream offset validity

During independent process comparisons, an otherwise identical PS trace exposed
different global `SourceVariableMapping.offset` values (464 versus 705). Inspection
of the local upstream 1.45 source confirmed that the DXBC signature/coverage
constructors and whole-constant-block wrappers leave that member uninitialized.
This is not a source structure offset and must not be displayed as one.

The native serializer exports **null** for those known unavailable global
offsets, and `debug_scope.unavailable_global_source_offsets` lists their indices.
It does not read the uninitialized bytes. Defined constant-member/resource and
per-instruction offsets are preserved. The comparison tool applies this exact
policy only to the original worker's matching global fields; it retains the
unmodified oracle reports on disk. All remaining trace fields are compared.

Source evidence is `renderdoc/driver/shaders/dxbc/dxbc_debug.cpp` in the local
upstream snapshot: `CreateShaderDebugState` signature/coverage construction and
`FillCBufferVariables` whole-block wrappers, compared with explicit offset
assignment in `FlattenSingleVariable` and resource/sampler mapping construction.
The public header's `SourceVariableMapping` also has no default offset initializer.
The snapshot's provenance limits are recorded in
[`REPLAY_HEADERS.md`](../third_party/renderdoc/REPLAY_HEADERS.md).

## Validation

`tools/validate_rdc_debug.py` compares real native traces against the original
Python worker, including all metadata, step changes, source text and disassembly.
Its fixtures cover Hardware/WARP PS/VS/CS with embedded source, multi-file PS
source and loops, original GF2/BF1 pixel/indexed vertex/compute traces, explicit
indices, 16/32-bit index buffers with offset and negative base vertex, nonzero
compute lanes, missing events, stage mismatches and absent pixel invocations.
Python/qrenderdoc are development oracles only. The product does not invoke them.

`RdcDebugTests` covers large integers, all numeric views, non-finite values, half
subnormals and signed zero, nested variables, variable birth/death, callstacks,
source mappings and unavailable-offset handling. `RdcWorkerTests` checks malformed
and conflicting selectors before loading a library and preserves existing output.
The final package `out/FloraGPA-debug-backend` passed **171 checks**, including
**21 complete trace comparisons / 279 steps** and **seven rejected selections**.
There were 37 global offset fields explicitly marked unavailable across those
traces; all other trace, source and disassembly fields compared equal. The two
36-step loop traces produced the independently expected `(51.5, 0, 0, 1)` output.
The known BF1 indexed VS/CS traces retained 59/53 steps, and GF2 PS retained three.

The complete Release build passed, as did four relevant CTest suites
(`rdc_debug`, `rdc_worker`, `rdc_events`, `history_ui`). Packaged GF2/BF1 golden
frames and suppressed-draw negative controls passed all four checks. Auditing
**44 native/recapture/golden runtime reports** found no Python/Tk/GPA modules;
Qt loaded from the package, and RenderDoc loaded only from the explicitly
selected installed DLL. All four delivered executable hashes match Release.
RenderDoc, Python and test binaries are not distributed in the package.

Local evidence: `artifacts/rdc-debug-package/validation.json`,
`artifacts/build-rdc-debug-release.log`, `artifacts/ctest-rdc-debug-release.log`,
`artifacts/rdc-debug-golden/validation.json` and
`artifacts/rdc-debug-runtime-audit.json`. Each native/oracle report and shader
disassembly is retained under `artifacts/rdc-debug-package/`.
Explicit Qt Test text logs additionally confirm **6 serializer**, **16 worker**
and **6 history UI** cases passed with zero skips:
`artifacts/rdc-debug-model-final.txt`, `artifacts/rdc-debug-worker-final.txt` and
`artifacts/rdc-debug-history-final.txt`. These logs supplement CTest's process
results because this host did not forward the Qt test stdout into CTest output.

```powershell
python tools/validate_rdc_debug.py --reference D:/CDXrepo/FloraGPA --exe out/FloraGPA-debug-backend/FloraGPA.Rdc.exe --cli out/FloraGPA-debug-backend/FloraGPA.Cli.exe --qt-bin out/FloraGPA-debug-backend --captures artifacts/rdc-capture-package --out artifacts/rdc-debug-check
```

Use a new output directory. The capture corpus comes from the optional recapture
validation tool. All GPU jobs and external oracles are run serially.

## Remaining work

The Qt PS/VS/CS debugger, replay-trace navigation adapter, source variables,
breakpoints/watches and debugger configuration have since been connected in
[REPLAY_DEBUG_UI_MIGRATION.md](REPLAY_DEBUG_UI_MIGRATION.md), with separate
recorded-trace and UI evidence. Existing GS/HS/DS checkpoint adapters are not
used as evidence for these different consumers. Other shared worker branches
(inventory/texture/postmesh/counters) remain outside this batch.
No claim is made for other RenderDoc ABIs, every shader operation, exhaustive
250,000-step exhaustion, or deployment on a separate clean Windows machine.
