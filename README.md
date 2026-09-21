# FloraGPA

Native Windows x64 DX11 frame analyzer. C++20, Visual Studio 2022, Qt 6 Widgets.

This repository migrates the independently recovered Python implementation in
`D:/CDXrepo/FloraGPA`. It does not load that implementation at runtime.
Migration status is tracked per source module in `docs/migration.json`.
Pending functionality is not represented as working functionality.

Output/SO setters now connect the dual binding model and cached history to native
replay, experiment projects and a compact Qt editor. Coverage and remaining
consumer dependencies are documented in
[`docs/OUTPUT_BINDING_MIGRATION.md`](docs/OUTPUT_BINDING_MIGRATION.md).

The Output tab supports live presentation/RTV/DSV selection, view-bounded layers,
MSAA resolve or individual samples, display channels/ranges and raw storage
export. See [`docs/FRAME_OUTPUT_MIGRATION.md`](docs/FRAME_OUTPUT_MIGRATION.md)
for parity evidence and remaining integration work.
Output settings and device/API selection are saved in experiment projects.
Click an output pixel to locate its API/resource; a buffer RTV pixel opens the
corresponding element bytes. See [`docs/OUTPUT_SESSION_MIGRATION.md`](docs/OUTPUT_SESSION_MIGRATION.md).
Output **Before event** stops before the selected command. Pipeline and input
inspection prepare its snapshot before reading; the boundary distinction and
missing-binding recovery are documented in
[`docs/BEFORE_BOUNDARY_MIGRATION.md`](docs/BEFORE_BOUNDARY_MIGRATION.md).

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

**Shader Debug** now contains native GS / HS / DS checkpoint views: Read,
Capture, Trace and Trace Input, instruction/source stepping, breakpoints,
frame-scoped variables and watches, configuration import/save and ZIP export.
See [CHECKPOINT_UI_MIGRATION.md](docs/CHECKPOINT_UI_MIGRATION.md) for the workflow,
verification and limits. VS / PS / CS now use the recorded native replay
debugger with instruction/source navigation, typed variables, callstack,
conditional/count breakpoints, watches, configuration import/save and JSON
export. See [REPLAY_DEBUG_UI_MIGRATION.md](docs/REPLAY_DEBUG_UI_MIGRATION.md).

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

Native CLI replay can optionally export a RenderDoc capture with
`--renderdoc "C:/Program Files/RenderDoc/renderdoc.dll"`. The original GPA command
and resource identities are retained in annotations/names. RenderDoc is loaded
only for this explicit option; neither Python nor qrenderdoc is used to capture.
See [RDC_CAPTURE_MIGRATION.md](docs/RDC_CAPTURE_MIGRATION.md).
`FloraGPA.Rdc.exe` now reads real Pixel History and CPU-write snapshots through
the optional RenderDoc 1.45 native replay API. It accepts an isolated JSON job;
see [RDC_HISTORY_MIGRATION.md](docs/RDC_HISTORY_MIGRATION.md). The main-window
**Pixel History** tab now provides native Read/Cancel, exact API navigation,
integer-aware before/after values, expandable details and JSON export, with
experiment-aware recapture caching. See
[PIXEL_HISTORY_UI_MIGRATION.md](docs/PIXEL_HISTORY_UI_MIGRATION.md).
The same native worker now accepts `debug-pixel`, `debug-vertex` and
`debug-thread` jobs with complete source/assembly and typed step traces. Their
Qt views share the Pixel History recapture cache and cancellation pipeline;
see [RDC_DEBUG_MIGRATION.md](docs/RDC_DEBUG_MIGRATION.md) for backend validation
and [REPLAY_DEBUG_UI_MIGRATION.md](docs/REPLAY_DEBUG_UI_MIGRATION.md) for the UI.

The native replay worker also accepts **inventory** and **texture** jobs,
preserving resource relationships, exact metadata and raw subresource bytes.
See [RDC_ASSETS_MIGRATION.md](docs/RDC_ASSETS_MIGRATION.md) for the job interface,
3D texture semantics and comparison evidence.

The right-side **Replay Metrics** inspector now measures generic replay counters,
with Selection/Frame views, filtering, API navigation, a counter catalog and
complete JSON export. It shares native recapture and caching with Pixel History
and Shader Debug. See [REPLAY_METRICS_MIGRATION.md](docs/REPLAY_METRICS_MIGRATION.md)
for validation and the observed BF1 PS invocation variability.

Use **F5** to replay, **F6** for GPU timings, **Escape** to cancel. Select API
events to inspect their pipeline and before/after output. Resources provide
texture mip/layer/slice previews, shader source/DXBC/reflection and buffer bytes.
The Buffer tab reads initial/before/after values and byte ranges as hex, ASCII or
32-bit words. Geometry provides IA tables and native Final/VS/DS/GS output,
stream and instance selection, typed attributes and a rotatable wireframe.
**VS identities** links native output to original input indices and instances,
with unique byte-preserving variants and a lossless reference table; see
[`docs/VS_IDENTITY_MIGRATION.md`](docs/VS_IDENTITY_MIGRATION.md).
**VS writes**, **DS writes** and **GS emissions** inspect original output records
with component validity, known identities and GS strip connectivity. These modes
preserve original downstream bindings through private output/SO copies. See
[`docs/OUTPUT_LOG_MIGRATION.md`](docs/OUTPUT_LOG_MIGRATION.md) for exports and limits.
**HS output** provides control-point and patch-constant tables, component validity,
original instance/patch identities and complete binary/CSV export. See
[`docs/HULL_OUTPUT_MIGRATION.md`](docs/HULL_OUTPUT_MIGRATION.md).
The native checkpoint parser and input-selector foundation is documented in
[`docs/CHECKPOINT_MODEL_MIGRATION.md`](docs/CHECKPOINT_MODEL_MIGRATION.md);
the complete DXBC transform and direct GPU tests are in
[`docs/DXBC_CHECKPOINT_MIGRATION.md`](docs/DXBC_CHECKPOINT_MIGRATION.md).
Production GS/DS/HS capture and binary/CSV export are available through the
`shader-checkpoint` CLI; see
[`docs/CHECKPOINT_CAPTURE_MIGRATION.md`](docs/CHECKPOINT_CAPTURE_MIGRATION.md).
Original SPDB/SDBG source-line maps and instruction locations are now included;
see [`docs/SOURCE_LINES_MIGRATION.md`](docs/SOURCE_LINES_MIGRATION.md).
SPDB/SDBG source-variable symbols are also included in checkpoint exports; native
register-to-source value resolution and SDBG assignment histories are available
as library APIs. See [`docs/SDBG_VARIABLES_MIGRATION.md`](docs/SDBG_VARIABLES_MIGRATION.md).
Source stacks,
per-frame locations and HS phase ownership are also included; see
[`docs/SOURCE_VARIABLES_MIGRATION.md`](docs/SOURCE_VARIABLES_MIGRATION.md) and
[`docs/SOURCE_STACK_MIGRATION.md`](docs/SOURCE_STACK_MIGRATION.md).
Native source navigation, conditional breakpoints and watch expressions are
available as library APIs; see
[`docs/SOURCE_NAVIGATION_MIGRATION.md`](docs/SOURCE_NAVIGATION_MIGRATION.md).
Debugger configuration and a compact Qt breakpoint/watch component are also
ported; see [`docs/NATIVE_DEBUG_CONFIG_MIGRATION.md`](docs/NATIVE_DEBUG_CONFIG_MIGRATION.md).
These components are now integrated into the GS / HS / DS checkpoint view;
see [CHECKPOINT_UI_MIGRATION.md](docs/CHECKPOINT_UI_MIGRATION.md).
Export retains JSON, CSV, raw output bytes and OBJ when positions are valid;
geometry choices are saved in experiment projects. Remaining stages and
validation limits are listed in
[`docs/POST_TRANSFORM_MIGRATION.md`](docs/POST_TRANSFORM_MIGRATION.md).
Resource names come from captured debug metadata.
Shader **Compile & Apply** and event enable/disable changes use an experiment
history with undo/redo and frame-bound JSON projects.
Replay reports and event Texture exports include the active experiment revision,
applied/pending events and replacement asset provenance. The collapsed Tasks log
shows a compact execution summary. See
[`docs/EXPERIMENT_REPORT_MIGRATION.md`](docs/EXPERIMENT_REPORT_MIGRATION.md).
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
The Texture toolbar provides **Import Input…** and **Import Output…** for packed
RAW subresources at the selected event. MSAA imports require an explicit sample.
Input clones are scoped to one event; output preconditions persist after command
submission. See [`docs/EVENT_TEXTURE_MIGRATION.md`](docs/EVENT_TEXTURE_MIGRATION.md)
for edit validation. Texture inspection now provides MSAA resolve/sample, typed
DXGI format, Y/UV plane, channel/range and DDS/RAW/PNG export controls. Legacy
P010/P016 initial data exports recovered Y only. See
[`docs/TEXTURE_INSPECTION_MIGRATION.md`](docs/TEXTURE_INSPECTION_MIGRATION.md)
for inspection evidence. Native planar Map preserves UV on WRITE/READ_WRITE and
leaves it undefined after DISCARD; NV12 Update and explicit planar source
replacements write complete Y/UV regions. Texture tooltips and JSON identify the
write source. See [`docs/PLANAR_WRITE_MIGRATION.md`](docs/PLANAR_WRITE_MIGRATION.md).
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

**Annotations** reads captured per-object Begin/End/Marker groups and explicit
QueryInterface context proofs. Filter names/IDs, inspect linked draws and evidence,
locate begin/end/draw API calls, or export all groups as JSON/CSV. Unknown identities
and interrupted/open groups remain explicit. **Range Metrics** prefills the closed
group's inclusive API endpoints in **GPU Statistics**.
See [`docs/ANNOTATION_MIGRATION.md`](docs/ANNOTATION_MIGRATION.md).

**GPU Statistics** measures a Draw/Dispatch, an inclusive command range, or the
entire frame on the selected hardware/WARP device. Pipeline invocations, occlusion,
four SO streams, overflow and elapsed time come from native DX11 queries. JSON/CSV
export preserves the sample's device, experiment and replay boundaries. These are
single replay measurements; range timings include binding, uploads and helpers.
See [`docs/GPU_STATISTICS_MIGRATION.md`](docs/GPU_STATISTICS_MIGRATION.md).

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
This view does not apply experiments. The **Snapshot** tab shows bindings with persistent setter edits
for the selected draw/dispatch, including shader and class-instance navigation.

Replay executes captured input-layout, vertex/index-buffer, six-stage SRV and sampler
bindings between draw/dispatch snapshots. CB/CB1 calls validate resource flags and
window alignment before applying native bindings. A selected boundary with missing
captured output views, input layouts or unresolved SRV slots returns an error; later
complete snapshots or ClearState restore binding validity. SRV replacement resolves
only the overwritten slots in that shader stage. Sampler, SRV, predicate,
output, SO, six-stage shader, IA and CB/CB1 setter editing is available.
The IA editor provides input-layout and index-buffer selectors and a vertex-buffer
range table. Persistent edits affect replay, Pipeline inspection, buffer patches
and geometry exports, with project save/load and undo/redo. See
[`docs/IA_SETTER_MIGRATION.md`](docs/IA_SETTER_MIGRATION.md).
The CB editor supports all six stages, start slots, buffer arrays and optional
CB1 first/count ranges. Moving a range preserves observed displaced windows;
ordinary setters reset CB1 windows. Constants and scoped buffer patches follow
the effective binding. See
[`docs/CONSTANT_BUFFER_SETTER_MIGRATION.md`](docs/CONSTANT_BUFFER_SETTER_MIGRATION.md).

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

Select an `OMSetRenderTargets`, `OMSetRenderTargetsAndUnorderedAccessViews`,
`CSSetUnorderedAccessViews` or `SOSetTargets` call, then **Edit > Edit Setter**.
The compact Render Targets, Unordered Access and Stream Output pages expose
resource slots, DSV, initial counters and byte offsets. **Keep bindings** preserves
the applicable OM group; **Provided** distinguishes null pointers from explicit
arrays, including empty arrays. Counter/offset cells accept decimal, hexadecimal
or `KEEP`. Changes support project save/load and undo/redo.

Output changes persist across snapshots and include collateral SRV/IA unbinding.
Buffer patches, SRV descriptor edits, counter inspection/edits and IA geometry
use the final bindings. SO append preservation requires a known native write
cursor. Full output verification reads native getters after applying setters and
edited snapshots. Texture input/output experiments and private diagnostic consumers remain separate migration work.
Global view descriptors now apply to native replay and binding inspection.

**Edit Setter** also edits topology, rasterizer state, viewport/scissor arrays,
blend state/factors/sample mask and depth/stencil state/reference. These changes
persist until a later setter of the same family or ClearState. Per-draw pipeline
editors inherit the modified state; Geometry uses the edited topology. See
[`docs/PIPELINE_SETTER_MIGRATION.md`](docs/PIPELINE_SETTER_MIGRATION.md).

```powershell
.\out\FloraGPA\FloraGPA.Cli.exe replay-pipeline D:\captures\sample.gpa_frame --event 430 --before --out D:\results\pipeline
```

Omit `--before` for the command's after boundary. Inspection writes
`replay-pipeline.json` without frame image readback.

Select a graphics draw and use **Edit > Edit Depth / Stencil** (also in the API Log
context menu) to change depth testing/writes, comparison, stencil masks/reference,
and front/back operations. The compact dialog saves only changed fields;
subsequent changes merge per field and support project history, undo and redo.
Edits apply at the selected draw, including its before boundary; the next unedited
draw restores its captured state. Reference values accept decimal or hexadecimal.
Select **Edit > Edit Rasterizer** to edit fill/cull mode, winding, depth bias,
depth clipping, scissor, multisample and line flags. The **Viewports** and
**Scissors** tabs edit up to 16 ordered slots; removing all rows clears the
binding. Changes merge per field and support the same history and save/load
workflow, including mixed depth/stencil edits and legacy wireframe/cull presets.
The next unedited draw restores its captured state.

Forced sample count and conservative rasterization use the device's native
D3D11.1/11.3 support. Unsupported combinations fail explicitly. Forced sampling
requires no DSV, disabled depth testing, compatible render targets and a pixel
shader without sample-frequency execution or depth output; shader replacements
are checked too. Conservative rasterization requires solid fill and device
support. Private coverage diagnostics remain pending.

**Edit > Edit Blend / Samples** provides a compact General page and eight render
target pages: alpha-to-coverage, independent blending, color/alpha factors and
operations, write channels, logic operations, blend constants and the uint32
sample mask. Decimal and hexadecimal masks are accepted. Edits preserve untouched
target fields, merge with rasterizer/depth edits, and support save/load, undo and
redo. `blend_disabled=false` restores each target's captured enable flag.
Logic operations require device and RTV-format support; invalid combinations
fail explicitly. MSAA masks and alpha-to-coverage execute on the GPU, while the
dedicated MSAA sample viewer and coverage diagnostics are still being migrated.

**Edit > Edit Sampler** edits any of the 16 sampler slots in all six shader stages
at the selected draw/dispatch. Choose the stage and slot, then **Load**; filtering,
address modes, comparison, anisotropy, border RGBA and LOD controls save only changed
fields. Minimum/maximum filtering requires native device support. Descriptor edits
are scoped to that event, including its before boundary and disabled events.

Select a captured `*SetSamplers` call in **All API calls**, then **Edit Setter** to
change its starting slot and resource array. Bindings persist across snapshots
until overwritten per slot or reset by ClearState. Moving or shrinking the array
preserves the previous bindings of displaced slots; missing prior observations
produce an explicit error. Descriptor edits inherit the final edited bindings,
regardless of operation order. Both editors support project save/load and undo/redo.
Private coverage/quad consumers remain pending.

**Edit > Edit Shader Resource View** edits the selected draw/dispatch's input view
in any of the six stages and 128 slots. Choose the stage/slot and **Load**, then
change the DXGI format, dimension, mip/layer/cube range or buffer element range.
The form follows the chosen dimension; format names and numeric values are both
accepted. `0xffffffff` selects all remaining mips. Invalid resource/format/range
combinations fail through native D3D11 validation. Edits support save/load and
undo/redo and restore the underlying binding at the next unedited event.

Temporary SRVs use the current view's native resource, including an edited buffer
input clone; other slots sharing the captured view remain independent. Native
overlap with an active output can unbind the edited SRV.

Select a six-stage **SetShaderResources** command and use **Edit Setter** to change
its starting slot and view array. Overrides persist per slot across draw/dispatch
snapshots until overwritten or cleared. Moving or shrinking an array preserves
previously observed bindings of the displaced slots; unknown bindings are rejected.
SRV conflicts track output subresources and read-only depth/stencil planes. Once an
output unbinds an SRV, removing that output does not resurrect the SRV. Descriptor
edits inherit the final setter edits regardless of operation order, and explicit
bindings remain visible even in shader-unused slots. Buffer patch validation,
the buffer editor and before/after exports also follow these bindings. Both editors use project
history, save/load and undo/redo.

Texture input clones and private diagnostic/debug consumers remain pending.
Unsupported experiment kinds remain explicit errors.

**Edit > Edit View Resource** changes a captured SRV, RTV, DSV or UAV across
all uses in the frame. Choose a view and **Load** its effective descriptor;
the compact form follows its dimension and exposes format, mip/layer/depth or
buffer element ranges and applicable flags. Global changes compose with event
SRV edits and support project save/load, undo and redo. Native D3D11 validates
resource compatibility during replay. The captured descriptor remains unchanged;
**Replay State** reports the native edited descriptor and its source.

Raw non-MSAA texture storage can be exported without display conversion:

```powershell
.\out\FloraGPA\FloraGPA.Cli.exe texture-storage D:\captures\sample.gpa_frame --event 430 --id 20 --out D:\results\texture
```

`texture.bin` contains tightly packed subresources in array-layer/mip order,
including all volume slices. Add `--before` or `--experiment` as needed.
Main-output view selection/typed conversion, MSAA sample inspection, planar
formats and coverage/debug consumers still have migration gaps. See
[global view migration](docs/VIEW_MIGRATION.md) for scope and validation.

**Pipeline > Predicate** reads native occlusion and stream-output overflow predicates
at the selected command boundary. The compact inspector shows the query status,
boolean result, binding and raw predicate value, with JSON export. Active queries
and hint-only queries have no readable result. Captured Begin/End/SetPredication
calls and snapshot bindings execute on the GPU; inspection helper draws are
excluded from captured query intervals. Select SetPredication in **All API calls**,
then use **Edit > Edit Setter** (also in the API Log context menu). Choose a predicate
or None and enter its raw uint32 BOOL as decimal or hexadecimal. Edits persist
across subsequent draw/dispatch snapshots until a captured setter or ClearState,
and support project save/load plus undo/redo. Begin/End pairing is not editable.

```powershell
.\out\FloraGPA\FloraGPA.Cli.exe predicate D:\captures\sample.gpa_frame --event 430 --id 60 --out D:\results\predicate
```

Add `--before` for the command's before boundary. The output is `predicate.json`.
Raw BOOL values are preserved: the tested WARP and hardware drivers differ for
noncanonical values such as 7 and `0xffffffff`, also reproduced by the Python
reference. Results describe this replay's GPU query, not captured GetData bytes.

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
edited replay requires known history. SO setter experiments preserve known native
append positions across snapshots. Private diagnostic SO passes remain pending.

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

Six-stage shader setters support program/class replacement, persistent snapshot
overrides, reset/ClearState and project undo/redo. Constants follow the rebound
shader, and stage navigation opens it for Compile & Apply. See
[`docs/SHADER_SETTER_MIGRATION.md`](docs/SHADER_SETTER_MIGRATION.md).
