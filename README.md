# FloraGPA

Native Windows x64 DX11 frame analyzer. C++20, Visual Studio 2022, Qt 6 Widgets.

This repository migrates the independently recovered Python implementation in
`D:/CDXrepo/FloraGPA`. It does not load that implementation at runtime.
Migration status is tracked per source module in `docs/migration.json`.
Pending functionality is not represented as working functionality.

## Build

Install Visual Studio 2022 with **Desktop development with C++**, the MSVC v143
x64 toolset and a Windows SDK; CMake 3.25 or newer; and Qt 6.11.2
**MSVC 2022 x64** (the MinGW kit is not ABI-compatible).

From the repository root, configure with your own Qt installation path:

```powershell
cmake --preset vs2022 -DCMAKE_PREFIX_PATH="C:/Qt/6.11.2/msvc2022_64"
cmake --build --preset release
ctest --preset release
```

Use `-DFLORA_BUILD_GUI=OFF` to omit Qt Widgets and the desktop application.
The CLI still uses Qt Core and Gui for file handling and PNG output.
Capture files and GPU test outputs stay outside Git. Commits use the configured
personal Git identity and English titles and bodies. No remote is configured.

`CMakeLists.txt` and `CMakePresets.json` are the version-controlled project
definitions. The configure command generates `build/vs2022/FloraGPA.sln` and
the `.vcxproj` files; open that solution in Visual Studio 2022 if desired.
Generated projects and their machine-specific paths stay in the ignored
`build/` directory. A fresh clone contains the source and vendored JSON headers
needed to generate them; Python, Intel GPA and the original workspace are not
required to compile the application.

CMake remembers the Qt path in the local build cache. Machine-specific presets
may also be kept in the ignored `CMakeUserPresets.json`.
Local `AGENT.md` / `AGENTS.md` instruction files are ignored as well.

Capture fixtures are optional and are not distributed with the repository.
To enable fixture-dependent checks, configure again with
`-DFLORA_TEST_CAPTURE_DIR="D:/captures"`, pointing to the external GF2/BF1
fixtures. These checks skip when this variable is empty; GPU checks also need
a working D3D11 adapter.

## Run and deploy

```powershell
.\tools\package.ps1
.\out\FloraGPA\FloraGPA.exe
.\out\FloraGPA\FloraGPA.Cli.exe replay D:\captures\sample.gpa_frame --out D:\results\sample
```

To package alongside a running build, use
`./tools/package.ps1 -OutputDirectory ./out/FloraGPA-next`.

The entire `out/FloraGPA` directory is the application package, including Qt
plugins and app-local VC143 runtime DLLs. Keep the worker beside the GUI.
No Python environment, GPA installation or original source directory is used
by the application. Package portability has been checked on this host with
Windows-only child process paths, not yet on a separate clean Windows machine.

Use **F5** to replay, **F6** for GPU timings, **Escape** to cancel. Select API
events to inspect their pipeline and before/after output. Resources provide
texture mip/layer/slice previews, shader source/DXBC/reflection and buffer bytes.
The Buffer tab reads initial/before/after values and byte ranges as hex, ASCII or
32-bit words. Geometry provides IA tables and a rotatable wireframe, with CSV/OBJ
export. Resource names come from captured debug metadata.
Shader **Compile & Apply** and event enable/disable changes use an experiment
history with undo/redo and frame-bound JSON projects.
The **Edit** menu and API Log context menu provide **Edit Clear Values** and
**Replace Update Source**, plus enable/disable for recovered resource-writing
commands. Update sources are tightly packed binary assets; the file picker
shows the required byte count. Inspect the affected resource at **After event**
to see its edited contents. Undo/redo refreshes the current texture or buffer view.
The Buffer toolbar provides **Edit Bytes** and **Import Patch** at an unsigned
byte offset for the selected draw/dispatch. **Before event** displays patched
inputs; input-only edits are scoped to that command, so **After event** restores
the original input storage. Output buffer edits persist after submission while
preserving UAV counters. Geometry inspection also uses the edited inputs.
The Buffer **Constants** tab shows reflected scalar, vector, matrix, array and
structure fields, including CB1 binding ranges. Read **Before event**, select a
field and use **Edit Value**. Changes preserve padding and untouched scalar bits
and undo as one operation. Per-component tooltips show byte offsets and raw bits;
`bits:HEX` accepts an exact scalar bit pattern. Stripped shaders without reflected
variable names leave the field list empty, as in the reference implementation.
The Buffer **UAV Counters** tab reads Append/Consume and Counter values per view.
Use **Edit Counter** at **Before event** to change the selected draw/dispatch or
the frame's initial counter value. Captured resets still override initial values.
Edits support the full uint32 range, project save/load and undo/redo; previewing
an event restores its prior counter, while a submitted event retains its changes.

API Log inspection preserves captured scalar/array fields, byte offsets, raw bits,
and resource references. Inspect **Captured fields** and **References** in the
right Inspector; double-click a resource reference to navigate to its buffer or
texture subresource. Search accepts space-separated terms including decode status,
and **Referenced resource ID** also finds references through views. The API Log
context menu exports filtered JSON and CSV. Unknown, partial, and invalid records
retain their undecoded bytes; decoded metadata does not imply GPU replay support.
Captured GetData results use only earlier metadata for the same query ID. Missing
high words, incomplete results and conflicting descriptors remain explicit.

The Inspector identifies captured and inferred contexts separately. Missing context
kind can be recovered only from a validated staging Texture2D Map READ / Unmap pair;
interface version, creation flags and captured pointer remain unknown. Contradictory
evidence blocks recovery. **Analysis > Contexts & Command Lists** shows identities,
recovery evidence and command-list inventories, with API navigation and JSON export.
Command-list pointers and resource IDs remain separate candidates. As in the Python
reference, ExecuteCommandList execution is not restored; only captured immediate-context
FinishCommandList no-op results with a zero returned reference are accepted by replay.

**Pipeline > Captured State** reconstructs the selected command's before/after state
from the original capture. Use **Read**, then filter by field or known/unknown state.
Each field retains its source and event; double-click a resource value or source event
to navigate. Export preserves all fields, including filtered-out rows and uncertainty
notes. Reads run in the background and a changed selection invalidates old results.
This view does not apply experiments. The existing **Snapshot** tab remains available
for draw/dispatch state blocks.

Replay executes captured input-layout, vertex/index-buffer, six-stage SRV and sampler
bindings between draw/dispatch snapshots. CB/CB1 calls validate resource flags and
window alignment before applying native bindings. A selected boundary with missing
captured output views, input layouts or unresolved SRV slots returns an error; later
complete snapshots or ClearState restore binding validity. SRV replacement resolves
only the overwritten slots in that shader stage. Setter editing is still being migrated.

**Pipeline > Replay State** reads actual D3D11 bindings at the selected command's
before/after boundary through the isolated worker. It includes all six shader stages,
CB1 windows, IA/RS/OM/SO and predication getters, view/sampler descriptors and extended
rasterizer/blend/depth state. The same field filter, resource navigation and JSON export
are available as in Captured State. Supported experiments apply to the inspection;
draw boundaries retain scoped input clones and disabled commands' prepared bindings.
Runtime object tokens preserve clone identity and ambiguous captured-ID provenance.
SO live write cursors have no native getter and remain unknown. Unsupported replay
commands and experiment types remain errors; this inspector does not enable them.
Changing the selection, boundary, experiment or adapter invalidates previous results.

```powershell
.\out\FloraGPA\FloraGPA.Cli.exe replay-pipeline D:\captures\sample.gpa_frame --event 430 --before --out D:\results\pipeline
```

Omit `--before` for the command's after boundary. Inspection writes
`replay-pipeline.json` without frame image readback.

Dynamic shader linkage restores captured named and explicitly created class instances
in all six draw/dispatch shader stages. Resource properties expose the linkage,
instance descriptor and names; shader properties show the interface slot count.
Static shader replacements remove captured interfaces, and dynamic replacements
preserve ordered class bindings. The existing Compile & Apply and undo workflow
also works with linked shaders, including geometry shaders with stream output.
Shader setter experiments are still pending. On the tested NVIDIA 10de:249d adapter, a reproduced
driver crash for empty-function-table pixel shaders is rejected with a WARP hint;
select WARP to execute the original bytecode without modification.

```powershell
.\out\FloraGPA\FloraGPA.Cli.exe class-linkage D:\captures\sample.gpa_frame --id 62 --out D:\results\class
```

This exports captured class linkage/instance metadata as `class-linkage.json`.
The `shader` export also includes the linkage ID and reflected interface slot count.

Stream output restores captured declarations, target buffers, explicit resets and
append bindings across all five recovered context versions. GS bytecode, VS/DS
passthrough and output-only signatures are supported. Shader **Reflection** and
Inspector show the SO declaration; **Pipeline > Snapshot** links its target buffers.
Replay reports include actual per-stream SO query history. DrawAuto uses known
written-byte history and the first IA buffer's offset/stride, including shader
replacements and temporary input edits. **Geometry > Inspect IA** exports these
effective arguments and identifies their provenance. Without known in-frame
history, unedited replay retains the captured count and marks it unverified;
edited replay requires known history. Setter experiments, retained outputs under
those experiments, predication and private diagnostic SO passes remain pending.

```powershell
.\out\FloraGPA\FloraGPA.Cli.exe commands D:\captures\sample.gpa_frame --out D:\results\api
# Optional: --filter "set decoded" --resource 25733
.\out\FloraGPA\FloraGPA.Cli.exe contexts D:\captures\sample.gpa_frame --out D:\results\contexts
.\out\FloraGPA\FloraGPA.Cli.exe command-lists D:\captures\sample.gpa_frame --out D:\results\lists
.\out\FloraGPA\FloraGPA.Cli.exe command-state D:\captures\sample.gpa_frame --event 430 --before --out D:\results\state
# Omit --before to inspect after the command.
```

See [migration status](docs/MIGRATION_STATUS.md) for implemented features and
the remaining parity gaps. This build is not the completed migration.

## Reference comparisons

These scripts use Python only as a development oracle. Neither the GUI nor the
worker launches or loads Python.

```powershell
python tools/validate_native.py --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --out artifacts/golden-run --isolated-env
python tools/validate_geometry.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/geometry-run
python tools/validate_buffers.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/buffer-run
python tools/validate_commands.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/command-run
python tools/validate_buffer_edits.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/buffer-edit-run
python tools/validate_class_linkage_port.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --isolated-env --out artifacts/class-linkage-run
python tools/validate_stream_output_port.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --isolated-env --out artifacts/stream-output-run
```

For constant-layout comparisons, run `FloraConstantTests` with
`FLORA_CONSTANT_EVIDENCE_DIR` pointing to a new artifact directory. Pass that
directory as `--fixture` to `tools/validate_constants.py`, together with the same
`--reference`, `--exe`, `--captures`, `--qt-bin` and a new `--out` directory.

Run GPU checks serially and use a new output directory each time.

The SO comparison also accepts `--captured-dir` pointing to the preserved
`analysis/capture_samples/stream_output` suite. It verifies the capture hashes,
original image/storage hashes, Python/native SO histories and DrawAuto counts,
and disabled producers on hardware and WARP. These optional captures are not
distributed with the repository.
Add `--linear-captured-dir` pointing to `analysis/capture_samples/linear` to
include the original point/line SO captures and their DrawAuto count recovery.

For UAV counter comparisons, run `FloraUavCounterTests` with
`FLORA_COUNTER_EVIDENCE_DIR` pointing to a new artifact directory, then pass it
as `--fixture` to `tools/validate_uav_counters.py` with the same reference/executable
arguments above. Parallel Append payloads in the BF1 case are compared as element
multisets, with exact counter values and exact bytes outside the appended range.

For API inspection comparisons, run `FloraApiCommandTests` with
`FLORA_API_EVIDENCE_DIR` set to an artifact directory. Run
`tools/validate_api_commands.py` with the reference/executable arguments above and
one `--fixture` argument for each generated `.gpa_frame`. This checks complete
record metadata and parsed CSV rows; invalid-record diagnostic wording may differ.

For context recovery and command-list parity, run `FloraContextTests` with
`FLORA_CONTEXT_EVIDENCE_DIR` pointing to a fixture directory, then run
`tools/validate_contexts.py` with `--reference`, `--exe`, `--captures`, `--qt-bin`,
`--fixtures` and a new `--out` directory. This compares context evidence, command-list
inventories and API metadata, including rejected recovery paths, plus GPU buffer
outputs from a capture whose context identity is missing.

For command-state parity, run `FloraCommandStateTests` with
`FLORA_STATE_EVIDENCE_DIR` and `FLORA_TEST_CAPTURE_DIR` set, then run
`tools/validate_command_state_port.py` with the same reference/executable arguments,
`--fixtures` pointing to that evidence directory and a new `--out` directory.
It compares before/after fields and provenance against Python, reuses the original
validator's synthetic fixture, and checks every qualified snapshot in GF2 and BF1.
