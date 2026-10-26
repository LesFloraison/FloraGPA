# DX11 native analysis migration

Baseline: `3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28` on `main`.
Implementation branch: `feature/dx11-native-backend`.

This is an implementation ledger, not a standalone release acceptance report.
RenderDoc removal is blocked until every formerly successful capability below has a
verified native replacement. The existing backend remains available during migration.
Neither reserved CLI action names nor native GPU checkpoints constitute a completed
VS/PS/CS debugger or fragment history implementation.

## Stage gates

| Stage | Gate | Current evidence |
| --- | --- | --- |
| P0 | Reproduce baseline, enumerate dependencies and retain reports/performance | Clean full build passed; 314/317 baseline tests pass with three reproduced historical failures; detailed end-to-end feature parity remains open |
| P1 | Close GPA history, geometry, query and debugger implementations with independent probes | Static history/geometry/query call paths identified; original debugger requests observed on three captures with null engine and no result callback; concrete engine remains unresolved |
| P2 | Versioned analysis requests, context identity, Worker isolation and GUI scheduling | Native CLI/Worker infrastructure and artifact verification tested; Replay Metrics uses asynchronous native scheduling; remaining consumers pending |
| P3 | Resource, counter and geometry parity | Native inventory/events/texture/mesh and all-event counters exposed; verified mesh artifact reader and VS identity exports tested on three adapters; full parity matrix pending |
| P4 | Event history including writes without changed values | Explicit event-storage CLI/Worker, typed values and narrow ordinary-texture readback; full format/semantic matrix and special-format performance remain open |
| P5 | Fragment history, failure reasons and shader outputs | Pending |
| P6 | Independent DXBC VS/PS/CS execution with source/debugger parity | CPU execution, private buffers, GPU assistance, cooperative CS groups, hidden counters, pixel quad derivatives and implicit sampling tested; captured quad inputs, helper memory semantics, production debugger integration and full parity remain open |
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
```

The native entry point never recaptures to RDC or selects an external replay
backend. History/debug actions are reserved but fail explicitly until implemented.
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

The Replay Mesh GUI remains on its existing path until its successful baseline
coverage is preserved. The following are explicit P3 blockers, not exemptions:

- The older assembled `vs-index` mode still omits adjacency helpers and incomplete
  tails. `vs-inputs` now preserves them in the three-adapter matrix, including full
  signatures and indirect draws. Full baseline mesh parity still needs the remaining
  real-capture, format and workflow comparisons before default cutover.
- Per-instance HS/DS/GS boundaries with UAV access remain unverified. The current
  native implementation rejects that selection. Tests check both this rejection
  and successful all-instance DS capture, separately from a pure-shader DS
  instance-selection fixture. They do not establish equivalence to baseline
  Replay Mesh for the rejected selection.
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
