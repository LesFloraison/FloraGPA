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
| P1 | Close GPA history, geometry, query and debugger implementations with independent probes | Static history/geometry/query call paths identified; debugger implementation not resolved |
| P2 | Versioned analysis requests, context identity, Worker isolation and GUI scheduling | Native CLI/Worker infrastructure and 11 focused tests pass; GUI integration pending |
| P3 | Resource, counter and geometry parity | Native inventory/events/texture/mesh and all-event counters exposed; three real inventory comparisons pass; full parity matrix pending |
| P4 | Event history including writes without changed values | Pending |
| P5 | Fragment history, failure reasons and shader outputs | Pending |
| P6 | Independent DXBC VS/PS/CS execution with source/debugger parity | Pending |
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
```

The native entry point never recaptures to RDC or selects an external replay
backend. History/debug actions are reserved but fail explicitly until implemented.
During this stage the legacy command is still present for baseline comparison;
its final retirement and GUI cutover remain separate gates.

Without an event/range selection, `counters` measures the 13 generic DX11 values
for every draw/dispatch in one replay. Query storage is bounded to 64 pending
samples, with a synchronization between batches. Counter identifiers and units
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
are retained beside the result. GUI cache acceptance still needs integration.

## Delivery constraints

GPU tests run serially. Final validation must include NVIDIA, Intel and applicable
WARP paths, clean capture-only/replay-only/capture-full builds, runtime module and
child-process auditing, bounded time/memory, and an installation without either
RenderDoc or GPA. No golden output or historical support limit may be weakened to
pass these gates. Main is not merged automatically.

All implementation commits use Author and Committer
`Floraison <181715567+LesFloraison@users.noreply.github.com>` and English messages.
