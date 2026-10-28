# DX11 native analysis migration

Baseline: `3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28` on `main`.
Implementation branch: `feature/dx11-native-backend`.

This is an implementation ledger, not a standalone release acceptance report.
RenderDoc removal is blocked until every formerly successful capability below has a
verified native replacement. The existing backend remains available during migration.
Neither reserved CLI action names nor native GPU checkpoints constitute a completed
VS/PS/CS debugger or fragment history implementation.

## Stage gates

The latest [per-sample merger test evidence](research/DX11_NATIVE_PIXEL_MERGER_TESTS.md)
adds independent depth, stencil, alpha and combined late-test observations to
private history joins. Original fragment qualification and public PS selection
remain open; no migration gate is waived by those measurements.

| Stage | Gate | Current evidence |
| --- | --- | --- |
| P0 | Reproduce baseline, enumerate dependencies and retain reports/performance | Clean full build passed; 314/317 baseline tests pass with three reproduced historical failures; detailed end-to-end feature parity remains open |
| P1 | Close GPA history, geometry, query and debugger implementations with independent probes | Static history/geometry/query call paths identified; original debugger requests observed on three captures with null engine and no result callback; concrete engine remains unresolved |
| P2 | Versioned analysis requests, context identity, Worker isolation and GUI scheduling | Native CLI/Worker infrastructure and artifact verification tested; Replay Metrics uses asynchronous native scheduling; remaining consumers pending |
| P3 | Resource, counter and geometry parity | Native inventory/events/texture/mesh and all-event counters exposed; VS identities, selected real-capture meshes and native UI exports tested on three adapters; full parity matrix pending |
| P4 | Event history including writes without changed values | Explicit event-storage CLI/Worker, typed values and narrow ordinary-texture readback; full format/semantic matrix and special-format performance remain open |
| P5 | Fragment history, failure reasons and shader outputs | Selected-region occlusion, batched pre-depth candidates, compact private outputs, multisample private merging, observed primitive IDs and original-PS private checkpoints with raster-input/current-resource shader-output joins are tested on three adapters; original fragment qualification, complete instance/identity coverage, ordering and full reconstruction remain open |
| P6 | Independent DXBC VS/PS/CS execution with source/debugger parity | Native VS/CS and explicit private-primitive PS jobs, sessions and UI are integrated; private PS inputs, journals, minimum-precision sampling, typed-buffer loads, outputs and helper memory suppression are tested; GF2/BF1/Helldivers selected private quad jobs complete on three adapters; original fragment qualification, full typed-format/precision coverage, unscoped PS selection and source parity remain open |
| P7 | All consumers switched; runtime/build/package/test dependencies removed | Blocked by P4-P6 and GUI parity |
| P8 | Independent clean builds, portable package and module/performance audit | Pending; no standalone package claimed |

## Existing dependency inventory

| Surface | Implementation and dependency | Required replacement |
| --- | --- | --- |
| Recapture | `src/replay/RenderDocCapture.cpp`: LoadLibraryExW, RENDERDOC_GetAPI 1.6.0, capture options, StartFrameCapture/EndFrameCapture/GetCapture; configured before device creation | Remove secondary capture entirely |
| Runtime replay | `src/rdc/Backend.cpp`: dynamic 1.45 loader, allocation ABI bridge, capture-file and replay-controller C++ interfaces | Direct GPA replay and project-owned results |
| Event/resource correlation | `ReplayAnnotation.h`, resource private debug names and `RdcEvents.cpp` | GPA identities throughout |
| Jobs and CLI | `RdcJobs.cpp`, `RdcAnalyzeCli.cpp`, `FloraGPA.Rdc` | `FloraGPA.Worker` native jobs; migration error for retired input |
| GUI analysis | `MainWindow.cpp`, `ReplayDebugView`, `ReplayMeshView`, `PixelHistoryView`, RDC counter/asset views and adapters | Native jobs and corresponding result adapters; preserve operations and exports |
| Settings/cache | `RenderDocLibrary.h`, DLL picker, experiment debug identity and cached recapture | Ignore retired paths; validated shader/instruction migration; native context identity |
| Serialization | `src/rdc/DebugTrace.cpp`, `Counters.cpp`, `Assets.cpp` | Project-owned enums, bit patterns, validity, units, fragments and traces |
| Build | CMake unconditionally builds RDC jobs/worker and links serializer tests to vendor headers | Remove targets, includes and dependency edges after parity |
| Packaging | `tools/package.ps1`, pinned runtime manifest, DLL/tool/CRT copies and license inventory | Portable package without RenderDoc artifacts or discovery |
| Tests/tools | RDC job/assets/counters/debug/worker/event tests and comparison scripts | Native contracts and deterministic independent probes |
| Third party | `third_party/renderdoc` API/replay headers, ABI types and runtime notices | Remove active dependency; retain historical provenance in history/docs |
| DXBC operand framing | The former derived scalar table is replaced by pinned Microsoft format facts with independent D3DCompiler probes | Completed for the table only; this is not shader execution parity |

The replay counter implementation collects generic DX11 counters (GPU duration,
eleven pipeline-statistics fields and occlusion). Enumerated vendor metadata is not
evidence that vendor counter collection currently works. Existing native Intel
Metrics Discovery facilities are separate from this dependency.

The active external call surface is:

- Application capture API: GetAPI, SetCaptureFilePathTemplate, MaskOverlayBits,
  SetCaptureKeys, SetCaptureOptionU32(RefAllResources), GetNumCaptures,
  IsFrameCapturing, StartFrameCapture, SetCaptureTitle, EndFrameCapture and GetCapture.
- Replay exports: GetVersionString, IsReleaseBuild, GetCommitHash, AllocArrayMem,
  FreeArrayMem, InitialiseReplay, OpenCaptureFile and ShutdownReplay.
- Capture/controller lifetime: OpenFile, OpenCapture, Shutdown and GetAPIProperties.
- Actions/identity: GetRootActions, GetStructuredFile, GetResources, SetFrameEvent,
  GetD3D11PipelineState and GetUsage.
- Assets: GetTextures, GetBuffers, GetTextureData, GetBufferData and GetPostVSData.
- Counters: EnumerateCounters, DescribeCounter and FetchCounters.
- History: PixelHistory and PickPixel (including supplemental CPU-write snapshots).
- Debugger: DebugVertex, DebugPixel, DebugThread, ContinueDebug and FreeTrace.

History compatibility includes pre/post color, depth and stencil; shader output;
primitive/fragment identity; sample/subresource; failure flags; GPA provenance;
record kind and CPU-write coverage gaps. Snapshot records must not gain invented
primitive IDs, shader outputs or per-fragment failure reasons. Debug compatibility
also includes input/output/resource descriptions, source files and locations,
instruction mapping, changes, call stacks, validity and raw register storage.

### Serialized field checklist for adapter migration

These are the baseline field spellings to audit, not a requirement to preserve
RenderDoc numeric enum values in the new schema. Every changed spelling or type
needs an explicit consumer/export mapping before the old adapter is removed.

| Result structure | Baseline fields |
| --- | --- |
| Resource format | type, compType, compCount, compByteWidth |
| Resource description | resourceId, type, autogeneratedName, name, initialisationChunks, derivedResources, parentResources, annotations |
| Texture description | resourceId, format, dimension, type, width, height, depth, cubemap, mips, arraysize, creationFlags, msQual, msSamp, byteSize |
| Buffer description | resourceId, creationFlags, gpuAddress, length |
| Mesh layout | vertexResourceId, vertexByteOffset, vertexByteStride, indexResourceId, indexByteOffset, indexByteStride, baseVertex, numIndices, topology, format, unproject, nearPlane, farPlane, status |
| Mesh result | stage, mesh, vertex_count, index_count, face_count; binary vertex/index data, CSV and OBJ |
| Counter description | counter, name, category, description, resultType, resultByteWidth, unit, uuid.words |
| Counter value | eventId, counter, name, unit, value, GPA provenance |
| Pixel value | floatValue, uintValue, intValue (four components each) |
| Pixel modification value | col, depth, stencil |
| Fragment | eventId, fragIndex, primitiveID, directShaderWrite, unboundPS, preMod, postMod, shaderOut |
| Fragment tests | sampleMasked, backfaceCulled, depthClipped, depthBoundsFailed, viewClipped, scissorClipped, shaderDiscarded, depthTestFailed, stencilTestFailed, predicationSkipped |
| Added history metadata | record_kind, color_interpretation, pixel_value_changed, write, GPA provenance and gap reasons |
| Shader variable | name, rows, columns, type, flags, value, members |
| Raw value interpretations | f16v, f32v, f64v, s8v, u8v, s16v, u16v, s32v, u32v, s64v, u64v (16 lanes each) |
| Source variable mapping | name, type, rows, columns, offset, signatureIndex, undefinedValue, variables (name/type/component) |
| Source location | disassemblyLine, fileIndex, lineStart, lineEnd, colStart, colEnd |
| Trace | stage, inputs, constantBlocks, readOnlyResources, readWriteResources, samplers, sourceVars, instInfo |
| Instruction metadata | instruction, lineInfo, sourceVars |
| Execution step | nextInstruction, stepIndex, flags, callstack, changes (before/after variables) |
| Shader source metadata | compileFlags.flags (name/value), files (filename/contents), entrySourceName, entryLocation, editBaseFile, encoding, compiler, debuggable, sourceDebugInformation, debugStatus, debugInfoLoadingLog |

Some global source-mapping offsets are deliberately null in the current serializer
because the upstream ABI leaves them uninitialized. CPU-write snapshots also
deliberately use null fragment IDs, primitive IDs, shader output, depth and stencil.
Those unknowns must remain unknown unless a native probe establishes the value.

## Capability matrix

“Available in baseline source” is not a claim of a newly reproduced runtime pass.
Baseline failures must be recorded on the same machine before they are exempted.

| Capability available in baseline source | Native implementation to reuse/build | Required evidence before cutover |
| --- | --- | --- |
| Capture and GPA replay | Existing capture/runtime and Replay | GF2, BF1, Helldivers; all capture/replay modes |
| Automatic GPU timing and interactive event chart | NativeSample, GpuProfile, EventChart | Auto collection, selection, zoom, cancellation, cache invalidation |
| Inventory, actions and resource identity | Frame, CaptureNames, ApiCommands | GPA IDs, event boundary, creation/alias/experiment semantics |
| Texture export/preview | TextureInspector, TextureReadback, MSAA paths | Formats, typed views, mip/layer/slice/sample, raw bytes and previews |
| Generic counters | NativeSample, GpuStatistics | Units, valid/disjoint timestamps, pipeline and occlusion values |
| Replay mesh and exports | PostTransform, Geometry, ReplayMesh presentation | Indexed/instanced/indirect/DrawAuto, VS/DS/GS, SO streams, vertex identities |
| Pixel History | New event journal plus multipass fragment extraction | CPU/Clear/Copy/Resolve/Draw/Dispatch, unchanged writes, subresources, fragments |
| Pixel failure reasons | Independent diagnostic passes | Coverage, depth/stencil, scissor, cull, discard, sample mask, alpha-to-coverage |
| VS debugging | New DXBC executor and native input collection | Vertex/index/instance selection, instruction and source trace |
| PS debugging | DXBC executor, quad/helper/sample and texture helpers | Derivatives, helper lanes, resource side effects, original fragment selection |
| CS debugging | DXBC executor and thread-group scheduler | Shared memory, barriers, atomics, group/local thread identity |
| GS/HS/DS checkpoint | Existing CheckpointInspection and DXBC checkpoint facilities | Preserve existing stage restrictions and all currently working selection/trace operations |
| Source debugging UX | ReplayDebugModel, source/stack/expression/config components | Back/forward step, breakpoint conditions/hits, watch, navigation, export |
| Debug values | New result types and serializer | All 16 raw lanes, width/sign/float interpretations, validity, NaN/Inf/-0, unknown offsets |
| State safety | Private snapshots plus isolated replay | Resources, bindings, hidden UAV counters, SO cursors, predicates after diagnostics/failure |
| Recovery | Worker jobs and desktop recovery | Cancel, timeout, crash, retry, switching frame/experiment, stale-result rejection |

## Native entry point (incremental implementation)

```text
FloraGPA.Cli analyze frame.gpa_frame inventory --out inventory
FloraGPA.Cli analyze frame.gpa_frame events --out events
FloraGPA.Cli analyze frame.gpa_frame counters --out counters-all
FloraGPA.Cli analyze frame.gpa_frame counters --gpa-event 1000 --out counters
FloraGPA.Cli analyze frame.gpa_frame texture --resource 20 --gpa-event 2000 --out texture
FloraGPA.Cli analyze frame.gpa_frame mesh --gpa-event 1000 --stage vs --out mesh
FloraGPA.Cli analyze frame.gpa_frame mesh --gpa-event 1000 --stage vs-index --instance 0 --out mesh-identities
FloraGPA.Cli analyze frame.gpa_frame mesh --gpa-event 1000 --stage vs-inputs --instance 0 --out mesh-inputs
FloraGPA.Cli analyze frame.gpa_frame debug-thread --gpa-event 893 --group 0 0 0 --thread 0 0 0 --out compute-debug
FloraGPA.Cli analyze frame.gpa_frame debug-vertex --gpa-event 876 --vertex 0 --instance 0 --out vertex-debug
FloraGPA.Cli analyze frame.gpa_frame history --history-scope private-draw --gpa-event 113 --resource 80 --x 799 --y 480 --output-merger --primitive-identity --out draw-history
```

The native entry point never recaptures to RDC or selects an external replay
backend. Event history requires `--history-scope events`; `debug-thread` uses the
native compute job described in `DX11_NATIVE_COMPUTE_DEBUG.md`. `debug-vertex`
uses the observed draw inputs described in `DX11_NATIVE_VERTEX_DEBUG.md`.
Private draw observations use `--history-scope private-draw`; their explicit
scope, options and limits are described in [Native private draw history](NATIVE_PRIVATE_DRAW_HISTORY.md).
Complete fragment history and PS debug actions still fail explicitly until implemented.
During this stage the legacy command is still present for baseline comparison;
its final retirement and GUI cutover remain separate gates.

Without an event/range selection, `counters` measures the 13 generic DX11 values
for every draw/dispatch and recovered resource-writing command in one replay.
Draw/dispatch intervals start after binding; resource-write intervals include the
command implementation, including CPU-upload work. Query storage is bounded to
64 pending samples. One completion fence covers each ended batch, avoiding a
separate flush/completion round trip for every sample. Counter identifiers and units
are strings owned by this project; invalid GPU durations are null. An explicit
event or inclusive range retains the existing native statistics export, including
stream-output and predication details. Vendor-specific metrics are not implied.

Requests use GPA event/resource IDs and project-owned schema version 1. A result
records the frame SHA-256, experiment file SHA-256 and effective experiment,
Worker executable SHA-256, backend version, selection and actual device identity
(including LUID and driver version when available). Metadata-only inspection has
no device and records a null adapter. The result context digest is validated along
with a unique request ID. Missing data is not reconstructed from guesses.

Each analysis uses a separate Worker process in a Windows job object. A readiness
handshake prevents analysis before assignment to the job. Timeout/cancellation
terminates the process tree; a failed job cannot publish an accepted success.
`result.json` is published after the action exports. Input and bounded worker logs
are retained beside CLI results. The desktop controller owns a temporary workspace,
retains verified action bytes and removes the workspace after completion.

### Replay Metrics desktop migration

Replay Metrics now uses `NativeAnalysisController` and `NativeCountersView` with
the same native Worker job API as the CLI. Frame/Worker hashing, experiment
serialization, process execution and artifact verification run off the GUI thread.
Cancellation is checked again when the queued completion reaches the GUI, so a
late success cannot replace a cancelled request. The entire controller operation
has a deadline, and context/request checks reject results after frame, experiment,
adapter or selection changes during work. Completed frame-wide values survive
ordinary selection changes. There is no recapture or RenderDoc backend fallback
in this consumer; retired library settings are ignored here.

Selection/Frame scope, filtering, exact uint64 values, null durations, details,
event location, cancellation/retry and JSON export are retained. The catalog uses
stable FloraGPA string identities, descriptions, category, byte width, type and
unit; external numeric IDs and UUIDs are replaced rather than reused as native
identities. The actual adapter and complete result context are exported. External
vendor-provider catalogs are not enumerated by this generic query implementation;
the existing native Intel Metrics consumer remains separate. Catalog coverage on
other installed vendor providers still needs the full capability audit.

The old counter view is compiled only into migration tests. Its historical report
tests and the other RenderDoc consumers remain until their respective replacements
pass. Current real-capture comparisons have no missing mapped generic values, but
BF1 and Helldivers retain unresolved PS invocation differences. The native values
match frozen-main native statistics at three investigated events; this establishes
provenance, not complete equivalence to the external replay. See the validation
ledger for exact counts, retained failures and performance observations. P3 is not
closed by this desktop migration.

Backend revision `dx11-native/2` publishes an artifact manifest containing the size
and SHA-256 of every action export. Transport files (`job.json`, `worker.log`,
`result.json`) are not action artifacts. The submitting process verifies all
exported bytes before returning success, using bounded streaming reads and the
same cancellation/deadline. Consumers can instead retain verified bytes with an
explicit total payload budget. That budget does not include parsed JSON, geometry
tables or canonical export reconstruction; it is not a peak-memory guarantee.

`vs-index` exports each assembled reference's instance, input vertex index and
shader vertex ID in `identities.json`. Unique vertices and references remain
separate, including distinct output bit patterns for the same identity. The
`NativeMesh` reader validates the request context, selection, binary data, identity
records, counts, tables and canonical CSV/OBJ exports before accepting a result.
It owns verified bytes independently of the worker directory and does not use
RenderDoc resource IDs, topology enums or mesh layouts. Hull phase exports retain
their existing separate representation; they are not coerced into assembled meshes.

### Complete VS input references

`vs-inputs` replays the original draw as points with the original VS and input
bindings. It does not patch shader bytecode or reserve output registers/UAV slots.
The original index buffer, offsets, base vertex, instance parameters, indirect
arguments and predicate remain native. Stream-output order is checked against the
input reference count; the shader's actual VertexID/InstanceID outputs provide an
independent check in the synthetic tests. Original resource contents, hidden UAV
counters and SO cursors are restored before the diagnostic returns.

The point capture retains incomplete tails and adjacency helper references. Cut
indices remain explicit null entries in the original reference sequence; they do
not receive fabricated output rows. The original topology reconstructs preview
faces/lines separately from the captured points. `input_references.csv` maps each
instance/reference, including cuts, to its output row. Inspection point counts
are explicitly distinguished from original primitive counts. These are replayed
VS values, not records of the original hardware invocation/cache schedule, and
atomic return ordering is not promised to repeat.

The Geometry/Independent view exposes **VS input replay**, with expanded/unique
vertices, index mapping, original references/cuts and instance selection. Project
settings round-trip this stage/table selection. This still uses the existing
native geometry Worker route; unified analysis scheduling and Replay Mesh tab
cutover remain separate work.

OSG1 output signatures now retain stream and minimum-precision metadata. The
parser is checked against `ID3D11ShaderReflection::GetOutputParameterDesc`, and
minimum-precision outputs are tested through native SO on all three adapters.
No conversion to a legacy signature or signature-carrier instrumentation is needed
for this VS path.

The implementation follows the point-list/null-GS SO use case and topology-independent
index cut semantics in the [D3D11 functional specification, sections 8.11 and 14.3](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
Signature metadata is independently checked using the documented
[D3D11 signature reflection descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d11shader/ns-d3d11shader-d3d11_signature_parameter_desc).

### Mesh cutover gaps

The Replay Mesh GUI defaults to its existing path until its successful baseline
coverage is preserved. An explicit Native preview mode now exercises the unified
Worker route without automatic fallback. The following are explicit P3 blockers,
not exemptions:

- The older assembled `vs-index` mode still omits adjacency helpers and incomplete
  tails. `vs-inputs` now preserves them in the three-adapter matrix, including full
  signatures and indirect draws. Full baseline mesh parity still needs the remaining
  real-capture, format and workflow comparisons before default cutover.
- Per-instance HS/DS/GS boundaries with UAV-dependent geometry remain unverified.
  A sufficient bytecode and input-alias proof now permits independent writes,
  including dead hidden-counter results. Reads are now permitted when actual
  resource ownership excludes writes by the executed geometry stages. Read/write
  feedback, live results, aliases and unproved liveness retain explicit rejection.
  Tests retain those rejections and full-draw capture separately from
  independent-write and pure-shader selection.
  They do not establish baseline parity for the rejected cases.
- The new VS input path removes the free-signature-component requirement and
  supports OSG1. Other instrumented stages retain their separate restrictions.
  Nonzero GS SO streams, deferred contexts and complete real-capture mesh parity
  still need the remaining matrix work.

The three-adapter tests preserve indexed reference reuse with nonzero base vertex,
VS instance selection with nonzero start instance, DS/GS output, DrawAuto's original
SO cursor count, artifact corruption detection, stale request rejection and byte
ownership. Passing those tests does not close the gaps above or authorize removal
of the legacy mesh backend.

## Event-storage history milestone

```text
FloraGPA.Cli analyze frame.gpa_frame history --history-scope events --resource 80 --x 1280 --y 720 --gpu-vendor 0x10de --out history
```

This explicit scope exposes schema-2 `pixel_event_history` with GPA identities.
Calling `history` without the scope still fails until fragment-history parity is
implemented. The existing Pixel History GUI remains available on its original
backend; this milestone does not replace or reduce its successful capabilities.

One complete replay records selected mip/layer/slice/sample storage before and
after candidate writes. CPU Map/Update, Clear, Copy, Resolve, GenerateMips and
supported Discard commands use addressed regions. Draw/Dispatch use actual native
RTV/DSV/UAV bindings after experiment edits, with potential coverage. Disabled or
suppressed commands are marked unexecuted; an unchanged value never proves that
a GPU write did not execute. GPU execution, coverage, fragment identity, shader
outputs and failure reasons remain unknown. CPU writes with captured overlapping
bytes retain execution evidence even when the stored value is unchanged.

Records are explicitly `event_snapshot`, never fragments. Missing initial objects
remain unavailable without premature materialization. Discarded snapshots have
unavailable storage and false definedness. Other readbacks retain unknown
definedness: stored bytes alone cannot establish initialization or full coverage.
BC blocks, packed pixel groups and planar luma/shared chroma retain raw storage
without claiming an individual pixel change. Schema 2 adds typed color/depth/stencil
values while retaining the original raw storage. Interpretation comes from an
explicit compatible selection, a unique command/bound view, or an already typed
resource, in that order. Ambiguous or absent interpretations stay unknown. Each
snapshot records `value_format`, `value_format_source`, `decoding_status` and
`decoding_source`; unavailable/unsupported values are not synthesized.

Ordinary normalized values use ideal storage math. B5 and BC formats use a native
texture Load on an isolated device with the verified same adapter LUID. Those
snapshots retain float32 `component_bits` and `decoding_adapter`; B5 also retains
`storage_math_color` because observed NVIDIA/Intel loads differ from ideal math.
The helper never dispatches into the captured context. Typeless storage is not
silently interpreted as float. A typed format also selects the MSAA read path;
raw storage format and interpretation remain separate. See
`DX11_PIXEL_VALUE_RESEARCH.md` for the independent hardware probes and limits.

The Worker keeps the existing immutable request/context, artifact hashes, explicit
adapter, timeout, cancellation and module snapshot. Readbacks are bounded to
256 MiB each by default and results to 100,000 records; overruns fail, never return
truncated success. Cancellation is checked at replay/readback boundaries. These
limits do not guarantee aggregate traffic, latency or peak auxiliary GPU memory.
Ordinary single-sample color storage uses a one-texel GPU copy and a reusable
staging resource. Source format and texture dimension are retained; source
mip/layer/slice coordinates are explicit. The copy temporarily disables predication
and preserves affected output bindings, including WARP's array-mip workaround.
Raw NaN and negative-zero bit patterns remain unchanged; the separate numeric
interpretation uses nonfinite strings and preserves negative zero.

Depth/stencil, MSAA, compressed, packed and planar layouts keep the complete
storage reader. This respects the whole-subresource requirement for depth and
MSAA in the [D3D11 CopySubresourceRegion contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion).
No failed copy is silently retried with another device. Result snapshots expose
`readback_scope`, and totals include `point_readback_count` and packed bytes copied
to CPU output; these are not measured PCIe traffic or physical allocation sizes.
Unsupported storage formats still fail explicitly; this is not full format parity.
Special-format optimization and broader format, alias/lifetime, sparse Map,
deferred-context and workflow validation remain open.
Existing replay rejection of unproven rectangle/array-view combinations is retained.

See `DX11_NATIVE_BASELINE.md` for tests, real-capture checkpoints, traffic and exact
reproduction commands. P4 and P5 are not closed by this event-storage milestone.

## DXBC execution checkpoint

`DxbcExecution` now provides a project-owned CPU execution core for original
SM4/SM5 VS/PS/CS instructions, raw register bits, validity, word offsets, return
stacks and reversible changes. It handles basic arithmetic, conversions, masks,
dynamic indices and structured control flow. Unknown control/address values and
unsupported instructions fail without committing the instruction. It does not
replace any production debug action yet.

The CPU-only conformance checkpoint had 357 passes and 17 failures, with no skips.
Those differences include immediate conversion/borrow behavior and Intel
destination aliasing. Same-adapter VS instruction assistance now resolves all
17 original cases with unchanged exact assertions. The original 374-case suite
passes, and the expanded suite passes 375 cases with no failures or skips.
Additional lifecycle validation and the retained prior differences are
documented in `DX11_DXBC_GPU_INSTRUCTION_RESEARCH.md` and its evidence ledger.
This does not establish equivalence for all shader contexts or other stages.
No historical baseline failure category or test skip was expanded. Private raw
and structured SRV/UAV snapshots now support loads, stores, view aliases and
reversible memory changes with per-byte validity. The separate CS buffer suite
previously had 119 passes and 6 extreme-address failures across three devices.
Same-adapter private GPU assistance now resolves those cases with unchanged exact
assertions; the expanded suite passes 246 cases, including two assistance modes,
reference-context binding checks and cancellation/recreation. Its complete output
comparisons include backing bytes outside the selected view. See
`DX11_DXBC_GPU_MEMORY_RESEARCH.md` for provenance, bounds and remaining limits.
This CS feature-level-11.0 evidence is not full stage or feature-level parity.
The shared private device helper is now named `GpuOperations`, with backend
identity `dxbc-gpu/5`; instruction and buffer passes retain separate trace flags.
The new `Group` core executes complete declared CS thread groups with private
raw/structured groupshared storage, 32-bit memory atomics and dynamic barrier
rendezvous. Its explicit `serial-round-robin/1` schedule is simulated, not recovered
from the captured GPU. Thirty hardware cases across WARP, NVIDIA and Intel plus
eight setup/CPU validation cases pass; atomic tickets use permutation constraints
and all other deterministic bytes use exact comparison. See
`DX11_DXBC_GROUP_RESEARCH.md` for validity, cancellation, trace reversal and limits.
Hidden counter allocation/consumption now uses private per-view state and reversible
counter changes. Native replay supplies typed counter snapshots with scoped event
overrides and explicit unavailable values. Forty counter cases pass, including
thirty GPU cases across WARP, NVIDIA and Intel, plus snapshot restoration and
cancellation checks. See `DX11_DXBC_COUNTER_RESEARCH.md` for Append identity limits
and the distinction between simulated and captured ordering.
Native buffer inputs now come from an observed replay boundary inside scoped
experiments: actual CB windows, raw/structured SRV/UAV views and hidden counters
share one event state. The 42-case suite validates execution from those snapshots
and cancellation restoration. See `DX11_DXBC_INPUT_RESEARCH.md` for identity,
independent-replay provenance and explicit unsupported resources.
CS typed buffer loads/stores now use private same-adapter GPU format conversion;
snapshots retain native format and subrange identity. The independent CPU executor
preserves per-component validity, full backing storage and reversible writes,
including writes whose bytes do not change. See `DX11_DXBC_TYPED_BUFFER_RESEARCH.md`
for the exact tested formats, operand forms and remaining stage restrictions.
VS typed SRV loads now use a native VS plus Stream Output on the isolated adapter,
preserving the executing stage instead of using CS results. The 580-case VS suite
passes, including 576 exact hardware comparisons and validity/lifecycle checks.
See `DX11_DXBC_TYPED_VERTEX_RESEARCH.md` for stage/model limits and trace evidence.
PS typed SRV loads now use an isolated native PS and integer render target, with
an occlusion query verifying one covered sample. The 580-case PS suite passes,
including 576 exact hardware comparisons and reference state/resource checks.
See `DX11_DXBC_TYPED_PIXEL_RESEARCH.md`; this does not yet supply captured pixel
inputs, quad/helper-lane semantics or full fragment debugging.
Complete texture execution and GPU sampling assistance, vertex/pixel input
extraction, quad/helper execution, full CS group/cross-group semantics,
source/GUI integration and full parity remain required P6 work.

Native texture inputs now accompany buffer observations in the same scoped event
snapshot. The 76-case texture suite covers packed mip/layer/depth storage, native
SRV/UAV ranges, alias deduplication, input/output edits, fractional MinLOD and
restoration after budget failure or cancellation across WARP, NVIDIA and Intel.
Real GF2, BF1 and Helldivers inputs have also been observed. See
`DX11_DXBC_TEXTURE_INPUT_RESEARCH.md` for exact scope and evidence. Input observation
alone does not establish texture instruction execution; the next checkpoint below
covers non-MSAA loads. MSAA storage, sampling and production debugger cutover
remain pending.

Non-MSAA `ld` now executes for 1D/2D/3D textures and 1D/2D arrays using the native
typed-format mapping and same-adapter PS/VS/CS assistance. The CPU preserves
coordinates, offsets, view-relative mip/layer selection, MinLOD bounds and
component validity. See `DX11_DXBC_TEXTURE_LOAD_RESEARCH.md` for the native CS
reference matrix, recovery checks and remaining format/performance limits.
The subsequent format extension adds BC1 through BC7, sRGB, BGRA/BGRX, packed
color, shared-exponent RGB, alpha-only and depth/stencil SRV loads. Its separate
texture layout tracks component validity without changing typed-buffer execution.
See `DX11_DXBC_TEXTURE_FORMAT_RESEARCH.md` for native comparisons and conservative
compressed-block validity. Texture UAV operations, MSAA, sampling and packed/planar
video access remain required before full texture execution parity or production
debugger cutover. The optional input-probe group execution mode reports bounded
real-CS progress without claiming native-output parity.

Texture `resinfo` now uses separately observed native metadata, including MSAA
descriptions without inventing pixel storage. Integer/float view queries execute
in the CPU interpreter; reciprocal, out-of-range and 3D UAV cases use the isolated
same-adapter helper to preserve observed driver behavior. Source identity is
`independent-replay-resource-inputs/4`, and the helper identity is `dxbc-gpu/8`.
See `DX11_DXBC_RESOURCE_INFO_RESEARCH.md` for exact comparisons, independent HLSL
probes, unknown-field handling and the remaining debugger integration requirements.

Compute texture UAV loads/stores now use the independent executor and private
same-adapter helper `dxbc-gpu/9`. Texture changes retain before/after bytes and
validity, including unchanged-value writes, and support checked undo/redo.
Bounded private storage caches resynchronize after CPU rollback or alias writes.
See `DX11_DXBC_TEXTURE_UAV_RESEARCH.md` for native format conversion, the observed
3D view discrepancy, cooperative-thread tests and exact remaining scope.
The real Helldivers event 893 single-dispatch probe completes and matches native
observed output bytes. BF1 event 209 completes the selected group but has multiple
dispatch groups, so its report explicitly does not claim whole-dispatch parity.
Neither result establishes production debugger or full release acceptance.

MSAA `ld_ms` now reads immutable, typed sample observations in the independent
VS/PS/CS executor. Input snapshot identity is `independent-replay-resource-inputs/5`.
The selected replay adapter supplies sample bits through a private copy and bounded
readback; no resolve, vendor substitution or original GPA module is used. Undefined
sample indices remain unknown. The four-sample hardware matrix and independent
stencil probe are documented in `DX11_DXBC_MULTISAMPLE_RESEARCH.md`, including the
locally observed Intel alpha discrepancy and remaining coverage/performance limits.
This supersedes the earlier checkpoint's MSAA-input limitation, while full P6
debugger parity and P5/P7/P8 gates remain open.
The follow-up also recognizes count-1 MS views by their actual SRV dimension.
Independent sample-query research records the accepted count-1 load behavior
and an unresolved Intel rasterizer/SRV center-position discrepancy in
`DX11_SAMPLE_QUERY_RESEARCH.md`. The follow-up query executor supports
`sampleinfo` and `samplepos` using separate native SRV/rasterizer observations,
input identity `independent-replay-resource-inputs/6` and input-probe schema 5.
The 375-test suite includes six CS/native-dispatch comparisons. No-target
rasterizer values remain explicitly unknown: independent probes found
order-dependent behavior on the local Intel and NVIDIA devices. Its 72
unknown-propagation cases do not close that acceptance item. See
`DX11_DXBC_SAMPLE_QUERY_IMPLEMENTATION.md` for the exact supported matrix and
remaining forced-count, depth-only, stage and integration scope.

Sampler observations now accompany resource snapshots under input identity
`independent-replay-resource-inputs/7` and input-probe schema 6. All 16 stage
slots retain effective GPA identities, actual descriptor bits and explicit
unbound/experimental state. Native comparisons recheck those descriptors on
both replay instances. The 82-test input suite covers sampler alias identities,
experiments, budget recovery and three subprocess comparisons. See
`DX11_DXBC_SAMPLER_INPUTS.md`. Independent explicit-LOD research records 6,426
observations and two adapter-specific filtering/clamp differences in
`DX11_SAMPLE_LEVEL_RESEARCH.md`; the sampler snapshot checkpoint does not yet
implement sampling opcodes or close full shader-debugging acceptance.

The subsequent explicit-LOD checkpoint adds `sample_l` to the independent
VS/PS/CS executor. It uses observed sampler inputs and private same-adapter
operations under helper identity `dxbc-gpu/10`. It preserves raw filtered values,
view mip/array ranges, resource clamps and result masks. See
`DX11_DXBC_SAMPLE_LEVEL.md` for the independent execution matrix, synthetic GPA
captures, cancellation recovery and remaining validity/performance gaps.
This supersedes the sampler-input checkpoint's lack of sampling execution;
implicit sampling, gradient/comparison variants, quad/helper semantics and full
debugger integration still block complete P6 acceptance.

Explicit-gradient sampling now has single-invocation VS/PS/CS assistance and a
coherent CS batch path under `dxbc-gpu/12` and `serial-round-robin/2`. Independent
WARP observations required retaining invocation context: the group path preserves
original flat indices, group dimensions and participation, then commits trace
steps only after readback. The complete gradient suite passes 262 cases. See
`DX11_DXBC_SAMPLE_GRAD.md` for exact same-context comparisons, retained failures,
cancellation checks and remaining divergence/quad/format/performance scope.
This supersedes the earlier checkpoint's lack of gradient execution but does
not close full shader-debugger or release parity.

## Delivery constraints

The subsequent compute sampling checkpoint uses
`bounded-sampling-rendezvous/3`. It gathers original flat invocation indices at
matching dynamic instructions and drains staged results before later writes.
Independent output comparisons cover explicit branches, staggered arrival,
early returns, uneven loops, barriers and four group dimensions. A bounded wait
also permits progress when a consumer polls a flag written after a sample.
See `DX11_SAMPLE_GROUP_RESEARCH.md`. This resolves the observed divergent WARP
sampling mismatches; arbitrary long divergence and complete VS/PS/CS debugging
remain unaccepted. Existing commands and product integration retain the staged
migration status described above.

GPU tests run serially. Final validation must include NVIDIA, Intel and applicable
WARP paths, clean capture-only/replay-only/capture-full builds, runtime module and
child-process auditing, bounded time/memory, and an installation without either
RenderDoc or GPA. No golden output or historical support limit may be weakened to
pass these gates. Main is not merged automatically.

All implementation commits use Author and Committer
`Floraison <181715567+LesFloraison@users.noreply.github.com>` and English messages.

## Comparison sampling continuation

`dxbc-gpu/13` adds native `sample_c_lz` execution in VS, PS and CS. It preserves
comparison filtering, reference bits/modifiers, mask semantics and observed
sampler/view/resource LOD controls. Missing comparison samplers are rejected as
undefined instead of accepting incidental driver output. The R32_FLOAT 2D
matrix covers all eight comparison functions, both invocation modes, NaN and
infinite references, offsets and addressing variants. The executor comparison
suite passes 190 tests, including 162 complete CS programs and cancellation.
See `DX11_DXBC_SAMPLE_COMPARISON.md` and the independent research ledger.
Additional shapes/depth-compatible formats, implicit comparison sampling and
complete captured shader/debugger integration remain separate acceptance gates.

## Pixel derivative execution continuation

`PixelQuad` and `dxbc-gpu/14` execute four CPU lanes with native selected-adapter
derivatives. Independent full-program comparisons cover reconvergence, unequal
loops, scissored helper coverage and post-discard arithmetic. The 1,022-case suite
passes, including raw bits, partial validity, bounded waits and cancellation.
See `DX11_DXBC_PIXEL_QUAD.md`. Captured quad provenance, implicit sampling, helper
memory semantics and debugger presentation remain open; this checkpoint does not
close P6 or permit removal of the existing debugger backend.

## Implicit pixel sampling continuation

`dxbc-gpu/15` and `coherent-pixel-sampling/2` add actual native `sample`, `sample_b`
and `sample_c` instructions to the CPU pixel quad scheduler. The R32_FLOAT 2D
matrix passes 9,408 complete shader scenarios, including 192 explicit undefined
comparison-sampler rejections, and 19 semantic/recovery checks. The implementation
preserves per-pixel operands, unknown validity, trace changes and private snapshots.
See `DX11_DXBC_PIXEL_SAMPLING.md`. Additional shapes/formats, captured invocations,
helper memory behavior and full debugger/source integration remain required.

## Native completion continuation

`dxbc-gpu/16` uses bounded private fence events where supported and retains native
polling otherwise. All staging and query readbacks share an operation deadline;
failure poisons the helper. Both paths pass cancellation, handle lifetime,
multi-subresource and VS/PS/CS checks. The 22 affected suites pass 7,958 tests;
captured BF1 group execution and Helldivers single-dispatch resource comparison
also pass. See `DX11_DXBC_GPU_COMPLETION.md`. This checkpoint does not enable the
currently reserved native debug actions or close any full debugger/history or
standalone release gate.

## Native execution journal continuation

`flora.dxbc-execution-trace` version 1 adds bounded, atomically published CS group
journals with project-owned types and reversible register/resource/counter state.
The independent validator reconstructs and rewinds real captured group execution;
516 affected tests and ten negative journal checks pass. See
`DX11_DXBC_EXECUTION_TRACE.md`. Invocation eligibility and Worker/UI integration
are separate required steps; no existing debug action is switched or removed by
this checkpoint, and no full debugger/history or release gate is closed.

## Native compute Worker continuation

The native `debug-thread` CLI action now produces a checked invocation report
and project-owned execution journal through the existing Worker. It supports
actual direct/indirect arguments, scoped buffer/shader experiments and known
replay predication, with explicit errors for unknown hint eligibility. The
seven affected suites pass 263 tests after restoring original-corpus coverage;
real BF1/Helldivers CLI jobs and eleven negative result checks also pass.
See `DX11_NATIVE_COMPUTE_DEBUG.md`. The debugger UI, complete shader/fragment
semantics and P7/P8 gates remain open; this does not retire any legacy UI feature.

## Native trace reader continuation

`ExecutionTraceReader` validates the accepted journal digest and complete state
chains, retains a Windows read lock, and supports reversible whole-group state
projection with explicit selected-thread boundaries. It preserves raw bits,
validity, exact source records and unknown constant input. Four affected suites
pass 188 tests; BF1 and Helldivers initial/final/selected-boundary states match
the independent Python projector. See `DX11_DXBC_TRACE_READER.md`. This is the
native debug model's state layer; legacy debug UI and full removal gates remain
open.

## Native compute session and UI continuation

The parallel `Native CS` tab now loads verified Worker results asynchronously,
retains a locked session journal, displays native stepping/source/watch state,
rejects stale selections and exports a complete session archive. Six affected
suites record 65 passes, no failures and five unchanged external-corpus skips;
MainWindow execution/cancel/retry passes on NVIDIA, Intel and WARP. Recorded
BF1/Helldivers jobs and a source fixture pass independent archive/reload checks.
See `DX11_NATIVE_DEBUG_SESSIONS.md` for exact scope and remaining UI/semantic
gates. Legacy tabs and RenderDoc dependencies remain until full parity is proved.

## Cancellable native debug operations

Interactive native stepping, source searches and session export now run in a
background controller. Snapshot presentation avoids trace seeks on the UI
thread; cancelled operations retain the previous display and can be retried.
Source encounter caches publish atomically, and ZIP cancellation preserves the
old destination. Nine suites record 95 passes, no failures and five unchanged
optional corpus skips. Actual BF1/Helldivers journals remain responsive during
movement and export and pass independent archive reloads. See
`DX11_NATIVE_DEBUG_OPERATIONS.md` for measurements and remaining parity/P8 gates.

## Native execution model continuation

The native model now connects shader-verified journal state to source values,
navigation, watches, conditional/hit-count and instruction breakpoints, and
native configuration. Worker jobs export the actual experiment shader. Nine
affected suites record 231 passes, zero failures and five existing optional UI
corpus skips. BF1/Helldivers model and state checks and fourteen result mutations
pass. See `DX11_NATIVE_EXECUTION_DEBUG_MODEL.md`. UI dispatch/presentation,
remaining source/shader/fragment semantics and standalone removal gates remain
open.

## Native vertex input continuation

The independent VS input path now retains the effective input signature and
uses a private device on the exact adapter to observe IA-converted raw registers.
It supports full 32-register signatures without spare UAV/output slots and
connects actual GPA bindings and experiment edits to the C++ executor. GF2 and
BF1 execute selected VS inputs; Helldivers input acquisition succeeds while
execution rejects an unsupported operand extension. GF2 native output comparison
is exact; BF1 floating differences remain unresolved. See
`DX11_NATIVE_VERTEX_INPUTS.md` and its evidence ledger. Whole-draw acquisition
budgets, remaining semantics, VS session/UI integration and full removal gates
remain open.

## Native arithmetic precision continuation

Minimum-precision arithmetic now uses the selected adapter through private
VS/PS/CS helpers, and VS output destinations retain their precision-bearing
signature. Native VS dot/multiply-add assistance closes the selected BF1 float
differences. Helldivers' minimum-precision operand extension now executes, with
all 13 output components matching exactly on NVIDIA, Intel and WARP. All nine
GF2/BF1/Helldivers comparisons match, and ten suites record 2,762 passes without
failures or skips. See `DX11_DXBC_PRECISION.md` for the bounded scope, rejected
precision combinations, validation order and evidence ledger. VS journals and
UI dispatch, remaining shader/fragment semantics and P7/P8 remain open.

## Native vertex debug job continuation

The native Worker and CLI now expose `debug-vertex`, with explicit draw-relative
input reference/instance identity, bounded reversible journals and effective
shader artifacts. The native VS/CS tab uses the shared source, watch, breakpoint,
session and background-operation paths. Nine suites pass 296 checks; nine real
capture jobs pass exact register/output comparisons and session reloads. The
initial CS preparation regression was caught and corrected without changing
existing assertions. See `DX11_NATIVE_VERTEX_DEBUG.md`. Arbitrary index override,
selected-only input acquisition, broad VS/source parity and the remaining PS,
fragment history and P7/P8 gates still prevent complete removal acceptance.

## Selected vertex input continuation

Native VS jobs now submit only the selected input reference and instance to the
private acquisition helper. Per-instance rates are rebased independently, with
explicit draw-relative system ID provenance; original full-draw acquisition
remains a comparison path. Seven suites pass 335 checks without failures or
skips, and nine real capture/adapter jobs retain exact input/final register and
native output comparisons, module audits and session reloads. See
`DX11_SELECTED_VERTEX_INPUTS.md`. Complete bound buffer snapshots, arbitrary
index override, broader semantic/source parity and P7/P8 acceptance remain open.

## Native vertex index override continuation

VS Worker/CLI jobs and the native view now accept an effective per-vertex
attribute index override. System IDs and instance inputs retain original draw
selection, and version 2 results distinguish original and overridden addresses.
Old version 1 sessions retain their prior semantics. Ten suites pass 446 checks;
nine identity-override real jobs retain exact native output comparisons and
session/module checks, and three old real sessions load and rewind. See
`DX11_VERTEX_INDEX_OVERRIDE.md`. Selected cut/missing/out-of-range index behavior,
complete semantic/source parity, fragment history and P7/P8 remain open.

## Pixel quad input transport research continuation

Four-lane raw register transport now has an independent no-PS-UAV GPU proof:
384 cases, 297,024 exact neighbor components and 18 rejected evidence corruptions
across NVIDIA, Intel and WARP. Measured lane direction avoids treating even
screen-coordinate alignment as a contract. This establishes a transport method,
not captured PS inputs, original fragment identity or completed pixel debugging.
See `research/DX11_PIXEL_QUAD_TRANSPORT_RESEARCH.md` for reconstruction, scope,
the retained signed-zero diagnostic failure and the next integration gates.

## Selected-pixel native acquisition continuation

Private native raster acquisition now joins the validated geometry bridge and
original PS input declarations. It retains target/view/sampling state, owns its
record UAV independently of application bindings, and reports empty or ambiguous
pre-depth observations explicitly. The input suite passes 125 checks, including
cancellation/retry and array-view contracts; 1,190 quad/sampling regressions also
pass. Nine real GF2/BF1/Helldivers capture/adapter runs observe 33 input registers.
See `research/DX11_PIXEL_RASTER_INPUTS.md` for exact comparisons and reproducible
evidence. Original fragment eligibility, cross-register ambiguous-record identity,
input-free PS invocation selection, full history/debugger parity and standalone
RenderDoc removal remain acceptance gates.

## Native pixel execution journal continuation

Selected private raster inputs now connect to reversible four-lane PS execution
journals and the native debug model, including helper state and PS-specific
configuration identity. GF2/BF1 execute and rewind on all three adapters; the
Helldivers minimum-precision sample instruction remains a failed parity gate.
See `research/DX11_PIXEL_EXECUTION_TRACES.md` for tests and independent projection
evidence. This does not switch the public pixel-debug job or retire its existing
backend. Fragment qualification, complete semantics, source/UI parity, history
and standalone removal acceptance still require implementation and validation.

## Typed-buffer precision continuation

`dxbc-gpu/19` preserves destination/address precision and refactoring permission
for native typed-buffer reads. The 5,760-case GPU comparison covers VS, PS and
CS SRVs plus CS typed UAVs on three adapters, with 23,040 exact output components.
Existing typed-buffer/VS/execution checks pass. GF2/BF1 traces still complete;
Helldivers advances to the remaining minimum-precision PS output restriction.
See `research/DX11_TYPED_LOAD_PRECISION.md` for scope, retained failed attempts,
raw evidence and reproduction. This does not close P6 or remove the existing
RenderDoc backend; full history/debugger parity and P7/P8 remain required.

## Pixel output precision continuation

`dxbc-gpu/20` preserves single PS output precision and the original refactoring
permission for assisted arithmetic. Independent draws validate 3,024 output
cases, with compute, execution, typed-load and sampling regressions passing.
All nine GF2/BF1/Helldivers selected private quad journals now execute and rewind
on NVIDIA, Intel and WARP. See `research/DX11_PIXEL_OUTPUT_PRECISION.md` for
evidence and limits, including the large Helldivers journal and unavailable
source symbols. Original fragment qualification, remaining semantics, public PS
sessions, source/UI parity and complete history remain required before P7/P8.

## Original debugger GPU-selection research continuation

Ghidra and an independent predicate probe establish the original GPU-ID layout;
research probes now compare the requested enumeration record with the actual
replay device. Ten explicit NVIDIA/Intel lifecycles complete with null debugger
objects. Two Helldivers/Intel lifecycles hit the unchanged watch deadline and
remain failed evidence, with their distinct replay/teardown phases preserved.
See `research/DX11_DEBUGGER_EXPLICIT_GPU.md`. Explicit Intel selection does not
recover the debugger engine or close P1, and this research adds no GPA product
dependency.

## Pixel helper memory continuation

`coherent-pixel-memory/3` executes helper SRV reads and suppresses inactive UAV
accesses while preserving undefined return validity and prior writes. Independent
NVIDIA, Intel and WARP draws validate 600 coverage/discard/memory/counter cases;
four serial suites pass 2,141 checks. New GF2/BF1 journals and an unchanged older
GF2 journal load and rewind. See `research/DX11_PIXEL_HELPER_MEMORY.md` for scope,
retained fixture compilation failures and reproducible evidence. Original fragment
qualification, active typed-UAV and remaining precision semantics, public PS jobs,
source/UI parity, full history and P7/P8 acceptance remain required.

## Original pixel history diagnostic continuation

Owned D3D11 draws and original requests now verify selected Pixel History status
values, the callback vectors and manager event identity. Four reference runs
validate 384 draws; eight original requests match same-GPU event pixel outputs.
The original diagnostic count is not a per-fragment record and does not faithfully
qualify scissor/sample-mask rejection in these cases. See
`research/DX11_PIXEL_HISTORY_DIAGNOSTICS.md`. P5 must preserve original coverage
independently of its diagnostic passes; full fragment extraction, public PS
integration and P7/P8 remain open.

## Native pixel occlusion continuation

An independent prefix replay now measures selected-region occlusion without
allocating another UAV slot. Tests cover original restrictions, unchanged writes,
MSAA/array slices, predication and cancellation. The owned twelve-draw fixture
passes 72 measurements and 576 corruption controls; three real capture smoke
measurements complete. Same-device full-draw controls retain unresolved
early-depth/forced-sample differences, so results remain raw diagnostic counts
with unknown original fragment eligibility. See
`research/DX11_NATIVE_PIXEL_OCCLUSION.md`. This is an internal P5 building block;
full fragment history, public PS integration and P7/P8 remain required.

## Batched pixel candidate continuation

Captured final geometry can now be rasterized in a single private range pass to
find candidate storage ordinals at a selected pixel/sample/layer. The 120-case
synthetic matrix matches 660 independent draw queries; 72 owned-capture checks
verify raw candidate records. Nine GF2/BF1/Helldivers probes complete with matching
ordinal sets across adapters. See `research/DX11_NATIVE_PIXEL_CANDIDATES.md` for
scope, budget limits and reproducible evidence. Candidate order and original
eligibility remain unknown; public history/PS cutover and P7/P8 remain open.

## Compact pixel shader output continuation

Private quad evaluation now returns typed color, depth and coverage outputs with
raw bits and validity, without retaining a full trace journal. The original
snapshot's mutable storage is copied for execution. Ninety independent GPU
fixture cases pass on three adapters; nine real capture probes complete, and a
GF2 result matches its selected-lane journal outputs. See
`research/DX11_NATIVE_PIXEL_OUTPUTS.md` for the 204-check regression evidence,
540 corruption controls, timing observations and reproduction commands.
Private outputs do not establish original fragment eligibility or output-merger
results. Full history, public PS/debug parity and dependency removal remain open.

## Private output merger continuation

Compact native shader outputs now feed a private D3D11 output merger on the
verified same adapter. It preserves raw color/depth storage and applies blend,
logical, depth/stencil, coverage and write-mask state to one single-sample cell.
Owned GPA fixtures compare that path with original shader draws on three
adapters; independent CPU checks cover depth, stencil and logical operations.
See `research/DX11_NATIVE_PIXEL_MERGER.md` for scope and reproducible evidence.
Missing depth observations remain unknown, and reference storage stays unchanged.
Early depth, multisample coverage, original fragment qualification/order and
public history/debug cutover remain required before P5/P6 and dependency removal
can be accepted.

## Multisample output merger continuation

The private merger now uploads and reads individual native samples, intersects
supplied raster coverage with the output-merger mask, and applies hardware
blend/depth/stencil state to 2x/4x fixtures. Its 144-case extension preserves the
546 single-sample cases. An independent D3D11 probe exposed a repeated typed
MSAA copy failure on the local Intel driver; replay-owned family storage now
starts typeless while captured typed interpretation remains intact. See
`research/DX11_NATIVE_PIXEL_MERGER_MSAA.md` for reproductions and explicit limits.
This completes neither original fragment qualification/order nor public PS
debugging, and does not close P5/P6/P7/P8 acceptance.

## Pixel merger coordinates continuation

The private output merger now preserves caller-supplied target extent and pixel
location. WARP fixtures demonstrate a spatial alpha-to-coverage difference that
the former 1x1 origin target could not reproduce. The extension adds 216 color
and depth/stencil coordinate cases on three adapters; the final combined suite
passes 909 checks with no failures or skips. Results expose a logical payload
budget and retain unknown original fragment eligibility. Single-sample D32S8
padding is explicitly invalid. See
`research/DX11_NATIVE_PIXEL_MERGER_COORDINATES.md` for evidence and reproduction.
Public history/debug integration, full parity and P7/P8 removal remain open.

## Primitive identity continuation

Native pixel geometry now carries validated shader primitive IDs from original GS
output or private system-value SO passes. Tessellation uses a DS patch-ID helper:
an independent D3D11 probe reproduces a conflicting inserted-GS result on WARP.
Pixel bridges and candidates consume the sidecar without inferring instance or
fragment eligibility. Upstream UAV ambiguity, including reflection-stripped
declarations, remains explicit. See `research/DX11_NATIVE_PRIMITIVE_IDENTITY.md`
for the 108-case matrix, real captures, independent runtime controls and remaining
gates. Public pixel debugging, complete history and dependency removal are pending.

## Original pixel shader geometry replay continuation

Private geometry ranges now execute the unmodified PS with original resource,
class, rasterizer and output-merger bindings. Consecutive checkpoints retain
private color/depth/UAV writes while restoring original resources, hidden UAV
counters and SO cursors. Seventy-eight data cases pass on three adapters, and
nine real-capture probes match primary storage and original-draw queries on the
same adapter. See `research/DX11_NATIVE_PIXEL_GEOMETRY_REPLAY.md` for evidence,
query differences between local adapters, payload/deadline limits and the
remaining fragment-qualification boundary. This internal diagnostic does not
close full history, public PS debugging or P7/P8 acceptance.

## Selected-pixel primitive storage continuation

Candidate observations now join to checkpoints from the same captured geometry,
with complete private drawing between candidates. Typed selected-texel records
preserve before/after values, sample and subresource identity, unchanged values
and explicit unknown qualification/failure fields. The extension covers array
mips, 3D slices, masked MSAA storage samples and forced raster-sample unions.
See `research/DX11_NATIVE_PIXEL_DRAW_HISTORY.md` for 66 new data cases, 501
corruption controls and nine real-capture comparisons. These private primitive
storage records do not yet replace public fragment history or PS debugging.


## Checkpoint inputs and typed atomic continuation

Current-bound resource snapshots now read private geometry checkpoints without
starting another replay. Snapshot acquisition scope is retained in compact pixel
outputs and execution journals; replay generation alone does not identify the
resource values at a checkpoint. Typed R32 integer atomics now execute on buffer
and texture storage, with isolated atomic footprint observation for ambiguous 3D
view bounds. The tests retain unordered native draw results and use independently
serialized original draws for exact checkpoint comparisons. See
`research/DX11_NATIVE_PIXEL_CHECKPOINT_INPUTS.md` for 294 new data cases, 2,337
combined passing checks and the remaining raster-input/output integration gates.
Full fragment history, public PS debugging and P7/P8 acceptance remain open.


## Private history shader-output continuation

Actual raster quad inputs now join current private primitive checkpoints and the
independent DXBC executor. Constant and coverage-only shaders are supported;
forced raster samples preserve separate acquisition provenance when equivalent
pixel-frequency invocations are merged. Shader results remain explicitly private
and do not qualify original fragments. See
`research/DX11_NATIVE_PIXEL_HISTORY_SHADER.md` for 72 new data cases, 663 passing
regression checks, nine real-capture probes, 474 corruption controls and measured
cost. Complete fragment history, public PS debugging and P7/P8 acceptance remain
open, and the GPA debugger host +0x3c0 path still needs independent closure.


## Active PS typed UAV continuation

The independent executor now supports active PS typed buffer and texture UAV
loads/stores through isolated, same-adapter PS assistance. Helper lanes retain
suppressed accesses and unknown results; stage-specific 3D footprint identities
and reversible memory records are preserved. The new 324-case matrix compares
native PS output and complete storage across six dimensions, six formats and
three address modes on three adapters. See
`research/DX11_NATIVE_PIXEL_TYPED_UAV.md` for validation, corrected normalized
resource declarations and remaining coverage limits. Public PS debugging,
fragment qualification, the GPA debugger implementation and P7/P8 remain open.

## Original host initialization continuation

The Ghidra investigation now maps three host-owner aliases, the D3D11 manager
initializer and concrete fallback query factory. Six original-code synthetic
cases and four serial GF2 lifecycles verify the alias writes, tables and reverse
links; the candidate engine remains null at all 20 sampled live boundaries.
See `research/DX11_DEBUGGER_HOST_INITIALIZATION.md` for addresses, retained probe
corrections and evidence limits. These manager implementations do not identify
the debugger engine or close P1. Product behavior is unchanged by this research.

## Original debugger alias-store continuation

Ghidra scans now inspect offsets relative to the three known host subobjects.
State-block, manager and stack matches are distinguished from the candidate
engine field using disassembly, concrete tables and six guarded original-code
state-block lifecycles. Thirty corrupted memory reports are rejected. See
`research/DX11_DEBUGGER_HOST_ALIAS_STORES.md` for the bounded search, corrected
caller edge and retained evidence. The debugger's non-null assignment and
implementation remain unresolved; this does not enable public pixel debugging.

## Native Replay Mesh preview continuation

Replay Mesh now offers explicit native GPA analysis through the shared Worker
controller, with retained position/identity artifacts, CSV/OBJ/JSON exports,
stale-context rejection and cancellation/retry. Nine VS/GS/pure-DS GPU cases pass
across three adapters; three UAV instance-selection cases retain the existing
explicit rejection. See `research/DX11_NATIVE_REPLAY_MESH_UI.md` for the independent
validator, original failed runs, corrected counter regression and legacy fixture
skips. Default cutover and full P3/P7/P8 acceptance remain open.

## UAV-independent mesh instance continuation

The original hull counter/store fixture now supports per-instance native capture
after a bounded DXBC liveness proof and actual input-resource alias checks. A GS
fixture with different output counts per instance validates nonuniform boundaries.
All selected bytes come from the full capture; private prefix draws provide only
counts. See `research/DX11_NATIVE_GEOMETRY_UAV_INDEPENDENCE.md` for 113 passing
regression checks, 108 rejected evidence corruptions, retained preliminary failures
and the remaining read/alias/control-flow gates. Default backend cutover remains
pending full baseline parity.

## Immutable geometry resource-read continuation

Native instance proofs now track SRV/UAV reads and compare their actual owners
with the union of written UAV owners across all geometry stages. Independent and
unbound reads are supported; feedback remains rejected. Compiler probes cover
structured/raw/typed buffer operands and sampling extensions, and the new GS
paths run through the Worker and native mesh UI. See
`research/DX11_NATIVE_GEOMETRY_RESOURCE_READS.md` for 141 passing checks, 186
rejected evidence corruptions, the corrected feedback fixture and remaining gates.
This expands P3 coverage without declaring default cutover or P7/P8 complete.

## Real-capture mesh validation continuation

Twenty-one GF2/BF1/Helldivers selection/adapter combinations now pass through
42 native Worker jobs and 21 actual VS mesh views. An independent triangle-list
assembly check compares all output attribute bytes with final SO storage on the
same adapter, including first/last instance selection and BF1 zero-W output.
See `research/DX11_NATIVE_MESH_REAL_CAPTURES.md` for 84 rejected corruptions,
module snapshots, elapsed time and memory measurements, and retained initial
validator failures. This is selected native-path consistency evidence; baseline
RenderDoc export parity, remaining P3 cases and default cutover remain open.

## Indexed mesh presentation continuation

Native Replay Mesh now preserves compact input identities in its table and
CSV/OBJ exports, with full-byte output variants and explicit source/reference
maps. Five of seven selected NVIDIA frozen-main exports now match exactly;
all seven have identical indices and referenced vertex records. Two strict
differences remain because the baseline instance-zero export also contains later
instances; additional native all-instance jobs reproduce those entire buffers.
See `research/DX11_NATIVE_MESH_INDEXED_PRESENTATION.md` for the 35 passing UI
checks, 21 real adapter/selection combinations and the still-failed strict gate.

## Native double instruction continuation

The owned DXBC executor now supports 19 double instructions with paired register
validity, reversible changes and arithmetic on the explicitly selected adapter.
`dxbc-gpu/21` checks both basic and extended hardware capabilities. Local evidence
contains 528 independent GPU comparisons and 264 Intel capability rejections,
with 5,772 rejected report corruptions. Seven regression entries pass 2,540 checks.
See `research/DX11_NATIVE_DXBC_DOUBLES.md` for raw/compiled shader coverage,
retained fixture failures, replay instructions and evidence limits. Source/type
parity, captured pixel qualification, full history and P7/P8 acceptance remain open.

## Double source workflow continuation

A public Worker probe exposed missing GPU assistance for ordinary double compute
snapshots; snapshots now honor the double capability declaration. The native GUI
also supports paired double register interpretation with strict pair validity.
WARP/NVIDIA Worker and CLI jobs retain precise source/watch values, conditional
breakpoints, independent GPU output and cross-process ZIP restoration. Six suites
pass 244 checks and the evidence validator rejects 18 corruptions. See
`research/DX11_NATIVE_DOUBLE_DEBUG_WORKFLOW.md` for the retained integration failure,
compiler fixture investigation and remaining full debugger/removal gates.

## Depth-only private merger continuation

The private output merger now accepts a DSV without an RTV and requires only
shader outputs consumed by bound targets or alpha-to-coverage. Independent
original draws cover 432 depth-only cases across WARP, NVIDIA and Intel,
including MSAA, stencil, read-only views and same-value depth writes. The full
suite passes 1,341 checks without failures or skips; five report validators reject
17,514 corruptions. See `research/DX11_NATIVE_DEPTH_ONLY_MERGER.md` for the
reproduction commands and evidence limits. Original fragment eligibility,
complete Pixel History, debugger parity and P7/P8 removal gates remain open.

## Measured raster input continuation

Private pixel records now preserve selected `SV_Position.z`, front-face state
and coverage through shader-output/history provenance. System-input rewriting
also handles compiler-packed sample-index/front-face components correctly.
Five suites pass 1,208 checks; 96 independent raster probes and nine real
capture/adapter history selections pass, with 1,824 rejected corrupt reports.
See `research/DX11_NATIVE_RASTER_MERGER_INPUTS.md` for the retained initial
failures, template disassembly, reproduction and limits. Actual state extraction
and complete merger/history qualification remain open alongside P7/P8.

## Live output-merger state continuation

Native merger snapshots now read actual RTV/DSV bindings, typed subresources,
blend/depth state and selected MSAA storage with bounded, cancellable readback.
The complete matrix passes 1,353 checks with no failures or skips; six validators
reject 29,610 corrupted reports, and four related replay test entries pass.
Independent D3D11 state creation distinguishes authored descriptors from actual
disabled-stencil fields without weakening numerical or historical checks. See
`research/DX11_NATIVE_PIXEL_MERGER_SNAPSHOTS.md` for retained failures, the
interrupted run, precision limits and reproduction commands. Complete merger
integration, original fragment eligibility and P7/P8 acceptance remain open.

## Private shader and merger integration continuation

Private history can now join observed output bindings, measured raster inputs
and owned PS execution at each before-primitive checkpoint, then compare the
selected-sample merger result with actual private GPU storage. The complete
suites pass 868 checks; 102 owned and nine real reports validate, including 36
real primitive predictions and 30 blend/same-value-write integration cases.
Two complementary validators reject 2,061 corrupted reports. See
`research/DX11_NATIVE_PIXEL_MERGER_JOINS.md` for exact scope and reproduction.
Original fragment eligibility, full sample-depth and early-depth semantics,
public Pixel History/debugger parity and P7/P8 removal acceptance remain open.

## Original draw observation continuation

Private history now executes one complete original draw on isolated writable
resources, preserving all effective graphics shaders and original command
arguments. It separately compares selected storage and whole-output occlusion
with the geometry checkpoints. The complete geometry suite and strengthened
atomic integration pass 274 Qt checks; nine real selected pixels match ordinary
GPU replay. The new validator checks 279 reports and rejects 2,073 corruptions.
Four Intel atomic fixtures retain different final pixel indices between complete
draws and serialized checkpoints, while independent storage/counter constraints
pass. Equality of sample counts does not establish original invocation order.
See `research/DX11_NATIVE_ORIGINAL_DRAW_OBSERVATION.md` for measured differences,
reproduction, retained development failures and evidence hashes. Full fragment
qualification, public debugger parity and P7/P8 acceptance remain open.

## Private pixel completion continuation

An internal diagnostic can now retain the original PS body and mark one existing
private color output without extra UAV slots, input registers or temporaries.
The final completion and geometry suites pass 386 Qt checks; nine real selections
and 12 separate Intel dual-source repetitions also pass. Independent validators
check 402 reports and reject 2,646 corruptions. Exact storage, alpha, side effects,
other MRT results and recovery are verified; four final Intel query-contract
deviations and an unreproduced earlier dual-source count failure remain explicit.
See `research/DX11_NATIVE_PIXEL_COMPLETION.md` for reproduction, original logs,
independent controls and scope limits. A diagnostic marker does not establish
original fragment identity or ordering, and the helper does not yet qualify
public pixel history/debugging. P1, remaining parity and P7/P8 gates stay open.

## Original PS primitive predicate continuation

Complete original PS draws can now return an RGB predicate on their actual
`SV_PrimitiveID` without extra UAV slots or discarding nonmatching primitives.
The implementation preserves alpha and original work, and checks GS output/input
hardware locations when adding a system input. Three suites pass 416 Qt checks;
918 owned predicates and nine real selections validate. Three validators reject
8,082 corruptions. Raw typed storage changes are not treated as decoded identity
matches, and system IDs are not promoted into primitive ordinals or instances.
See `research/DX11_NATIVE_PIXEL_PRIMITIVE_PREDICATES.md` for retained failures,
reproduction and evidence. Original quad/fragment association, full history and
debugger parity, P1 engine recovery and P7/P8 acceptance remain open.

## Typed pixel identity association continuation

Private history can now decode actual typed RGB predicates and associate a
verified conditional system ID with all matching geometry candidates. Duplicate
GS IDs remain ambiguous. Three suites pass 420 Qt checks; 39 owned and nine real
selections validate through complete storage and merger joins. Four validators
reject 8,934 corrupted reports. Cancellation, retry, zero sample coverage and
deliberate equality-readback failure retain unknown or unverified results where
appropriate. See `research/DX11_NATIVE_PIXEL_IDENTITY_JOINS.md` for exact scope,
performance, reproduction and evidence. Original invocation qualification,
complete history/debugger parity and P1/P7/P8 gates remain open.

## Private draw history CLI and Worker continuation

`analyze history --history-scope private-draw` now exposes the native storage,
shader, merger and conditional-identity observations through the existing isolated
Worker and context/artifact validation. Explicit options preserve the difference
between private replay observations and original fragment records. The new suite
and analysis/event-history regressions pass 157 Qt checks. Fifteen owned jobs and
nine real CLI jobs pass independent validation, with 90 corrupted results rejected.
See `research/DX11_NATIVE_DRAW_HISTORY_JOBS.md` for exact scope, measured time and
peak memory, module-snapshot limits and reproduction. Complete fragment/debugger
parity, GUI cutover and P1/P7/P8 acceptance remain open.

## Private primitive PS debug jobs and sessions continuation

`analyze debug-pixel --debug-scope private-primitive` now exports a reversible
quad journal from measured before-primitive inputs, with mutable resource copies
isolated from subsequent history observations. Verified native sessions and the
`Native execution` panel accept this explicit scope and preserve unknown original
fragment eligibility. The dispatcher also restores required before-work state
preparation for existing VS/CS jobs. Seven affected suites pass 214 Qt checks;
18 owned jobs and nine real jobs pass independent validation, with 189 corrupted
results rejected and all 27 archives successfully reloaded and re-exported.
See `research/DX11_NATIVE_PIXEL_DEBUG_JOBS.md` for retained failures, expensive
large-journal audits, performance measurements and reproduction. This does not
close original fragment selection, complete history/debugger parity, GPA engine
recovery or P7/P8 dependency-removal acceptance.

## Accumulated system-ID presence continuation

Private history now supports `--primitive-presence`: repeated complete original
PS draws accumulate RGB predicates so later nonmatching returns do not erase
earlier color completions. Distinct system IDs are observed twice and associated
with every matching candidate ordinal; duplicate GS IDs retain ambiguity and
original fragment eligibility remains unknown. Six suites pass 304 Qt checks.
The validators cover 45 owned GPU cases, three recovery cases, 18 owned jobs,
15 real probes and nine real jobs, rejecting 432 corruptions. BF1 independently
confirms both IDs 162 and 2044, adding the earlier completion missed by the old
last-value observation. See `research/DX11_NATIVE_PIXEL_PRESENCE.md` for exact
limits, retained fixture failures, ordinary-replay comparisons and performance.
Full fragment reconstruction, debugger parity and P1/P7/P8 remain open.

## Complete-draw output-test continuation

Private presence now accepts `--primitive-tests` to compare repeated whole draws
with depth, stencil, both depth/stencil, alpha-to-coverage or sample masking
changed explicitly. Actual before/effective states and raw results are retained;
differences do not assign original fragment failure reasons. The combined
owned depth/stencil case demonstrates why a single bypass cannot identify a sole
cause. Seven suites pass 567 checks, and 48 owned GPU cases, three recovery cases,
21 owned Worker jobs and nine real jobs pass independent validation. Validators
reject 768 corruptions. BF1 IDs 441, 578, 2460 and 3935 appear after depth bypass
on all three adapters, while original fragment eligibility remains unknown.
See `research/DX11_NATIVE_PIXEL_TEST_VARIANTS.md` for scope, measurements and
reproduction. Full P1/P5/P6 parity and P7/P8 removal/release gates remain open.

## Debugger module-route continuation

Pinned Ghidra exports and original-code probes identify parser, logging and metrics
extension routes separately from the unresolved debugger object. The player has
separate executable-directory and containing-module-directory helpers. In the
Python-hosted probe those paths differ, and relative parser loading initially
fails; explicit verified preloading permits the wrapper/layout probe to complete.
This bounds earlier null-engine observations and directs the next experiment toward
an isolated host with original executable-relative module resolution. Twelve
Ghidra exports, six wrapper lifecycles, six resolver cases and two directory cases
pass validation, with 48 corruptions rejected. See
`research/DX11_DEBUGGER_MODULE_ROUTES.md`. No debugger-engine recovery or product
parity/removal gate is claimed by these research-only results.

## Executable-directory environment continuation

A research-only native launcher now controls the process executable directory
while reusing the existing Python probes. Relative parser loading succeeds beside
read-only GPA copies and fails under the ordinary Python host, confirming the
previous loader finding without explicit parser preloading. Six environment
reports, nine watcher calibrations and 23 complete original lifecycles validate;
158 corrupted observations are rejected. The colocated, controlled Intel
Helldivers case retains two 120-second timeouts at different replay boundaries.
The complete lifecycle matrix therefore remains open. No live debugger object
is observed, and neither shader-debug parity nor product/release acceptance is
claimed. See `research/DX11_DEBUGGER_HOST_ENVIRONMENT.md` for exact scope, failed
attempts, module observations and reproduction. P1/P5/P6 and P7/P8 remain open.

## Minimum-precision texture load continuation

Native VS/PS/CS execution now supports minimum-precision non-MS SRV texture
loads for all five existing load dimensions, preserving coordinate operand form,
destination precision and unknown storage validity. The explicitly selected GPU
executes both coordinate conversion and the original load inputs; no universal
CPU truncation rule is assumed. The helper identity advances to `dxbc-gpu/22`.
Ten suites pass 3,766 checks without failures or skips. Independent validation
covers 180 fixtures and 7,200 direct GPU comparisons on WARP, NVIDIA and Intel,
rejecting 54 corrupted reports. See `research/DX11_NATIVE_TEXTURE_PRECISION.md`
for the retained development failures and exact scope. Minimum-precision MS/UAV
loads, full debugger/history parity and P1/P7/P8 remain open.

## Original debugger availability continuation

Pinned Ghidra metadata and actual dispatch instructions associate the original
`ShadersPresenter::isShaderDebuggerAvailable` property with a constant false
getter in GPA 25.1. Nine guarded reads through both the original getter and Qt
dispatcher pass; independent validation rejects twelve corrupted reports. The
request-forwarding path reaches the previously identified builder, but this
does not recover the host `+0x3c0` implementation or prove whole-program engine
absence. See `research/DX11_DEBUGGER_AVAILABILITY.md`. Product code does not load
the original module, and P1 remains open.

## Native texture gather continuation

VS/PS/CS now execute `gather4`, `gather4_c`, `gather4_po` and `gather4_po_c`
through the independent executor and explicitly selected GPU. Source channels,
result swizzles, offsets, view/resource selection, partial writes and unknown
validity are preserved. The helper identity advances to `dxbc-gpu/23`.
Eleven serial suites pass 4,093 checks without failures or skips. Independent
validation covers 324 configurations, 2,808 direct GPU comparisons, 108 explicit
unsupported-format contracts and 36 journal round trips, with 99 corrupted
reports rejected. See `research/DX11_NATIVE_GATHER.md` for retained development
failures and bounded scope. Minimum-precision gather, feedback/status variants,
captured invocation qualification, full P1/P5/P6 parity and P7/P8 remain open.

## Minimum-precision gather continuation

The four gather operations now preserve destination and source precision,
modifiers, literal operands and VS/PS output declarations through the selected
adapter. The helper identity advances to `dxbc-gpu/24`. Seven focused suites pass
3,515 checks without failures or skips. The final minimum-precision matrix covers
216 configurations, 2,880 original GPU comparisons and 72 reversible journal
checks, rejecting 198 corruptions. The ordinary gather matrix also passes with
2,808 comparisons, 108 unsupported-format contracts, 36 journal checks and 99
corruption rejections. See `research/DX11_NATIVE_GATHER_PRECISION.md` for the
retained mixed-precision transport failure and its independent correction.
Integer gather format coverage, feedback/status operations, original fragment
qualification, full P1/P5/P6 parity and P7/P8 acceptance remain open.

### Native planar SRV execution

The native executor now resolves NV12/P010/P016 Y and UV views for load,
explicit sampling, gather and resource-information paths. Plane dimensions,
byte offsets and unknown-byte propagation retain the selected typed view.
The GPU dimensions helper now declares the view's actual return class, fixing
its validation failure on UNORM resources. Backend identity is `dxbc-gpu/25`.

The new matrix passes 219 checks, including 2,304 direct original-shader
comparisons across VS/PS/CS, 24 NV12 GPA frame round trips and 24 independently
verified NVIDIA P016 rejection contracts. The validator rejects 632 corrupted
reports. No capability rejection is counted as a shader comparison or skip.
See [the planar SRV report](research/DX11_NATIVE_PLANAR_SRV.md) for retained
development failures, precise coverage and reproduction instructions. Legacy
P010/P016 incomplete chroma, planar UAV execution, original-pixel qualification
and full dependency removal remain outside this completed increment.
Twelve existing suites also pass 4,831 checks without failures or skips; the
new and existing suites total 5,050 successful checks. GUI, CLI and Worker
builds pass. These focused results do not close the standalone release gate.

### Native class-interface execution under validation

The independent executor now resolves named/created class bindings from the
original shader metadata and effective GPA pipeline, executes function tables,
preserves call-stack boundaries and displays raw `this[]` registers. Empty
optimized function tables are accepted. The input provenance identity advances
to `independent-replay-resource-inputs/8`; no private GPA library is involved.

The expanded suite reports 40 passing checks and 22 failed original GPU
comparisons, with no skips. Native arithmetic agrees with the authored integer
contract and WARP across the 36 expanded layout configurations, but NVIDIA/Intel
original outputs have unresolved differences. Reusing identical DXBC with and
without the debug layer does not remove them. Existing regression suites pass
1,842 checks; the combined CTest run remains failed because interface comparisons
remain exact. See [the layout and control evidence](research/DX11_NATIVE_INTERFACE_LAYOUTS.md).
This is partial P6 implementation, not a parity exemption or authorization for P7
dependency deletion. Class texture/sampler members, general VS/PS and source
debugging coverage, original fragment qualification and P1/P5/P8 gates remain open.

### Class-debug baseline status versus instruction semantics

Six frozen-main class-debug jobs return success on Software/NVIDIA/Intel, but
their four-state traces never enter the class function bodies and leave the
store's temporary input at zero. Six native Worker jobs execute those bodies,
preserve return boundaries and record the authored 24 group writes. The saved
observation validator rejects 30 corrupted traces. See [the focused baseline
report](research/DX11_MAIN_CLASS_DEBUG_BASELINE.md). This separates job success
from semantic coverage; it does not waive original GPU differences, establish
general debug parity or close the detailed P0 matrix.

### Legacy SDBG source values in native execution

VS/PS/CS execution models now resolve supported original SDBG input and assignment
records. Compute system inputs require matching declarations; VS/PS inputs require
declaration and signature coverage. Opaque structured-UAV assignments remain
resource observations rather than invented scalar values. Three direct source
fixtures verify 91 selected boundaries, loops, watches, source breakpoints and
reverse traversal. Three captured compute Worker workflows match 12 original GPU
output words on WARP/NVIDIA/Intel. The new suite passes 9 cases and existing
model/UI/checkpoint regressions pass 58, with no failures or skips. See [legacy
source execution evidence](research/DX11_NATIVE_SDBG_EXECUTION.md). Other resource
annotation forms, original subroutine source stacks, full PS selection and final
dependency removal remain open.

### Original debugger host accessors

Seven reproducible Ghidra exports trace the singleton creation paths, raw host
accessors and immediate consumers. A separate original-code probe passes 54
guarded combinations and confirms that the alternate setter changes only host
byte `+0xf8`. The pointer consumers reach a neighboring `+0x3b8` callback, query
management and resource processing; they do not establish an engine at `+0x3c0`.
See [the accessor evidence](research/DX11_DEBUGGER_HOST_ACCESSORS.md). P1 still
requires the engine's nonzero assignment, constructor, vtable and module. This
research increment changes no product routing or existing acceptance gates.
