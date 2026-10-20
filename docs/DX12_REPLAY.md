# DX12 single-frame directory replay

Development branch: `feature/dx12-replay`, based on `aa2b5e4`.
This is an implementation record, not a claim of completed DX12 replay.

## Current implementation

The shared `IReplaySession` interface distinguishes DX11 frames from DX12 stream
directories. DX12 uses read-only file mappings, checked 64-bit offsets, a compact
packet index, and on-demand bounded payload previews. Opening a directory enters
the existing workspace. API name/text and thread filters, event location and
drag-and-drop use the existing API log. Initialization records are explicitly
distinguished from frame-thread records. CPU timestamp order is not GPU execution
order, and an indexed record is not yet a command-list execution instance.

`inventory`, `commands`, `command-lists`, `replay`, `buffer`, `shader` and `validate-frame` accept DX12 directories. Validation
reports `blocked` while complete frame replay and feature acceptance remain unfinished;
the selected native execution and isolated analysis paths below are available for investigation.
DX12 CLI `replay` without `--event` now processes the entire captured frame
stream, including its trailing CPU/Fence records. This reports
`frame_stream_end_reached` separately from unverified original-frame fidelity.
The workspace offers **Frame end** as well as selected execution boundaries;
choose a texture creation explicitly for image readback.
Selected draw, dispatch, copy, command-list, queue/fence and resource-creation
arguments are decoded on demand. Committed, placed and reserved resource
creation descriptors appear in the resource table with their originating record
and captured object pointer. These rows identify creation records, not resolved
object lifetimes or restored GPU resources. Newer creation APIs remain gaps.
Unknown packet types retain their numeric identity and bytes; they are not
silently treated as supported calls. Captured-object filtering currently covers
decoded resource creation and copy arguments only; the UI labels that limitation.
It does not resolve object lifetime aliases or infer shader binding use.

The API log can switch from recorded calls to submitted command instances, with
queue filtering and location within either domain. Each submitted list ordinal
gets a distinct execution range, including repeated submissions of the same
recording. Reset starts a new generation; incomplete recordings remain explicit.
Queue signal/wait records preserve 64-bit fence values. This index does not yet
resolve cross-queue dependencies or certify a global GPU execution order.

CopyOperation resource data uses a checked 64-byte descriptor and a signed
offset relative to its end. Data remains a mapped span; the inspector reads at
most 64 preview bytes. Unsupported data kinds and missing/invalid ranges fail
explicitly. The initializer contains ExecuteCallOperation references to other
records, so physical initialization record order is not an execution schedule.

Native DX12 replay, resource inspection/history, Pixel History, shader debugging,
geometry, metrics and experiments remain **unfinished**. Ordinary execution does
not load GPA binaries. No capture preview is used as a replay output.

## Format evidence

Reference: installed GPA 2025 R1 `Streams/StreamPlayback.dll`, SHA-256
`e723d2904d0bf277714c0844882ccd3641b63d324c86b27532cad19ab8d0b07c`.
Addresses below are RVAs relative to image base `0x180000000`.

- `0x04fcc0` reads the current fixed `metadata` header (`0x288` bytes).
- `0x4f71b0` dispatches metadata records, including variable version strings,
  application names, frame boundaries and versioned keyframe headers.
- `0x4f80c0`, `0x4f7a00`, `0x4f7be0` establish header and string layouts.
- `0x0ad2e0` dispatches interface/method identifiers. The returned Playable
  objects' RTTI establishes API names in `StreamNames.inc` independently of
  guessing from argument bytes.
- `0x53bb20`, `0x53bbd0`, `0x53baa0`: indexed/nonindexed draw and dispatch readers.
- `0x676fb0`, `0x67c580`, `0x50cd50`, `0x5b5db0`: submission, reset, close and
  queue signal/wait readers. Submission extensions remain undecoded.
- `0x679c10`, `0x679fd0`, `0x67a1a0`: committed/placed/reserved creation readers;
  `0x60ec40` reads the optional 56-byte resource descriptor.
- `0x67bfa0`, `0x53ac50`: buffer/resource copy readers.
- `0x6798b0`: command-list creation reader. `0x0affc0` establishes global API
  identities, including D3D12CreateDevice and CreateDXGIFactory variants.
- `0x0a2ea0` dispatches internal operations; `0x4f4ad0`, `0x4f5090`,
  `0x4f56f0`, `0x4f4a20`, `0x4f60c0` establish Copy, ExecuteCall, ThreadRestart,
  AdjustRefCount and Wait kinds. `0x4f4c50` writes the CopyOperation fields;
  `0x4f05c0` resolves its resource descriptor. `0x5438d0` independently shows
  descriptor length at `+0x28` and data at `descriptor + 0x40 + relative(+0x30)`.

Ghidra 12.1.4 imported the reference into the local `FloraDX12` project with a
180-second initial analysis budget. Initial analysis timed out; targeted
decompilation subsequently used read-only/no-analysis runs. This is not a claim
that the whole binary has been analyzed. Static emulation of the identifier
dispatcher corroborates the RTTI mapping; it is a research tool only.

All three supplied directories have type-10 metadata headers and version-3
keyframes with inline packet storage. Packet traversal uses a 32-byte header and
the captured 64-bit payload length, with exact end-of-region checks. Unrecognized
metadata layouts and alternate keyframe storage fail explicitly.

| Sample | Initialization packets | Frame packets | Decoded creation records | Resource file bytes |
| --- | ---: | ---: | ---: | ---: |
| BF1, 2025-12-10 | 363,164 | 54,897 | 17,132 | 2,751,020,329 |
| Talos, 2026-10-18 | 185,466 | 7,069 | 7,352 | 1,591,201,807 |
| Hellblade2, 2026-10-17 | 577,511 | 35,999 | 4,876 | 6,161,707,140 |

These counts are structural observations, not replay-fidelity acceptance.
Raw binary analysis, scripts and fixture reports are local under
`artifacts/dx12-research` and are not distributed.

## Focused validation

`FloraDx12SessionTests` covers packet identity, explicit capability boundaries,
malformed packet lengths, missing files, cancellation, mapping offsets above
4 GiB, and opening/searching/locating records in the existing window. Set
`FLORA_DX12_CAPTURE_DIR` to the supplied capture root to include the three original
directory indexes. GPU tests remain serial and are added only for an implemented
execution path or a reproduced fault.

2026-10-18: Release application, CLI, Worker and focused test target built.
The structural/parameter/navigation checks passed for all three directories;
the separate DX11 compatibility/switch check passed. GF2's full-frame RGBA SHA-256
remains `2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`, and its
geometry analysis succeeds. All three real DX12 directories enter the window
with resource-creation rows. Replacing an active DX11 worker session with DX12
clears the old image. A stale enabled geometry action found by this check was
fixed. Other documented DX11 baseline failures were not rerun.
No DX12 GPU replay or advanced-analysis acceptance has passed yet.

Subsequent focused checks passed for signed resource-data offsets, overflowing
lengths, repeated list submissions, generation changes, queue-filter location
and decoded object references. All real CopyOperation data ranges were in bounds:
BF1 9,571; Talos 3,679; Hellblade2 2,781 (1,473 above 4 GiB). The largest observed
Hellblade2 block is 603,979,776 bytes and is not copied by the inspector.
Submitted-list/command-instance counts are BF1 226/54,186, Talos 41/19,410,
Hellblade2 349/37,104, with no missing list-recording associations in these samples.
These are structural checks, not proof that recorded GPU commands have executed.

## Remaining milestones

### Native device prerequisite (2026-10-18)

`validate-frame <directory> --check-device --out <new-directory>` creates a
native device in the CLI/Worker and records both the capture's SDK requirement
and the actual runtime SDK version. This command still returns exit code 2 and
`status: blocked`: creating a device is not frame replay validation.
The preferred path uses `ID3D12SDKConfiguration1::CreateDeviceFactory`; the
legacy fallback calls `SetSDKVersion` before device creation and reports failures.
No GPA library is loaded by this code. The SDK comes from the capture directory.

Targeted Worker checks on NVIDIA RTX 5090 Laptop succeeded for Talos (requested
616, actual 616) and BF1 (requested 602, actual 616). Both captured devices require
feature level 11_0. The newer actual version is reported rather than assumed to
equal the directory name. Device argument readers were corroborated at playback
RVAs `0x676b80` and `0x67a6c0`. Resource restoration remains a separate requirement.

SDK behavior follows Microsoft's
[SDK configuration API](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12sdkconfiguration-setsdkversion)
and [independent-device specification](https://microsoft.github.io/DirectX-Specs/d3d/IndependentDevices.html).

### Outstanding implementation

Worker requests now carry a shared backend/session/request/revision envelope.
The native Worker publishes an atomic `response-identity.json`; the analysis
Worker echoes the same identity in its result and envelope. The window checks
the returned identity before reading artifacts and again against the active
context after asynchronous decoding. Request/revision integers are decimal
strings, and numeric JSON identities are rejected. The monotonic revision also
changes on selection and cancellation, providing stricter invalidation than an
undo cursor alone. Legacy direct CLI use without an envelope remains supported.
Focused checks covered UINT64_MAX round trips, changed backend/session/request/
revision rejection, analysis failure envelopes and the DX11 replay/session-switch
path. The DX11 image hash and geometry result remained unchanged.

DX11 experiment files now carry explicit `backend` and `capture_identity` fields.
Legacy version-1 projects are still accepted after validating their original
frame hash, then normalized on load. A backend or capture identity mismatch is
rejected without changing the active experiment. A focused legacy-load check
passed; this does not implement DX12 experiment operations.

An additional targeted prerequisite is available as
`validate-frame <directory> --check-copy <initialization-record> --out <new-directory>`.
It resolves the selected buffer copy's creation recipes (including forward heap
references), validates a complete captured Create/Map/CopyOperation/Unmap upload,
creates native committed/placed resources, executes that CopyBufferRegion and
compares GPU readback byte-for-byte. Allocation is bounded by `--max-bytes`
(default 256 MiB), including the device's actual allocation requirements.
This isolated check does not replay original queue dependencies or establish the
resource's complete history. Overall validation remains blocked with exit code 2.

| Targeted copy | Source / destination creation record | Bytes | Exact GPU readback SHA-256 |
| --- | --- | ---: | --- |
| Talos record 8530 | 16 / 13, heap 69087 | 2,130 | `5b3538cefb72a75c3d01861f8787be8ce4ade7effc1404fa09297c502500fb9a` |
| BF1 record 11858 | 28 / 25, committed | 10,752 | `4741bc07df80a8c59551a9d7cb86d72c527a5284caece523a2b72fa869480721` |
| Hellblade2 record 507335 | 102 / 99, heap 530246 | 8,192 | `c76759aec5daf179a7e2b3d717370c6707338e9cf24b6009b30d1143d5377f9e` |

Each GPU check ran once, serially, in the native Worker on the NVIDIA adapter.
No GPA/Python runtime is used by this path. The CPU-only dependency test also
checked memory-budget rejection, wrong record type and cancellation. Heap and
Map readers were corroborated at RVAs `0x5b61d0`, `0x67ced0`, `0x67d060`,
and `0x60eb40`. Full portable-package validation is still outstanding.

1. Complete typed arguments, resource/object catalog and submission-instance
   identity; preserve source file/offset provenance.
2. Restore initial resources, addresses, descriptors, PSOs and queue/fence
   dependencies; produce and compare actual native GPU output for all samples.
3. Connect history, shader debugging and geometry with exact event/resource
   mapping to the isolated analysis backend.
4. Implement GPU measurements and core non-destructive modification experiments,
   then verify the portable package without GPA or Python runtime dependencies.

## Acceptance ledger

### Pipeline reconstruction prerequisite

`PipelineRecipes` decodes native graphics/compute descriptors, pipeline-library
loads, root signatures and pipeline state streams. Shader bytecode and semantic
names stay in the read-only mapping. Only native stream structures and small
pointer-bearing arrays are materialized. All embedded pointers are replaced;
captured driver-specific PSO caches are consumed but not submitted to a different
adapter. A library load is reconstructed from its saved complete descriptor, so
the implementation does not depend on the original driver library blob.

`NativePipelines` resolves root signatures by captured object identity, including
forward keyframe references, and caches native objects by creation record.
Ambiguous initialization lifetimes, unknown stream subobjects, inconsistent
lengths and truncated records fail explicitly. The pipeline-stream path includes
AS/MS bytecode; Hellblade2 contains 72 mesh pipelines, which are required work
for command/signature execution, not optional omissions.

Focused CPU decoding covered all recorded pipeline recipes: BF1 1,015 / 21 root
signatures; Talos 640 / 31; Hellblade2 11,571 / 208 (11,555 state streams).
Malformed stream sizes/types and truncated records are rejected. The extra BF1
pipeline beyond its 1,014 initialization recipes is a frame-thread creation.

Serial native checks through `validate-frame --check-pipeline <record>` succeeded
on the NVIDIA adapter for these distinct implementation paths:

| Sample / record | Native path | Root creation record | Shader format |
| --- | --- | --- | --- |
| Talos / 183 | Library descriptor → graphics PSO | 102760 | DXBC VS/PS |
| BF1 / 4756 | Compute PSO | 11792 | DXBC CS |
| Hellblade2 / 4 | Pipeline state stream | 518001 | DXIL VS/PS |
| Hellblade2 / 1390 | Mesh pipeline state stream | 538845 | DXIL MS/PS |

These are native object-creation checks, not Draw/Dispatch or frame validation;
the overall validator still reports blocked and returns 2. GPA/Python are not
runtime dependencies of this path. Ghidra evidence uses the same reference DLL
hash above: RVAs `0x679f00`, `0x679cf0`, `0x67a250`, `0x67cba0`, `0x677ee0`;
descriptor readers `0x60dce0`, `0x60e800`, `0x8e1f50`; stream traversal `0x783d30`
and pointer fixups `0x8e1720`, `0x8e16a0`, `0x8e28a0`, `0x8e2a60`.

### Captured initialization submissions (2026-10-18)

`NativeReplay::initializeTransfers` now executes the actual submitted keyframe
command-list recordings, with their original queue/list/submission identities.
Unsubmitted graphics recordings in metadata are not executed. The implementation
restores mapped buffer snapshots, shared heaps, committed/placed/reserved resources,
legacy barriers, buffer/texture/resource copies and tiled-resource transfers.
It serializes the captured single initialization queue with completion fences;
it does not establish frame or cross-queue execution support.

`NativeResources` keeps native allocations by creation record. Repeated addresses
within initialization select the latest preceding creation; forward references
are accepted only when unambiguous. This was required by Hellblade2's upload
resource declarations at records 1324 and 1563. Explicit helper `IUnknownRelease`
records retire completed upload allocations. Other COM lifetime/refcount and
residency operations still need the full frame object-state implementation.
Castable formats are preserved for `CreateCommittedResource3` and passed to
`GetResourceAllocationInfo3`: the older allocation query incorrectly rejects
Hellblade2's BC1 UAV resource at creation record 531786. Invalid allocation
descriptions and actual budget exhaustion now have separate diagnostics.

The resource budget includes native committed allocations and complete backing
heaps, using device allocation requirements. Reserved virtual address ranges are
not counted as physical allocations; their tile heaps are. Readback allocations
must fit the remaining budget. This is resource allocation accounting, not a
measurement of total driver/physical VRAM usage. Resource data remains mapped
read-only, and CPU restoration copies at most 8 MiB per cancellable iteration.

Use a fresh CLI/Worker process and a new output directory, for example:

```text
FloraGPA.Worker validate-frame <Talos-directory> --check-initialization --max-bytes 3221225472 --id 13 --length 2130 --verify-texture-copy 8525 --out <new-directory>
```

`--id/--offset/--length` optionally read back one initialized buffer;
`--verify-texture-copy` compares a complete initialized texture subresource with
its captured upload, excluding row padding. Both identify the boundary as
`after_initialization_submissions`. The top-level result remains `blocked`, with
exit code 2 and `frame_replayed: false`. A failed attempt cannot be reused.
The report distinguishes commands recorded from commands whose submission
completed, and retains failure record, partial progress and allocation counts.

Targeted native execution on the NVIDIA adapter produced:

| Sample | Completed submissions | Buffer / texture copies | Barrier calls | Peak resource allocation bytes | Selected readback |
| --- | ---: | ---: | ---: | ---: | --- |
| Talos | 25 | 2,299 / 10,300 | 50 | 2,295,332,864 | Buffer creation 13: same exact 2,130-byte hash as above; texture copy 8525 matched |
| BF1 | 52 | 5,785 / 15,722 | 101 | 3,339,649,024 | Buffer creation 25: same exact 10,752-byte hash as above |
| Hellblade2 | 26 | 761 / 8,885 | 52 | 4,841,340,928 | Buffer creation 99: same exact 8,192-byte hash as above; texture copy 507333 matched |

Talos texture copy 8525 targets creation 1, subresource 0 (16×16 BC1 typeless).
All 128 effective bytes matched SHA-256
`800c034632490c5b5cf5e41adc9ea0c6ceb57c55873861c06f7701f357c31b01`.
Hellblade2 copy 507333 targets creation 57, subresource 0 (480×456 R8 UNORM).
All 218,880 bytes matched SHA-256
`d63358ddc37ec6bf67f2c66451f2b44ddbc19ca6bd5c70bc6d2bd0c96e72643f`.
These comparisons use GPU readback, not directory preview images. They verify
selected initialization contents, not every resource or original GPA frame parity.

Hellblade2 additionally executed 8,192 `UpdateTileMappings` calls and two
4,096-tile `CopyTiles` calls. Native subresource tiling, heap ranges, array lengths,
copy direction, states and linear buffer bounds are checked. Implicit mapping
arrays, packed-mip tile coordinates and regions crossing subresource boundaries
remain explicit unsupported cases; they were not required by these initialization
records. Tile-copy bytes have not yet received their own readback comparison.
Semantics follow Microsoft's [UpdateTileMappings documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-updatetilemappings)
and [CopyTiles documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytiles).

Focused CPU checks decoded BF1 17,132 resources, Talos 7,352 resources / 6 heaps,
and Hellblade2 4,883 resources / 61 heaps (7 modern descriptions), plus all
Hellblade2 tile records. Truncated resource/heap/tile records and oversized tile
arrays were rejected. Native guard checks verified budget rejection before
allocation/submission, pre-execution cancellation and rejection of context reuse.
Cancellation while GPU work is pending, device-loss recovery, and overall
Hellblade2 frame memory/residency behavior remain unverified.

Ghidra evidence uses the reference DLL hash above. Relevant reader RVAs:
`0x6774d0` / `0x60ebc0` (modern resource / DESC1), `0x679300` / `0x5b61d0`
(heap), `0x6799a0` (queue), `0x5e48a0` (allocator), `0x5c9c90` (fence),
`0x67c060` / `0x60edc0` (texture copy / location), `0x67c6e0` (barriers),
`0x50cd50` (Release), `0x677210` (tile mappings), `0x5b5c70` (tile copies).
`ID3D12ExecuteCommandListsUserDataReader` at `0x50baf0` reads the `0xfede0000`
header and a count of three-qword remapping entries. Initialization submissions
here have zero entries; nonempty remapping data is explicitly rejected pending
indirect-argument reconstruction. Runtime code loads no GPA/Python component;
portable-package replay/analysis acceptance is still outstanding.

### Descriptor and address restoration prerequisite (2026-10-18)

`AddressIndex` resolves captured GPU virtual addresses to a creation record and
byte offset using an interval index. It checks access length, boundary and
resource lifetime before querying the native resource address. Overlapping
active resource address ranges remain explicit unresolved aliases; the index
does not guess a resource from the nearest address. Native CBV descriptors and
acceleration-structure SRV addresses use this translation.

`NativeDescriptors` reconstructs descriptor heaps and CPU/GPU handle offsets
using each captured device/type's recorded descriptor increment and the native
device's increment. It restores CBV/SRV/UAV/RTV/DSV/sampler writes, including UAV
counter resources, and implements range/simple descriptor copies. Per-slot
provenance retains the originating write and most recent copy; unknown source
slots remain unknown. Copy provenance staging is limited to 64 MiB. This is not
yet a shader-access or Resource History acceptance result.

Frame integration must preserve descriptor consumption boundaries: CPU RTV/DSV
bindings are recorded against the descriptor contents at the command's recording
time, while shader-visible tables can be updated before submission. Rebuilding a
frame command list at submission using only the latest CPU descriptors is not a
valid replacement for this distinction. Initialization transfer lists do not
consume descriptors. The selected frame execution path below now preserves these
recording boundaries; its remaining gaps are tracked separately.

`BackBufferIndex` reconstructs offscreen resources from captured swapchain
descriptions and `GetBuffer` identities, including forward initialization
references (Talos buffer query 35118 precedes parent creation 42874). Repeated
queries preserve one buffer lifetime; resize generations remain separate.
Descriptions are derived from recorded swapchain dimensions, format, samples
and usage. No window is created, and this does not recreate missing initial
backbuffer contents or claim successful presentation. Unresolved dimensions,
stereo layouts and unsupported interface identities fail explicitly.

CPU decoding covered all three samples' descriptor records:

| Sample | Heaps | Writes | Copies | Backbuffer lifetimes |
| --- | ---: | ---: | ---: | ---: |
| BF1 | 19 | 180,773 | 4,141 | 3 |
| Talos | 9 | 25,046 | 0 | 3 |
| Hellblade2 | 26 | 512,622 | 4,378 | 4 |

Native initialization with descriptor restoration succeeded for Talos (9 heaps,
24,974 write calls, 23,150 known slots) and BF1 (19 heaps, 171,892 write calls,
171,098 known slots). The initialization portions contain no descriptor-copy
calls; native frame copy execution remains to be verified. Hellblade2's descriptor
restoration and frame descriptor copies now have first-Draw evidence below. Targeted checks also verified
native GPU descriptor increment translation, invalid alignment rejection,
GPU address offsets/access bounds, cancellation and truncated descriptor input.

Ghidra reader RVAs: `0x677480` / `0x60e8f0` (CPU heap handle), `0x53a2f0`
(GPU handle), `0x53ba30` (increment), `0x50dbe0` (GPU virtual address),
`0x67a3d0`, `0x67a560`, `0x679d80`, `0x679e30`, `0x67a310` (views/sampler),
`0x679750`, `0x679810` (copies), `0x5f47a0`, `0x5e3f30`, `0x5ce7f0`
(swapchain descriptions). The legacy swapchain descriptor omits native padding
and is decoded field-by-field rather than copied as a 72-byte native structure.

### Selected native frame execution

`NativeReplay::replayTo` records frame command lists at their CPU stream
boundaries and executes the indexed submission instances. Native allocations
and command allocators stay alive through GPU completion. A selected instance
can execute a prefix before or after its exact execution ID; repeated instances
can retain both a full recording and the selected prefix. Resource-state checks
run in submission order, independently of interleaved CPU recording order.
Unsupported operations, unresolved queue producers, missing keyframe recording
state and nonempty indirect submission remappings fail at their source boundary.
Queue completion is currently serialized for correctness, not performance.

The recorder implements the graphics/compute binding and draw paths encountered
in Talos's first main recording, including native PSOs/root signatures, GPU
address translation, vertex/index views, descriptors, clears, discard regions
and query commands. Frame memory restoration handles bounded partial buffer
patches (Talos records 185507/185508: 9,732 bytes followed by 1,080 bytes at offset
8,652). COMMON promotion and submission-boundary decay follow the
[D3D12 resource-state rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12);
upload/readback heaps retain their fixed states. State lookup also recognizes
when every subresource has individually converged to the same state.

Targeted native results on SDK 616 / RTX 5090 Laptop, with the D3D12 debug layer:

| Boundary | Resource | Result |
| --- | --- | --- |
| Before execution 12675, source record 185550 | Depth object `4980699392`, creation 167081, subresource 1 | 2048x2048 R16 readback, all 8,388,608 bytes zero |
| After execution 12675 | Same depth subresource | 792,141 changed bytes; SHA256 `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217` |
| After execution 16000, source record 188916 | Color object `1651488160`, creation 42877, subresource 0 | All 521 Draws in the first main recording executed; 1920x1080 BGRA8 readback, SHA256 `4fcac91107578f2a4a96ec8ffaaaa1d1cf343155205ecf28beb3861c843eb2e8` |

These accepted boundaries have zero debug-layer errors. Warnings report captured
sampler comparison fields ignored by non-comparison filters, optimized-clear
value differences, and buffer initial-state normalization. Raw texture bytes and
PNG output are produced from GPU readback, never from the directory preview.
Local evidence: `artifacts/dx12-research/talos-first-draw-before2`,
`talos-first-draw-after1`, and `talos-first-color1` (ignored by Git).

The next command, `ResolveQueryData` at source record 188923 / execution 16001,
requests 236 occlusion queries, but this frame has only completed 96 of them.
Executing it without restoring historical query state produced 140 debug-layer
errors. The implementation now rejects that submission before GPU execution;
it neither fabricates query results nor accepts the resulting image as a
complete replay. `talos-query-guard1` verifies this failure in the Worker with
zero native validation errors and no falsely reported executed Draws. Historical
command recordings exist in metadata, but the necessary query-state restoration
has not yet been established. This remains a full-frame blocker.

The later **CPU query coverage index** now detects these gaps before device
creation in the replay CLI, and before native initialization in direct backend
requests. It follows submitted recording generations and repeated submission
instances, retaining query state across submissions. It uses the same unique
keyframe forward-reference rule as native query heap reconstruction. The
execution details and `command-lists` report include query heap creation,
destination/offset, completed counts, missing ranges, and bounded examples of
recorded EndQuery calls that did not supply a completed replay result.

| Talos resolve execution / source | Submission | Heap creation / object | Requested / completed / missing | Missing slots with / without a recorded EndQuery |
| --- | --- | --- | --- | --- |
| 16001 / 188923 | 188940 | 78117 / 180523136 | 236 / 96 / 140 | 16 / 124 |
| 19409 / 192515 | 192532 | 69442 / 180521984 | 236 / 90 / 146 | 21 / 125 |

The recorded-End distinction is deliberately weaker than a restored value:
historical, unsubmitted recordings cannot seed a native query heap merely by
being present in metadata. For example, the first missing range starts at slot
0 with eight slots, and missing slot 37 has historical EndQuery records 10150,
26647, 48381, 71580, 86422, 110894, 129601 and 147603. No usable result restoration
has been established for these entries. This evidence does **not** establish
that every possible source of saved query values has been exhausted.

#### Historical query source audit

A later targeted audit inspected all **185,466 initialization** and **7,069
frame** records in Talos, plus every referenced resource-data descriptor. It
adds evidence about the missing source without changing replay policy:

| Query heap | Resolve destination | Destination creation | Heap offset | Resource-data snapshots referenced |
| --- | --- | --- | --- | --- |
| 180523136 | 1643440736 | 78132 | 327680 | 0 for heap or destination |
| 180521984 | 1643459296 | 12800 | 458752 | 0 for heap or destination |

Both destinations are 65,536-byte placed buffers on heap **1646217152**,
created at record **42957** with `D3D12_HEAP_TYPE_READBACK` and size **42,008,576**
bytes. Their ranges do not overlap each other or any of the other six captured
placed buffers on this heap. The keyframe forward reference for creation 12800
is consistent with the existing unique-lifetime rule.

Each destination has exactly 14 direct object references in the parsed stream:
nine ResolveQueryData recordings, one creation, one GPU-VA observation, one
Map, one Unmap and one reference-count adjustment. Eight resolves are historical
recordings and the remaining resolve is in the frame. Map records **78134** and
**12802** describe a read range `[0, 1936)`; frame Unmap records **188885** and
**192477** precede their respective frame resolves. These Map/Unmap records do
not contain the result-buffer bytes. This reference audit does not infer that
CPU application decisions can be reconstructed from the stream.

All **3,679** referenced CopyOperation resource descriptors were inspected:
each descriptor's captured object matches its packet object, and none names
either query heap or either destination. Thus neither an alternate object in a
CopyOperation nor an overlapping placed resource provides a known saved-value
source. This is a statement about referenced descriptors and understood records,
not a claim that every unreferenced byte or possible format extension was decoded.

Ghidra analysis of GPA 2025 R1
`layers/d3d12-state-tracker-x64.dll`, SHA-256
`066b82ef52c258d0db820dcca2f5c6719fc37deb7cbc78c96c077113dd8e2d8d`,
finds the following functions (image base `0x180000000`):

- `Pre_BeginQuery`, RVA **0xb200**, calls **0x5c160**. The latter inserts a
  `(heap pointer, query type, slot)` tuple into the command-list state's ordered
  set at offset `0x300`; the allocated tree node is 48 bytes.
- `Pre_EndQuery`, RVA **0xbb00**, calls **0x5cc80**, which removes that tuple via
  **0x5f9a0**. The erase helper unlinks and frees matching tree nodes.

These handlers track active query boundaries. They do not retain completed GPU
query values or resource-data offsets. This observation does not prove that no
other capture-layer routine could save query data; it rules out these particular
state handlers as that source. Together with the previously inspected playback
forwarders and original-player validation errors, the audit provides no basis
to fill missing slots, submit historical recordings as if they were executed,
or silently skip the unresolved ResolveQueryData calls.

Ignored local evidence: `talos-query-sources1.json`, `talos-query-heaps1.txt`,
`talos-query-descriptors1.txt`, `query-state-strings2.txt`,
`query-state-xrefs1.txt`, `query-state-impl1.txt` and `query-state-erase1.txt`.
The audit used bounded metadata inspection, mapped resource descriptors and
Ghidra; it performed **no GPU replay**. A new saved-value source is needed before
retrying restoration. Talos full-frame replay remains unaccepted; work on other
replay/analysis requirements can proceed independently.

#### Capture-layer READBACK exclusion

A further targeted Ghidra investigation identifies an explicit exclusion in
the installed GPA **2025.1.3.1269** capture layer, rather than inferring omission
only from absent CopyOperations. Reference binary:
`Streams/layers/capture-x64.dll`, SHA-256
`a1afd0b41e6bf668381e4898b03460130e0b61432c88d56c8559aef5b1b72c62`.
Addresses below are RVAs from image base `0x180000000`.

- The diagnostic string for
  `ID3D12ResourceApiCallCache::PostVisit` identifies function **0x8e170**.
  Its call at **0x8e6de** uses the resource vtable's **0x70** slot,
  `ID3D12Resource::GetHeapProperties`, with heap-properties and heap-flags outputs.
- **0x8e726** compares the returned heap type against **3** (`READBACK`).
  The branch at **0x8e729** goes to **0x8fbaf**, bypassing the ordinary resource
  content-copy/serialization path. The other condition on this branch excludes
  multisampled resources; this finding is specifically about READBACK buffers.
- The excluded branch examines tracked resource states for **0x10**
  (`DEPTH_WRITE`) and can call **0xa6e80**. That helper builds 32-byte resource
  transition records; it does not serialize a query-result byte array.
  This is not a historical query-value restoration path.
- `ID3D12ResourceApiCallCache::EnumerateKeyframeCalls`, **0x8abf0**, separately
  reconstructs resource creation. Its ordinary placed-resource branch calls
  **0x86990**; resource creation alone is not a contents snapshot. Some other
  creation branches do write saved blobs, so the READBACK finding must not be
  generalized to every resource or every capture mechanism.

The two original Talos placed-resource packets were reread at metadata packet
offsets **6917915** (creation **78132**) and **1090253** (creation **12800**), and
their bytes match the earlier saved audit. Both initial states decode as
**0x400** (`COPY_DEST`); their heap is the previously verified READBACK heap
**1646217152**. Thus the absence of ordinary snapshots for these buffers is
consistent with this explicit capture-layer branch. The local reference DLL's
behavior does not prove that the capture was produced by this exact binary,
nor that every possible source of query values has been exhausted. It provides
no basis for inventing values, discarding resolves or declaring full replay.

Evidence remains outside Git: `query-resource-writer1.txt`,
`query-resource-readback-branch1.asm`, `query-readback-fallback1.txt`,
`query-resource-writer-xrefs1.json` and `talos-readback-exclusion1.json` under
`artifacts/dx12-research`. Ghidra ran read-only with analysis disabled; PE unwind
ranges and instruction references were used to identify the functions.
This investigation performed no GPU replay and changes no runtime policy.

The workspace reports the first blocking resolve as `MissingData`, including
its execution/source and exact missing count. Before execution 16001 remains
selectable; after 16001 and later prefixes report the gap. Execution 16000 is
still available. Analysis captures and performance require the selected event
to be included even when the output view selects a before-event boundary.
Malformed query records report a separate failure. Existing native deferred
query checks remain as an execution-time guard.

Query destination preflight now also identifies the resource lifetime and its
captured allocation: resource creation, dimension, width, heap type and, for
placed buffers, heap creation/offset/size. These are allocation facts, not proof
that a query result is unused. Talos executions 16001 and 19409 resolve 1,888
bytes each into resource creations 78132 and 12800 respectively. Both are
65,536-byte buffers on READBACK heap creation 42957, at heap offsets 327680 and
458752. The report retains the missing-result status and 140/146 missing slots.
No query is omitted or replaced with an invented value.

CPU preflight rejects missing destinations, non-buffer destinations, unaligned
offsets and writes beyond the selected resource lifetime. Native recording uses
the same overflow-safe size check. Pipeline-statistics-1 results occupy 112
bytes, correcting the previous fallback to eight bytes; this layout check does
not certify mesh-shader query support on the replay device. Unknown result
layouts fail explicitly. These checks follow the destination requirements in
[Microsoft's ResolveQueryData reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata).

A follow-up address audit scanned every byte offset in all 192,535 Talos
serialized packet payloads for values within the two 65,536-byte GPU address
ranges (bases 12696485888 and 12696616960). Its only matches were the respective
GetGPUVirtualAddress return values, at records 78133 and 12801. This audit did
not scan resource contents, establish all indirect dependencies or prove that
the missing historical values can be discarded. It does not authorize partial
query execution or change the full-frame blocker.

Focused evidence: `query-destinations1.txt` records four passing cases (plus
setup/cleanup) in 0.519 s: repeated submissions and resource-pointer reuse,
destination range/layout checks including offsets above 4 GiB, the two real
Talos destinations, and workspace navigation around execution 16001. The
reports are `talos-query-destinations1.json` and `talos-query-addresses1.json`.
All are ignored local artifacts; this validation created no GPU device.
The rebuilt CLI/Worker (build272) also preserve preflight rejection:
`talos-query-destination-preflight1/report.json` returns exit 2, `missing_data`,
`completed: false` and `device_created: false`, with the destination evidence.

Targeted CPU evidence, without GPU replay:

- `query-coverage3`: all submitted query ranges in the three supplied captures.
  BF1's **5** resolves and Hellblade2's **240** resolves have completed query
  coverage in the backend's serialized submission order. This says nothing
  about cross-queue dependency correctness or full-frame image fidelity.
- `dx12-query-coverage3.txt`: the real capture audit, workspace boundary checks
  and a fixture covering repeated submissions, unsubmitted query recordings,
  64-bit heap identities, bounds, truncation and cancellation pass in 0.499 s.
  Earlier retained reports `coverage1/2` exposed the keyframe forward-reference
  omission and the resulting one-slot correction in the first Talos breakdown;
  the accepted figures above use that rule consistently.
- `talos-query-preflight1/report.json`: selected after-execution 16001 returns
  `completed: false`, `device_created: false`, `capability_status: missing_data`
  and the query coverage evidence. The diagnostic run requested the debug layer
  but did not create a device or submit GPU work.
- Build157 includes the final GUI/CLI/Worker integration. The focused fixture
  additionally checks that before-event display selection does not incorrectly
  permit an after-event performance capture.

The index is cancellable and bounded to four million submitted commands and
one million slots per query resolve. It neither synthesizes missing results nor
changes the native query calls. Talos full-frame replay remains unverified and
blocked on result restoration; disabling an unavailable action is not feature
completion.

Reproduce the accepted color boundary (new/empty output directory required):

```powershell
FloraGPA.Worker.exe validate-frame "D:\CDXrepo\captures\Talos_2026_10_18__00_55_08.gpa_frame" --check-initialization --max-bytes 4294967296 --check-execution 16000 --texture-object 1651488160 --debug-device --out talos-color
```

Use execution `12675`, texture object `4980699392`, `--subresource 1`, and
optionally `--before` for the first Draw boundary. Overall validation continues
to return blocked/exit 2 because full feature acceptance is unfinished.
`nativeFrameUploadsAndStates` verifies the real partial upload bytes, upload-heap
state preservation, buffer promotion/decay, depth promotion rejection, and the
per-subresource convergence regression. No broad regression or Hellblade2 GPU
rerun was used for this change.

Additional Ghidra reader RVAs in the same StreamPlayback build/hash recorded
above: `0x67c420` (render targets), `0x67c110` (discard), `0x67bcf0` (depth clear),
`0x6094c0` (root constants), `0x67c2d0` (vertex views), `0x67bdb0` (color clear),
`0x67c8c0` (descriptor heaps), `0x67a0c0` (query heap creation).

### Original-player comparison and isolated DX12 analysis

GPA 2025 R1's actual Talos playback, using `gpa-player --offscreen
--exit-after-frame 1` with the `api-debug-layer:flushafterexecute=true` and
`screenshot:frames=1,path=<output>,async=false` post-layers, also reports the same
140 unresolved occlusion queries. StreamPlayback RVAs `0x632220`
(`ResolveQueryData` execution) and `0x628860` (`CreateQueryHeap` execution)
forward the native calls; these routines do not supply historical results.
This is an observed original-player limitation on this capture, not proof that
the missing values can be reconstructed. Flora's pre-submit guard remains.

The original player's `screen-1.png` and Flora's accepted execution-16000 color
readback have **identical RGB values at all 2,073,600 pixels**. Alpha differs at
957,622 pixels: the original screenshot is entirely opaque, whereas the native
resource retains varying alpha. The screenshot-alpha audit below now establishes
that the screenshot writer forces alpha to 255; screenshots cannot validate raw
resource alpha. RGBA equality and complete-frame correctness are not claimed. The screenshot
layer additionally reports a PRESENT/COPY_SOURCE state error; it is separate
from the 140 undefined-query errors. Local evidence:
`gpa-talos-debug-reference.log`, `gpa-talos-reference1/screen-1.png`, and
`talos-original-color-comparison.json` under the ignored research directory.

`validate-frame --check-initialization --check-execution <execution> --renderdoc
<renderdoc.dll>` now captures the selected native DX12 replay prefix for the
existing isolated `FloraGPA.Rdc.exe` analysis process. Initialization precedes
capture, and diagnostic texture readback follows it. The capture retains needed
resources and gives each submitted list instance its own execution markers;
repeated submissions are not collapsed. Each marker preserves source record,
submission, queue, list generation and ordinal. Unknown helpers stay unmapped.
DX12 execution/object/creation identities in analysis jobs and results are
decimal strings, including values above JavaScript's exact integer range.

Native allocations omit captured `ALLOW_WRITE_WATCH`, a CPU dirty-tracking flag
unused by the read-only replay implementation. Captured resource recipes remain
unchanged and explicit captured patches still restore contents. This avoids a
RenderDoc 1.45 replay failure: its large-upload-buffer conversion to DEFAULT
otherwise retains WRITE_WATCH on CPU-inaccessible memory (`E_INVALIDARG`).
The targeted allocation check verifies flag removal and recipe preservation;
the existing partial-upload check verifies restored bytes and resource states.

Verified with the installed **RenderDoc 1.45 release**, commit
`2fc0bc04cb95499635f63986a55bc6f67849dd9f`, DLL SHA256
`313f450bfa1f7f7d5f50cc7ee80d9f99b68d3fe873e25eb865ff0433e359f7b3`:

- Talos prefix through execution `12675`: all 26 marked command executions
  have unique RenderDoc mappings, including Clear `12667` -> EID 127 and Draw
  `12675` -> EID 151. Synthetic mapping checks cover repeated source records,
  duplicate markers, helper exclusion, uint64 overflow and identities above 2^53;
  existing DX11 command mapping checks also pass.
- VS invocation vertex 0 / instance 0: 20 DXBC instruction-debug states.
  No source debug information is present; no source or call stack is invented.
  Post-VS output contains 8,053 vertices and 42,006 indices, exported as binary,
  CSV and OBJ. The debugger's position register and the first GPU post-VS
  position differ only in Y by `5.960464477539063e-08` (two float32 ULPs);
  other components match. Y is produced by instruction 8's `dp4` and copied
  to the position by instruction 11. Re-evaluating that dot product with
  per-step fused multiply-add rounding reproduces the GPU value; the shader
  permits refactoring. This supports an arithmetic-rounding explanation, but
  the driver's actual instruction lowering has not been inspected. It is not
  a bit-exact pass.
- Depth pixel `(1527,1240)`, mip 0 / layer 1 / sample 0: history identifies the
  exact Clear and Draw executions. RenderDoc's D16 typeless history decoder
  incorrectly returns half-float `6.234375` for raw UNORM bits `17980`.
  Affected fragment pre/post depths are therefore **withheld**, with a partial
  result and explicit depth-test-reason limitation. Separate event-boundary
  GPU readbacks return 0 before the Draw and `17980/65535 = 0.2743572137026017`
  afterward, exactly matching native replay. They do not replace fragment
  history. Boundary reads are limited to single-sample 2D data, 256 events and
  256 MiB total per request; omitted observations have explicit reasons.

Example analysis job for the captured prefix (new output directory required):

```json
{
  "action": "history",
  "renderdoc": "C:/Program Files/RenderDoc/renderdoc.dll",
  "capture": "<validation-output>/native-dx12_capture.rdc",
  "execution": "12675",
  "resource": "4980699392",
  "resource_creation": "167081",
  "x": 1527, "y": 1240, "mip": 0, "layer": 1, "sample": 0,
  "out": "<new-analysis-output>"
}
```

Run with `FloraGPA.Rdc.exe --job <job.json>`. `debug-vertex` uses the same capture
and execution with `vertex` / `instance`; `postmesh` uses `stage: "VSOut"` and
`instance`. Local evidence: `talos-native-rdc2`, `talos-rdc-events3`,
`talos-dx12-history4`, `talos-dx12-vsdebug1`, `talos-dx12-mesh1`, and
`talos-dx12-vs-mesh-comparison.json`. The analysis processes report no loaded GPA
modules and run without Python. This does not yet verify a portable bundle.
`talos-rdc-query-guard1` verifies capture-failure cleanup: the missing historical
query stops frame execution, the active capture is discarded while its device
is alive, and no RDC file or executed Draw is reported. Release CLI, Worker,
GUI and analysis executables build successfully. Only checks directly related
to the mapping, resource-flag failure and selected analysis paths were run.
PS/CS/DXIL validation, complete Resource History,
fragment-depth repair, counters and modification experiments remain unfinished.

### Selected execution in the shared workspace

The existing Replay action now dispatches DX12 selected execution requests to
the isolated native Worker. Switch the API Log to **Submitted command instances**,
locate an execution ID, then choose **Before event** or **After event**. Optionally
select a resource creation in Resources and set its D3D12 subresource index.
Press Replay explicitly; changing DX12 selection does not automatically repeat
GPU work. The allocation budget defaults to 4096 MiB and is editable. The selected
resource creation must match the object's lifetime at the execution boundary.
Missing/partial recordings, buffer or multisample readback, unsupported commands
and missing captured state remain explicit failures or unavailable capabilities.

For Talos's first Draw, use execution `12675`, resource creation `167081` and
subresource `1`. The workspace displays the actual 2048x2048 depth readback before
and after that execution. R16 UNORM and D16/R16 typeless depth textures use a
grayscale display of [0,1]; the unmodified 16-bit values remain in the storage
artifact. BGRA8/RGBA8 display retains stored components. Other texture formats
can produce raw readback without a display image; the report states that limit.
Export Image saves the display PNG. Export Output Storage saves checked native
bytes and a JSON sidecar containing object/creation/execution identities, format,
subresource, row layout and hash. Export uses bounded copying and cancellation.

Equivalent CLI request:

```powershell
FloraGPA.Cli.exe replay "D:\CDXrepo\captures\Talos_2026_10_18__00_55_08.gpa_frame" --event 12675 --id 167081 --subresource 1 --max-bytes 4294967296 --out talos-selected
```

Here `--event` is an **execution instance** and `--id` is a **resource creation
record**. Omit `--id` for execution without texture readback; add `--before` for
the preceding boundary. `--renderdoc <dll>` also captures that native prefix for
isolated analysis. DX11 CLI meanings and behavior remain unchanged. Successful
requests return zero with `completed: true`, `replay_scope: "selected_execution"`
and `frame_complete: false`; this completes the request, not full feature
acceptance. `validate-frame` still reports overall acceptance as blocked.

The UI uses the existing request/session/backend/revision envelope, asynchronous
report/image validation, cancellation and isolated Worker. Resource, subresource,
execution-domain and boundary changes invalidate the displayed result and reject
late output. DX12 controls do not silently pass ignored DX11 adapter/channel/MSAA
options. The actual hardware adapter and Agility SDK appear in the output tooltip.

Targeted evidence: `selectedReplayWorkspace` executed Talos before/after through
the real UI/Worker path, checked the known pixel's displayed values 0 and 70,
replaced an active DX12 session without accepting its result, then verified the
existing GF2 DX11 image hash
`2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`.
The native D16 value is still `17980`, not the 8-bit displayed value 70. Logs and
the offscreen workspace image are in `artifacts/dx12-research/dx12-workspace-replay2.txt`
and `talos-dx12-workspace2.png`. No BF1/Hellblade2 GPU rerun was required for this
UI integration. The Shader Debug and Replay Mesh connection is described below;
successful selected replay alone is not acceptance of those features.

The equivalent CLI run in `talos-workspace-cli1` completed with zero D3D12 debug
errors. `selectedReplayStorageExport` read that actual Worker-compatible report,
validated its display PNG/RGBA pair, exported native storage through the existing
bounded exporter, and verified all 8,388,608 bytes against SHA256
`c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`.
The exported pixel remains exactly `17980`; its sidecar retains creation
`167081` and subresource `1`. See `dx12-storage-export1.txt`. Release CLI, Worker,
GUI and targeted test executables build successfully.

### Selected shader debugging and replay mesh in the workspace

For a submitted direct Draw, the existing Shader Debug VS/PS tabs and Geometry >
Replay Mesh now create an isolated native DX12 prefix capture and dispatch analysis
to RenderDoc 1.45. Direct Dispatch enables the CS tab. Analysis always captures
through the selected invocation, independently of the output texture and its
Before/After readback setting. The native allocation budget still applies.
Changing the execution, session or budget invalidates the analysis context.
The Worker envelope and returned backend/execution are checked before accepting
results; execution and resource identities remain decimal strings, including
values above 2^53. DX11 request fields retain their existing numeric contract.

The bounded prefix cache includes the session, execution, budget, canonical DLL
path and DLL SHA256, so VS debugging and mesh analysis can reuse exactly the same
capture. DX12 results carry an explicit runtime-session capture identity rather
than inventing a capture content hash. Persistent content identity is unfinished.
Pixel History is connected separately below; counters remain unavailable.

`selectedAnalysisWorkspace` exercised Talos execution `12675` through the actual
UI/Worker/RenderDoc path: 20 VS instruction states, no source files, working Step
and `SV_Position.y` Watch (`-0.3396325707435608`), and 8,053 post-VS vertices with
42,006 indices. OBJ export succeeded. The same prefix capture was reused for
both analyses. Changing execution invalidated both results and rejected an old
request. A separate check rejected a mismatched execution above 2^53.
See `artifacts/dx12-research/dx12-analysis-ui1.txt`.

For shaders without source mappings, enter an instruction number and use
**Instruction breakpoint**, then **Continue**. This sets a persistent instruction
breakpoint, separate from Run to and source-line rules. Active instruction
breakpoints appear in the status line; invoking the same action removes one.
They are saved in the existing debug configuration and exported reports. Old
configurations without instruction breakpoints remain readable. Imports validate
the shader identity, actual instruction information and duplicate/bounded rules
before replacing existing state. Missing source and callstack information is
never synthesized. PS/CS, DXIL, additional geometry stages, and complete analysis
coverage still need real sample validation.

The CPU-only `instructionDebugWithoutSources` check reused the actual Talos VS
trace, stopped Continue at instruction 8 in Assembly, round-tripped its config
through disk, rejected invalid/duplicate instructions atomically, read the old
configuration shape, and cleared state on execution change. Capability checks
accepted the Draw and rejected Clear, missing execution and stale session.
Logs: `dx12-instruction-breakpoint1.txt`, `dx12-analysis-capability1.txt`.
Relevant legacy configuration/selection and RDC job validation checks passed;
Release GUI, Worker and targeted tests built successfully (`build60.log`,
`build61.log`). These checks did not replay another GPU frame.

### Pixel History from a native DX12 readback

After selected execution replay with a texture resource, enable **Pixel History**
and click the output image. Native readback now carries its actual mip, layer,
plane, sample count and texture dimension. Picking validates that metadata against
the selected execution/resource lifetime, then queries the same independent
RenderDoc prefix through that execution. The current pick path accepts only
single-sample 2D plane-0 images at **After event**; Before-event picking is rejected
explicitly until a matching preceding analysis boundary is implemented. Coordinates
can also be edited in Pixel History; DX12 event and object fields follow the API
and resource selections, with creation identity supplied separately to the Worker.

The table uses **execution instances**, retains fragment/operation records and
separately labelled observed event-depth boundaries, and displays partial-result
limitations. For D16 it shows “Fragment depth unavailable” and “Depth-test result
unverified”; the separate raw UNORM readbacks are never substituted into fragment
records. Only mapped/selectable rows can navigate, using the submitted-instance
domain and restoring filtered events. Export preserves the full original evidence
and limitation fields. Resource and execution changes invalidate old requests.

`selectedPixelHistoryWorkspace` replayed Talos `12675` / creation `167081` /
subresource `1`, clicked `(1527,1240)`, and obtained two backend records plus two
observed Clear/Draw boundaries. It checked layer 1/mip 0, partial status, unavailable
fragment depths and the separate Draw result `raw_unorm16: 17980`, exported the
report, then located Clear execution `12667` through an active API filter. The first
run exposed a disabled pick action after image validation; the fixed path passed
(`dx12-pixel-history-ui2.txt`). Existing DX11 pixel-selection/stale-result behavior
passed (`dx11-history-selection1.txt`). This is a working partial D16 history path,
not completion of Pixel History or full-frame acceptance.

`pixelHistoryIdentityAndMapping` reused the saved real history report without GPU
work to verify decimal identities above 2^53 through UINT64_MAX, reject numeric or
mismatched execution IDs, keep ambiguous rows non-navigable, invalidate resource
changes, and restore DX11 request fields after switching backend. It passed in
`dx12-history-identity1.txt`. Release GUI/Worker and targeted test builds passed
(`build63.log`, `build64.log`); no BF1/Hellblade2 replay was added for this UI work.

### BF1 command lists recorded in the keyframe

BF1's first frame submission (`364657`) includes lists begun in metadata, including
one begun at `187454` and closed in the frame at `364398`. Its first Draw is source
record `328782`, execution instance `21622`, in a list begun at `328776`. These are
frame executions even though their commands were recorded before the frame's CPU
stream. Rejecting all such lists prevented BF1 from reaching its first Draw.

The native backend now prepares the selected execution plan before initialization,
reconstructs these command lists at their original metadata record positions, and
continues open recordings through the frame stream. This preserves the descriptor
and resource lookup timing instead of replaying old binding commands against the
end-of-initialization descriptor contents. Each submission instance still owns its
own native list and execution markers. Recording a future frame Draw during
initialization does **not** execute it or increment executed-command counters.
Initialization counters now accumulate only the commands actually submitted.
CLI and Worker selected replay both prepare this plan before initialization.

The BF1 prefix also needed explicit handling of Fence/PSO/RootSignature reference
counts and read-only DXGI/device queries. A swapchain interface query must return
the same captured object; unresolved aliases are rejected. These observations do
not synthesize GPU commands. Native buffer inspection can now read a successfully
reached selected boundary, checks the active resource creation, and reports that
boundary/execution with a bounded 256-byte preview. Workspace buffer readback is
still unconnected.

Targeted evidence on the real BF1 capture:

- First Draw executed with zero D3D12 debug-layer errors (`bf1-first-draw2`).
- Its root UAV resolves to creation `11815`, object `864906160`, byte offset `75292`.
  The captured DXBC VS has no inputs or position output and executes
  `store_structured u0.x, l(0), l(0), l(1)`.
- The four-byte GPU readback is `00000000` after initialization and immediately
  before execution `21622`; after that Draw it is `01000000`. SHA256 changes from
  `df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119` to
  `67abdd721024f0ff4e0b3f4c2fc13bc5bad42d0b7851d456d88d203d15aaa450`.
  The before boundary excludes the Draw and still observes zero after the frame's
  CPU patches and preceding copies. Both boundaries report zero validation errors.
  Logs: `bf1-keyframe-recording1.txt`, `bf1-keyframe-before2.txt`.
- The related Talos first-Draw check retains its exact depth hash
  `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`
  and zero validation errors (`talos-planned-recording1`).

The BF1 shader intentionally writes a buffer without producing a render target;
this evidence is not a final-frame image or a comparison against original GPA.
At this checkpoint, complete BF1 replay and RenderDoc mapping of keyframe-recorded
lists were still unverified; the next section records further progress.
No Hellblade2 GPU replay was added for this change. Release CLI,
Worker, GUI and the targeted test executable built successfully (`build69.log`,
`build70.log`). Research inventories are in `bf1-frame-lists1`,
`bf1-first-work.json` and `bf1-first-prefix-cpu.json`; they stay outside Git.

### BF1 submitted work, indirect Dispatch and compute debugging

The native backend now decodes command signatures and replays single-argument
Draw, DrawIndexed and Dispatch signatures, including GPU-generated arguments and
optional count buffers. Buffer ranges, alignment and indirect-argument state are
checked. Root-argument and GPU-address-bearing signatures still fail explicitly;
their address patching is unfinished. BF1's three signatures at records `15483`,
`15525`, `15543` have strides 12/16/20 and null root signatures. This matches the
[D3D12 indirect drawing contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing).
Float/uint UAV clears preserve captured handles, values and rectangles. Float
clear has actual BF1 evidence below; uint clear and indirect Draw variants have
not received separate GPU correctness checks.

Two keyframe dependency problems were exposed by extending the submitted prefix:

- Query heap `0x26ceed280` is used by BeginQuery at `265729`, before its unique
  creation at `346334`. Only a unique keyframe lifetime may satisfy this forward
  reference. Missing historical query values still fail before GPU execution.
- RTV binding `16047` consumes heap creation `166996`, slot `2659`, before its
  descriptor write `16069`. The keyframe repeats that exact snapshot 13 times.
  CPU-consumed views now require recorded contents. Forward restoration requires
  identical descriptor bytes **and** the same resource/counter creation across
  every candidate; divergent writes, unresolved copies and missing slots fail.
  Frame references cannot request a forward snapshot. The index is cancellable
  and bounded to 4,194,304 provenance entries, with at most 32 evidence rows in
  the report. The old undefined RTV caused RenderDoc 1.45 to crash while opening
  `bf1-first-submission3`; the repaired `bf1-consumed-descriptors3` capture opens
  successfully (`bf1-first-submission-events2`). Zero native validation errors
  alone had not detected that missing descriptor content.

OMSetBlendFactor now reads the serialized array length as well as its four floats
(`265884`). Frame CPU queries for timestamp frequency, resource description,
device ownership and list type are decoded separately from GPU work. Interface
queries accept a recorded E_NOINTERFACE/null result or the same object identity;
unresolved aliases fail. Captured timestamp frequency is not used as a native
performance measurement.

Targeted evidence, all using the original read-only BF1 directory:

| Boundary/check | Observed result |
| --- | --- |
| First submitted Draw `21622` | All 14 prefix execution identities mapped; VS trace has 3 states, no invented source (`bf1-keyframe-vs1`). Native UAV write was verified in the preceding section. |
| First direct Dispatch `21895` | RenderDoc event `1075`, DXBC CS hash `e8a1fa7c-e1447c80-05b26856-2c8d5806`, 925 states for group/thread `(0,0,0)` (`bf1-cs-debug1`). |
| CS workspace trace | Breakpoint at instruction 155, step to 156, Watch of both input-block dimensions gives 64; changing the thread invalidates the old request/result (`bf1-cs-ui1.txt`). No additional GPU replay for this UI check. |
| UAV clear `35703` | Resource creation `300425`, object `8697204528`: all 18,800 bytes covering 4,700 floats read back as `1.0`. SHA256 `f9ef289a12f1f8e88c143059b09b92b7c2be1ef4026b0b7420bdf5146675ed07`; zero debug-layer errors (`bf1-views-clear1.txt`). |
| First submission through `37815` | 86 lists, 333 DrawInstanced, 1,208 DrawIndexedInstanced, 123 Dispatch, 11 ExecuteIndirect, 6 BeginQuery/8 EndQuery and one ResolveQueryData; zero debug-layer errors. |
| Indirect Dispatch `35808` | Actual generated Dispatch is RenderDoc event `42909`, with groups `(1226,1,1)`. The isolated CS debugger returns 256 states for group/thread `(0,0,0)`, shader hash `5297230d-b11ed8d4-8c1b1ca9-2d9bfa5f` (`bf1-indirect-cs1`). |
| Last indexed execution `54186` | Submission `392108` reached, cumulatively 691 DrawInstanced, 2,415 DrawIndexedInstanced, 246 Dispatch, 22 ExecuteIndirect and 5 ResolveQueryData; zero debug-layer errors (`bf1-last-execution4`). |
| Last submission color | Native GPU readback of swapchain buffer creation `11395`, object `2119326144`, subresource 0: 1920×1009 RGBA8, 7,749,120 bytes; SHA256 `73450f309a632a1b102c92eefb173412eca7e9bfd015dab4691ce1503171ecaf`. Scene, weapon and HUD visible (`bf1-last-color1/texture.png`); zero debug-layer errors. |

Indirect mapping now keeps the ExecuteIndirect wrapper events in `api_event_ids`
and selects only generated work. All 16,207 first-submission execution identities
are uniquely selectable in this capture after that fix (`bf1-indirect-cs1`). Zero
generated actions remain unrepresented; multiple generated actions retain their
reverse provenance without silently selecting one. CPU fixtures cover those
cases, duplicate submitted-list markers and the existing DX11 provenance path
(`bf1-indirect-mapping1.txt`). The isolated BF1 VS/CS reports list no loaded GPA
modules. Native descriptor fixtures check repeated identical snapshots, divergent
snapshots, missing slots and rejection of forward restoration from a frame call.

This is not full feature acceptance. The stream has Present records `390416` and
`418061`; the last indexed submission precedes the latter and replay currently
stops at the requested execution boundary. Present/frame selection and original
GPA image/intermediate-resource comparison remain to be established. The color
image above is an actual GPU result, not the directory preview. PS debugging,
BF1 history/geometry, performance, experiments and portable-package acceptance
are unfinished. At this checkpoint indirect CS was exercised through the isolated
job interface; workspace support is recorded in the next section.

Release CLI, Worker, GUI, isolated RenderDoc service and the focused test targets
built successfully (`build80.log`). Final signature-decoding/CS-control checks
pass (`bf1-final-cpu1.txt`). The related Talos first-Draw replay still produces
exact SHA256 `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`
with zero debug-layer errors (`talos-consumed-descriptors1`). No Hellblade2 replay
or broad regression run was added. Captures, images, traces and crash-reproduction
artifacts remain outside Git.

### BF1 workspace backbuffers, indirect analysis and original-player comparison

The resource catalog now includes swapchain buffers from the existing checked
`BackBufferIndex`. These entries retain their GetBuffer record, captured object,
swapchain generation, index and queue; their descriptions are explicitly derived
from captured swapchain/GetBuffer data. No directory preview supplies their
contents. They participate in normal resource selection, capability queries,
Worker replay and raw-output export. BF1 has three such entries. Selecting buffer
creation `11399` (index 0) and execution `37815` in the main window produces a
1920×1009 native RGBA image, SHA256
`cbd80034812485b2ee6c5a25d12896cc1671bb0e95df461fd72fa198c73a894b` in the observed
run (`bf1-first-color-workspace1.png`). This hash is evidence, not a deterministic
golden for this capture.

Submitted events now expose a work kind resolved from the command signature at
the recording boundary. The workspace enables CS for simple indirect Dispatch,
and VS/PS/Replay Mesh for simple indirect Draw signatures. Missing/invalid
signatures report missing data; unsupported root arguments/layouts report not
implemented. Exactly one generated action is still required by the analysis
backend. Zero or multiple generated actions report an explicit selection error;
they are not successful empty traces. Actual BF1 execution `35808` now completes
the main-window selection → native Worker recapture → isolated RenderDoc CS
debugger path, returning 256 steps at RDC event `42909`. Switching to Draw
`21622` clears that CS trace and changes the enabled stages. This is verified in
`bf1-indirect-workspace1.txt`; indirect Draw UI behavior has not received separate
GPU evidence.

GPA 2025 R1 was run offscreen with `--exit-after-frame 2` and synchronous screenshot
post-layer frames `1;2`, followed by one repeat of frame 2 to investigate the
observed differences. GPA reports playback starting on captured frame `14773`.
The first native submission ends its output transition at record `364404`
(buffer creation `11399`), before Present `390416`; the second ends at `391654`
(buffer creation `11395`), before Present `418061`. Their native images match
the corresponding GPA screenshot's scene/particle frame, while cross-comparing
frames gives RGB MAE about 10.38. Quantitative comparison covers 1,937,280 pixels:

| Comparison | Exact RGB pixels | RGB mean absolute channel error | Maximum channel difference |
| --- | ---: | ---: | ---: |
| Native first submission vs GPA screenshot 1 | 1,868,607 | 0.0244121 | 4 |
| Native last submission vs GPA screenshot 2 | 1,881,828 | 0.0285946 | 13 |
| GPA screenshot 2 vs its repeat | 1,916,189 | 0.0146083 | 14 |
| Native last submission vs repeated GPA screenshot 2 | 1,892,450 | 0.0167906 | 9 |

This establishes observed frame correspondence, **not exact replay fidelity**.
Original playback varies between runs. However, 35,403 native-differing pixels
were unchanged between those two original runs, so this repeat does not explain
all differences. Their producing passes remain to be isolated. The original
screenshots are opaque while native alpha differs at 1,887,436 / 1,887,447 pixels
for the first / last output. The screenshot-alpha audit below establishes that
GPA forces screenshot alpha to 255; this difference is not a raw-resource alpha
comparison. GPA also logs a missing capture-root D3D12Core.dll and
a captured/replay GPU-ID mismatch; neither is treated as proof of the difference's
cause. Local evidence: `gpa-bf1-reference1.log`, `gpa-bf1-reference2.log`, both
screenshot directories, `bf1-original-color-comparison1.json`,
`bf1-original-repeat-comparison1.json`, and `bf1-first-color-comparison1.json`.

The targeted main-window backbuffer test also ran the existing GF2 DX11 replay,
event selection and geometry entry, then switched among the three DX12 sessions
and replaced a running DX11 session. All passed, including unchanged GF2 RGBA
SHA256 `2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`
(`bf1-backbuffer-dx11-workspace1.txt`). This did not replay Hellblade2 or run a
broad regression suite.

Release CLI, Worker, GUI, isolated analysis and focused tests build successfully
(`build83.log` through `build85.log`). The small missing-signature/root-argument fixtures
and existing selected-analysis capability checks pass (`bf1-analysis-gaps1.txt`).

### BF1 color history and pixel shader invocation

RenderDoc 1.45 queried backbuffer object `2119382448`, creation `11399`, through
execution `37815` at `(960, 500)`, mip/layer/sample zero, using the existing
`bf1-consumed-descriptors3/native-dx12_capture.rdc`. The query returned four
mapped rows, without loading GPA modules (`bf1-color-history1/result.json`):

| Execution | Source record | RenderDoc event | Primitive | Observed color result |
| ---: | ---: | ---: | ---: | --- |
| 37273 | 363202 | 47373 | 0 | RGBA8 `(0,0,0,0)` to `(34,66,73,57)` |
| 37309 | 363255 | 47485 | 1 | Unchanged; backend reports stencil-test failure |
| 37326 | 363288 | 47536 | 1 | Unchanged; backend reports stencil-test failure |
| 37343 | 363321 | 47587 | 1 | Unchanged; backend reports stencil-test failure |

The final RGBA8 values exactly match this pixel in the independently read native
image `bf1-first-color-workspace1.png`. This verifies one color history chain;
the depth/stencil value fields accompanying these color records have not been
validated, and the reported stencil failure reasons are backend observations.

The history toolbar now offers **Debug PS** for mapped DX12 Draw records with a
known primitive. It restores a filtered event's visibility, selects its execution,
and transfers X/Y/sample/primitive into the existing PS panel. The optional
primitive field is passed through the isolated job and `DebugPixelInputs`; CLI
analysis also accepts `--primitive`. Changing event or picking a new pixel clears
the previous primitive. Primitive selection does not select a fragment index or
an instance, and resource operations/synthetic depth boundaries do not enable
this action.

The main-window test used the real history result, selected execution `37273`,
then generated and analyzed that native prefix through the Worker. It returned
37 PS states, mapped to RenderDoc event `47373`, with no GPA modules loaded
(`bf1-color-ps-workspace3.txt`, exported `bf1-color-ps-workspace1.json`). Shader
hash `3e0cb770-f5d412fc-8252836d-963dedeb` is DXBC `ps_5_0`, sampling the main,
distortion, bloom and 3D color-grading textures. Its final `SV_Target` equals the
history row's GPU shader output exactly in all four components:
`(0.13182269036769867, 0.2581216096878052, 0.2846723198890686, 0.2233850210905075)`.
Source information is absent, so this remains instruction-level debugging.

The initial targeted UI check exposed a selected-history invalidation callback
accessing cleared rows before GPU analysis began. The row validity guard fixes
that fault; the same workspace path subsequently passed. Tests reuse the recorded
trace for breakpoint, Watch and invocation-invalidation checks instead of
replaying the GPU again: instruction breakpoint 34, the following step, exact
`SV_Target` Watch values, primitive-change trace invalidation, and clearing a
selected history result all passed (`bf1-ps-instructions1.txt`). Existing DX11/12
invocation-selection checks and unsigned job-option checks passed
(`ps-primitive-selection2.txt`, `ps-primitive-jobs1.txt`). Release GUI, CLI, Worker,
isolated analysis and focused tests build successfully (`build90.log`).
This work does not establish full Pixel/Resource History,
all PS invocations, DXIL debugging, or full-frame replay fidelity.

### DX12 resource history in the shared workspace

**Resource History** now queries the selected resource creation through a selected
execution using the isolated native RenderDoc service. The same native-prefix
cache, cancellation, request/session/revision identity checks and stale-result
rejection used by the other analyses apply. Texture and buffer resources are
accepted; this does not require the currently texture-only workspace readback.
The CLI analysis action `resource-history` is also available for existing RDCs.

The report and view deliberately distinguish:

- Original captured resource creation, including GetBuffer/swapchain provenance.
- Replay initialization chunks, such as native CreateCommittedResource, SetName
  and Initial Contents, which have no fabricated original execution identity.
- Execution usage records: bindings are potential accesses, while Copy/Clear,
  CPUWrite and Barrier records are recorded operations. Neither proves a content
  change; each row explicitly reports `content_change: not_measured`.

Mapped rows preserve source record, queue, command-list generation, submission
and list ordinal. The workspace attaches the original captured call. A checked
decoder now exposes transition, aliasing and UAV barrier arguments, preserving
64-bit resource identities as decimal strings. For matching transition barriers,
the table displays the captured subresource, before/after state bits and flags.
Other ranges remain unknown instead of being treated as whole-resource accesses.
Fence-dependency history, complete resource lifetime/alias analysis and observed
content modifications remain unfinished.

BF1 backbuffer object `2119382448`, creation `11399`, through execution `37815`
has 60 indexed uses in the existing independently generated capture: 56 color
target bindings and four barriers. Every use maps uniquely. These are not 56
observed writes; the previously verified pixel history at `(960,500)` has only
four fragment records. The captured transitions for this backbuffer are:

| Execution | Source record | Subresource | State before → after |
| ---: | ---: | --- | --- |
| 36771 | 364562 | All | `0x0 → 0x4` |
| 37357 | 363338 | 0 | `0x4 → 0xc0` |
| 37440 | 363459 | 0 | `0xc0 → 0x4` |
| 37815 | 364404 | 0 | `0x4 → 0x0` |

All four carry zero flags. The last transition explicitly names subresource zero;
the initial assumption that it covered all subresources failed the targeted check
and was corrected against the captured packet. Evidence:
`bf1-resource-history1/result.json` and the source records above.

The main-window buffer path was tested using BF1 creation `11815`, object
`864906160`, through execution `21622`. The Worker generated that native prefix
and queried it successfully without GPA modules. The report contains an unmapped
CPUWrite at RenderDoc event `186` and a `VS_RWResource` binding at event `238`,
mapped to execution `21622` / source `328782`. The initialization write is not
attributed to the adjacent Draw. Earlier native buffer readback separately proved
that this Draw changes the root UAV DWORD from zero to one; the usage query itself
does not measure that change. Original creation details, filtering/locating the
Draw and rejecting the old request after switching to Talos all passed.

Local evidence: `bf1-buffer-history-workspace1.json` and
`bf1-resource-history-workspace1.txt`. The same focused run verified the four real
barrier records, truncated-barrier rejection and decimal identities above 2^53.
The final CPU-only display check also passed for the captured all-subresources
and subresource-zero transitions (`bf1-resource-history-display1.txt`). Release
GUI, CLI, Worker, isolated analysis and focused tests build successfully
(`build95.log`); the CLI help exposes the new analysis action.
Full Resource History acceptance remains open, including capture synchronization,
subresource usage for bindings, complete initialization provenance and actual
modification observations. No full-sample replay suite was run for this change.

### Native selected-execution GPU metrics

The first DX12 RenderDoc counter probe exposed an error-handling defect:
`FetchCounters` returned no values while the controller held a fatal error. The
adapter previously reported this as success with an empty array. It now checks
`GetFatalErrorStatus` and rejects an empty measurement instead. A targeted retry
confirmed RenderDoc 1.45's failure at stable-power-state setup: it reports that
Windows developer mode is required (`bf1-dispatch-counters2/result.json`). Its
catalog also reports the missing Nsight Perf SDK library for NVIDIA counters
(`bf1-dispatch-counters1/result.json`). Neither condition establishes a GPU query
hardware limitation, and no system settings were changed.

An independent native D3D12 query path now measures the selected execution in its
own submitted command-list instance. Timestamp queries bracket the command;
pipeline-statistics queries are also collected on direct lists. Results are
resolved to a small readback buffer and read only after the replay queue fence
completes. The duration uses that same queue's timestamp frequency. Raw timestamp,
frequency, statistics and execution identities remain decimal strings, preserving
values above 2^53. Compute lists currently provide timestamps only, with pipeline
statistics explicitly unsampled; copy-list queries are not implemented.

The existing **Replay Metrics** panel uses this native Worker path for DX12,
independently of its RenderDoc library setting. It shows the source and units,
supports export and accurate execution navigation, and invalidates metrics when
the selected execution changes. Worker response identity checks remain in force;
exports carry the runtime session/request/experiment identity. The DX11 RenderDoc
path and its selection-versus-frame behavior are retained. Native measurement is
also available through `replay <directory> --event <execution> --measure-event`.
Measurement requires an after-event boundary and cannot currently be combined
with `--renderdoc`, so profiling helpers cannot contaminate original-event markers.

BF1 execution `21895`, source `274044`, was measured through the actual workspace
Worker with its RenderDoc path deliberately set to a nonexistent file:

| Observation | Value |
| --- | --- |
| Capture queue / command-list type | `861708976` / DIRECT |
| CS invocations | `4096` |
| Expected invocation count from decoded dispatch and shader | `8 × 8 × 1` groups, `8 × 8 × 1` threads = `4096` |
| VS invocations | `0` |
| Raw timestamp begin / end | `1792340049618257664` / `1792340049618262272` |
| Queue timestamp frequency | `1000000000` Hz |
| Single measured interval | `4608` ticks = `4.608 µs` |

Evidence: `bf1-native-metrics-workspace1.json` and
`bf1-native-metrics-workspace1.txt`. The test also checks the displayed metric
count, filtering/locating the original execution, and stale-result rejection on
event change. This is one instrumented sample, without explicit warmup repetitions
or stabilized clocks; it is not an original-application performance baseline,
full-frame timing, or a calibrated multi-queue timeline. Draw-specific statistics,
compute-list timing, cross-queue synchronization, repeat sampling and vendor
hardware counters remain to be verified or implemented. No missing metric is
replaced with an estimate.

The saved measurement was reused for CPU-only display and identity checks:
decimal execution IDs above 2^53 and UINT64_MAX count text round-trip exactly,
native response identities survive conversion for the panel, and stale execution
results are rejected (`native-metrics-identities1.txt`). Existing DX11 exact-value
and selection-invalidation checks pass (`dx11-counters-compat1.txt`). Release GUI,
CLI, Worker, isolated analysis and focused tests build successfully (`build100.log`,
`build101.log`). These checks did not repeat the GPU measurement.

### Hellblade2 first submitted Draw and DXIL pixel debugging

The first frame submission is record 578777 on queue 2131165365824. Its first
list contains timestamp/barrier/timestamp commands (executions 9701-9703).
The next list, 2152464446256, generation 2, ordinal 1, contains the first Draw:
execution **9731**, source record **577579**, six indices and one instance.
This is a selected submitted prefix, not a complete Hellblade2 frame.

The native recorder now restores `OMSetDepthBounds`, `RSSetShadingRate` and
`RSSetShadingRateImage`, queries their device capabilities and rejects unavailable
features. The sample uses depth bounds [0,1], rate 1x1, combiners [PASSTHROUGH,MAX]
and a null shading-rate image. Nondefault rates and nonnull images do not yet have
GPU evidence. Queue/device `QueryInterface` observations accept only an unchanged
identity or `E_NOINTERFACE` with no returned object; aliases still fail. Captured
queue/descriptor-heap references retain replay-owned native objects. Reserved
resources no longer call `GetHeapProperties` during state decay: that query is
invalid for them and previously produced three validation errors.

GPA 2025 R1 `StreamPlayback.dll` (same version/hash recorded above) reader RVAs:
`0x53ba30` depth bounds, `0x67bb40` shading rate plus optional two-entry combiner
array, `0x53bda0` shading-rate image, and `0x53a130` queue interface query.
Playback RVAs `0x62d550`, `0x62efe0`, `0x62f0d0` forward the three restored states.
Evidence: `hellblade-draw-state-readers1.txt` and real records 577565-577567.

CPU `WaitOperation` stores a captured event handle (writer RVA `0x4f61d0`);
Hellblade2 handle 3964 also appears in `FenceSetEventOnCompletion` record 578875.
The first prefix includes waits 577992 and 578048 on thread 29988. After these
waits, through submission 578777, that thread only observes Fence completion.
The original observation-wait guard proved this bounded property before initialization and
reports the two waits explicitly. It does not synthesize an event signal or
weaken GPU queue/Fence checks. Any later state-changing operation on the same
thread, including in another stream file, failed preflight. The next milestone
below restores the known Fence dependency at registration 578875 and extends
the prefix to submission 579109; general CPU event scheduling remains unfinished.

The native before/after requests used SDK 616, the NVIDIA RTX 5090 Laptop GPU,
a 12 GiB allocation budget and the debug layer. Both reached execution 9731 with
**zero validation errors**. Initialization still executes 26 captured submissions,
8,192 tile mappings and restores resources by bounded mapped reads. Peak accounted
native allocation is 5,238,685,696 bytes; this is not a process-memory measurement.
The existing workspace budget control must be raised from 4096 MiB for this sample.
No complete-frame residency or cancellation acceptance is claimed.

Target object 2152470267632, creation 503905, subresource 0 is a 512x512
`R16G16B16A16_FLOAT` texture. Each raw readback contains 2,097,152 unpadded bytes:

| Boundary | SHA-256 |
| --- | --- |
| Before 9731 | `1dae70fd20aa8420722ee30f924d254f06a0e1efb300328b6898096d67e6fee2` |
| After 9731 | `97a0605a8e4f0a3cdfbe99e5001ec193c0bfa47ab31d76510000d04ef99e9e7d` |

123,029 pixels change. This texture contains negative and HDR values; it is
exported as raw float storage, with `display_interpretation=unavailable`. No
preview PNG is used as replay evidence. The native RenderDoc-captured run produces
the exact same after hash. Evidence: `hellblade-first-draw3`,
`hellblade-first-draw-before1`, `hellblade-first-draw-rdc1` (742,457,018-byte RDC),
and `hellblade-first-draw-difference1.json`.

RenderDoc 1.45 maps all 31 marked executions in this prefix uniquely; 9731 maps
to RDC event **418**. At pixel (169,269), sample 0, Pixel History reports primitive
1 with one mapped modification. Before/after RGBA values exactly match the
independent native readbacks:

- Before: `(0.5224609375, -0.81787109375, 427.75, 0.4306640625)`.
- After: `(0.51123046875, -0.82080078125, 428, 0.419677734375)`.
- Shader output: `(0.51123046875, -0.82080078125, 428.224609375, 0.41978317499160767)`.

The DXIL SM6.6 PS, hash `9b9ce25ad2594828fe1a0767a1cb6b62`, returns **276 debug
states**. Its final `SV_Target` equals all four GPU history `shaderOut` values
exactly. The difference between shader output and stored target is present in the
GPU history itself; the raw native target agrees with `postMod`. No shader PDB or
source files are available. The backend retains `MainPS` and variable mappings;
the UI correctly offers instruction debugging without fabricated source files.
No GPA modules are loaded in these isolated analysis jobs. Evidence:
`hellblade-dxil-ps1`, `hellblade-first-pixel-history1`, and the compact comparison
`hellblade-first-draw-validation1.json`.

CPU-only panel verification reuses the saved trace: instruction breakpoint 273,
step to 274, `SV_Target` Watch, and stale-result rejection after changing pixel
all pass. Wire truncation/array-length checks, cross-file wait guarding, and the
then-blocked next-submission dependency are covered by focused tests. This does
not yet verify Hellblade2's complete workspace request flow, VS/MS debugging,
geometry, full histories, performance or modification experiments.
Release GUI, CLI, Worker and the focused test target build successfully
(`build106.log`). All four focused cases pass (`hellblade-final-checks1.txt`);
these final checks reuse the trace and inspect CPU records without another GPU
replay. No unrelated DX11/Talos/BF1 GPU regression run was added for this change.

### Hellblade2 CPU Fence completion and DXIL compute debugging

At this milestone, `planCpuWaits` associated a single captured `SetEventOnCompletion` registration
with its subsequent wait. A null handle represents a synchronous completion wait.
The native executor verifies the actual Fence value before passing that boundary;
captured handles are never passed to Windows. Missing registrations permit only
Fence observations/reference operations until a known completion boundary on the
same captured thread. Mutations before that boundary, overlapping registrations,
failed/truncated registrations and unscheduled producers fail explicitly. This
does not restore arbitrary event reset/lifetime behavior or a complete CPU scheduler.
Captured allocator resets validate object identity and HRESULT; each submitted
recording still owns a fresh native allocator. The later submitted-queue drain
policy below supersedes this single-registration/observation-only restriction.

GPA 2025 R1 `StreamPlayback.dll`, version/hash recorded above: reader RVA
`0x67b010` reads Fence, value, event handle and HRESULT; execution RVA `0x62cb70`
forwards the registration (`hellblade-fence-event-readers1.txt`). In the sample,
queue Signal 578825 targets Fence **2131166327360**, value **449714**.
Registration **578875** binds that pair to handle 3964; wait **579052** consumes it.
The native Fence actually reports **449714** at that wait. The report separately
lists three preceding observation-only waits and one verified Fence wait.

One serial native run, `hellblade-second-submit1/validation.json`, reaches execution
**10135** (source **578172**) in submission **579109**, list 2144009157136,
generation 2, ordinal 1, after a `Dispatch(6,1,1)`. It executes the entire first
frame submission and the selected second-submission prefix: ten Draw calls,
50 Dispatch calls, 39 frame buffer copies and two query resolves. SDK 616 and the
D3D12 debug layer report **zero validation errors**. The 12 GiB allocation budget
has peak accounted allocation 5,238,685,696 bytes. This is not a full-frame or
process-memory acceptance result. The diagnostic command's overall status remains
blocked because complete-frame acceptance is unfinished; the selected boundary
is explicitly reported as reached.

The resulting 888,337,182-byte native RenderDoc capture maps all **435** marked
executions uniquely. Execution 10135 maps to RDC event **1814**. RenderDoc 1.45
returns **715** instruction states for group (0,0,0), thread (0,0,0), DXIL SM6.6
CS hash `4dd2a9db58a8cac1b38e542290c4ca45`, with 64x1x1 threads per group.
No PDB/source files or GPA modules are present. This invocation does not reach
the shader's atomic UAV write; neither UAV write correctness nor original GPA
output equivalence is claimed (`hellblade-dxil-cs1/result.json`).

The trace exposed a Watch gap: typed SSA values existed in the instruction state
but lacked source mappings. Watch, expression evaluation and conditional
breakpoints now use typed instruction values when no source declaration owns the
name. Undefined/unavailable source declarations retain priority. Instruction
values do not become fabricated source variables. Typeless registers and matrices
are not inferred; unsupported 64-bit expression types fail explicitly without
narrowing or floating-point conversion. Variable removal and backward stepping
restore the correct Watch availability.

Focused CPU checks reuse saved traces: Hellblade2 instruction breakpoint 1677,
step to 1678, `_1509` Watch = 8193, declaration priority and stale invocation
rejection; existing BF1 CS Watch compatibility; event-plan boundaries and malformed
registrations; shared model lifetime/configuration checks. All ten test cases pass
(`hellblade-cpu-events-final2.txt`, `debug-watch-final2.txt`). Release GUI, CLI,
Worker and both focused test targets build (`build110.log`, `build111.log`). No
additional DX11/Talos/BF1 GPU replay was run. Subsequent indirect calls, complete
Hellblade2 execution, geometry, performance, experiments and portable acceptance
remain unfinished.

### Hellblade2 indirect mesh dispatch and MeshOut geometry

The second submission contains 42 indirect calls: 22 compute dispatches and 20
mesh dispatches. Signature creation 1296, object `0x1f0337daf80`, has one
`DISPATCH_MESH` argument, 12-byte stride, node mask 1 and no root signature.
The first mesh call is execution **10267**, source **578355**, submission 579109,
using argument buffer 2144426640464 at byte offset 16, maximum count 1, without a
count buffer. Native command signatures now support this address-free layout and
check device MeshShaderTier. Root-argument/address patching remains unsupported.
The layout and API path follow the [Microsoft mesh shader specification](https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html).

One native prefix run (`hellblade-first-mesh1`) reaches that event with SDK 616,
MeshShaderTier 10 (tier 1), and **zero debug-layer messages/errors**. The prefix
contains 65 direct Dispatch calls, 11 direct Draw calls and three indirect calls.
This is not all of submission 579109 or the complete frame. Its RenderDoc capture
is 931,226,507 bytes. All **567** marked executions map uniquely. The first two
indirect calls generate `Dispatch(145,1,1)` at RDC events 2109 and 2153; execution
10267 generates **`DispatchMesh(795,1,1)`**, RDC event **2215**
(`hellblade-first-mesh-inventory1`). MeshDispatch is classified separately from
compute Dispatch, and its generated action is selected instead of either API
wrapper. Group dimensions are retained in the analysis action inventory; the
backend reports unknown thread dimensions as zeros, not as inferred thread counts.

Geometry > Replay Mesh now selects **Mesh output (all meshlets)** for a mesh
execution. Draw-instance selection is disabled for this path; ordinary Draws keep
their VS/GS output choices. The isolated analysis service requests `MeshOut` and
exports positions, rebased triangle indices, CSV and OBJ. Meshlet counts bound the
position readback and exclude adjacent primitive attributes and indices from the
vertex table. Empty output, inconsistent counts, unsupported topology, oversized
storage and out-of-range triangle indices fail explicitly.

The first extraction exposed a backend metadata inconsistency: the reported
per-primitive offset is 1,561,728 (`97608 * 16`), whereas the compacted vertex
region occupies 5,856,480 bytes (`97608 * 60`). Local RenderDoc D3D12 post-VS code
confirms that primitive data follows compacted vertices, while its reported offset
uses primitive stride times vertex count. The implementation uses meshlet vertex
counts and vertex stride, checks the combined regions against index offset
7,404,176, retains the reported offset for diagnosis, and **does not export or
interpret per-primitive attributes**. The same saved capture was reused for the
corrected extraction; no second native prefix replay was needed.

`hellblade-mesh-output2` and `hellblade-mesh-validation1.json` provide:

| Observation | Value |
| --- | --- |
| Meshlets | 795 |
| Vertices / stride | 97,608 / 60 bytes |
| Indices / triangles | 290,193 / 96,731 |
| Position components | float32 x/y/z/w; all finite, all w = 1 |
| Indices outside their own meshlet vertex range | 0 |
| Vertex storage SHA-256 | `d252e09b196bef61a6920990dec2e8f88bc496995a8dc6135cc411ffa17b6b9b` |
| Index storage SHA-256 | `c6e4f38810301e0d330f9040b03a7c18a6761cb4b9b670561006061ad0172caf` |
| OBJ SHA-256 | `4057bf6d5ef6389bd42abadb42c08b6fbd83daa505c5ea02644052b3588a7d06` |

No GPA modules load in the analysis processes. CPU-only workspace checks load the
real saved output, verify all 97,608 table rows and byte-identical OBJ export,
reject the stale result after selecting CS 10135, and restore VSOut/instance
controls when selecting Draw 9731. Signature classification, unsupported indirect
layouts and exact action mapping also pass (`hellblade-mesh-final1.txt`,
`hellblade-mesh-mapping2.txt`; six focused cases total). GUI, Worker, CLI, isolated
analysis and focused test targets build (`build112.log` through `build116.log`).
The new extraction required one native capture, one inventory query, and two
geometry queries, with the second query fixing the concrete layout failure.

This verifies one MeshOut position/index path and its workspace display/export,
not full original-GPA geometry equivalence, every output attribute, meshlet
filtering, AS/MS shader debugging, the complete workspace worker request cycle or
the remaining indirect calls. Those remain unfinished. Existing VS/PS/CS debugging
is not advertised for mesh shader invocations.

### Hellblade2 last indexed submission and RGB10 output

The full directory contains 46 CPU waits, including repeated waits without a new
registration and multiple Fence registrations on handle 3964. Creation/reset and
timeout results for that Win32 event are not recorded. Replay now explicitly
serializes all previously submitted native queues at each captured CPU wait,
using replay-owned completion fences and cancellation-aware polling. Captured API
arguments and mapped writes are replayed; the original CPU application is not
re-executed. This is a conservative GPU serialization policy, not reconstructed
Win32 event semantics or a faithful concurrency/performance timeline.

Registrations since the preceding wait remain diagnostic annotations, including
all overlapping registrations. Reports retain actual native completion values and
whether each condition was reached; an unregistered wait never invents a Fence
dependency. Null-handle `SetEventOnCompletion` remains a synchronous wait and
requires its exact Fence condition. GPU queue Wait packets retain their separate
producer checks: unscheduled producers still fail. The report declares
`scope: serialized_submitted_queue_barriers` and `full_event_semantics: false`.

This exposed a real placed-buffer alias at record **587987**. Creations 571474,
571692 and 572348 have widths 49,920, 276 and 32,772 bytes, respectively, but share
heap `0x1f34678c0b0`, heap offset `0x7540000`, and captured GPU base
`0x135d940000`. Native address binding now accepts multiple candidates only after
checking the same heap lifetime, same heap byte, sufficient ranges, and identical
actual native GPU addresses. Other overlaps still fail. Resource-identity lookup
remains ambiguous; histories/debugging do not arbitrarily pick an alias owner.

Frame command-allocator creation is published at its original CPU boundary
(first encountered at 594770), retaining a fresh native allocator per submitted
recording and rejecting duplicate captured allocator lifetimes. Resource AddRef,
fullscreen-state and heap-description observations are decoded and checked.
They do not create a display window or alter replay ownership. GPA 2025 R1
`StreamPlayback.dll`, version/hash recorded above, provides fullscreen reader
RVA **0x5e3e80**, heap-description reader **0x5c2960** and its optional 48-byte
description helper **0x5b61d0**. Local evidence is in
`hellblade-fullscreen-decoder2.txt` and `hellblade-heap-description-helper1.txt`.

`hellblade-remaining-prefix6` reaches execution **37104**, submission **613374**:
all **125 frame submissions**, following 26 initialization submissions. Executed
commands include 1,122 direct Draws, 882 direct Dispatches and 1,270 indirect API
calls (not a count of the GPU-generated indirect actions). All **32 CPU waits**
through this boundary drain; all **20 annotated Fence conditions** are reached.
The entire directory has later CPU records, including Present, beyond this
selected boundary. This does not certify complete frame semantics.

SDK 616 on the RTX 5090 Laptop reports **zero D3D12 errors and 560 warnings**:
341 overlapping GPU-address bindings, 123 waits for Fence value zero, 68 buffer
initial-state notices, 21 missing RTV notices, four unbound vertex-buffer notices,
two missing clear-value notices and one missing DSV notice. These warnings remain
visible. Peak resource allocation is **5,238,685,696 bytes** under a 12 GiB budget;
61 heaps and 4,879 resources were created, with 2,489 resources resident at the
boundary. The alias checks succeeded for 341 bindings. None of these observations
alone establishes output fidelity or alias-state correctness.

The last recorded `GetCurrentBackBufferIndex` returns 3. An initial readback of
serialized buffer 3 found no frame-thread writes and returned zero
(`hellblade-last-color1`); treating these two indices as equivalent was incorrect.
Following actual GPU commands instead identifies CopyResource **613353**, copying
to object **2149283441408**, creation **506294**, swapchain buffer index 1. Readback
after execution 37104 produces the visible scene, with the character, rocks and
letterbox bars, from that resource. Later capture-layer analysis established that
GPA rotates serialized GetBuffer indices when synthesizing restoration records;
the live observation and serialized index belong to different domains. See the
original-presentation comparison below. Resource identity, rather than the live
index, determines the copied output.

`hellblade-last-color3` reads **1920 x 1200 R10G10B10A2_UNORM**, subresource 0,
9,216,000 unpadded bytes. Native display now reuses the existing integer UNORM
conversion for RGB10/alpha2 to RGBA8; it applies no color-space transform or HDR
tone mapping. Raw storage remains unchanged. A CPU comparison against the saved
raw bytes verifies every displayed pixel exactly; 1,541,760 pixels have nonzero
RGB and every alpha component is 3. Evidence hashes:

| Artifact | SHA-256 |
| --- | --- |
| Raw GPU readback | `08e46411da923de9f08a5db1f240be69684ff2804f61b8d1190c88ae4c6ec1f0` |
| Native display PNG | `8484d044b185c291e950f36d445c414ba044908ff4e0cf14a26a96dcb24c4384` |

The preceding raw readback (`hellblade-last-color2`, before adding display) has
hash `17cb0fd4a855eba921464c46046a0e4f1039ad7e8d00f13138ce4fd447cc782b`.
Comparing these already available outputs finds **10,593 changed pixels** out of
2,304,000; maximum absolute RGB differences are **3, 3, 4** in 10-bit units,
with no alpha changes. The difference is **unexplained**, not accepted as a
tolerance or attributed to a particular GPU operation. No repeatability sweep
was run (`hellblade-last-color-inspection1.json`). Original GPA comparison,
intermediate-resource fidelity and the source of this variation remain unfinished.

Four targeted CPU cases pass: real alias candidates/range/identity ambiguity,
single/overlapping/repeated/null-handle wait plans and malformed input, the real
46-wait capture plan, and cross-thread/file boundaries with cancellation
(`hellblade-sync-alias-cpu2.txt`). GUI, CLI, Worker and focused tests build through
`build122.log`. Each GPU rerun addressed a concrete decoder/address/allocator
failure, selected the actually written output, or verified the new RGB10 display.
No DX11, BF1 or Talos GPU rerun was added. No new full-frame RenderDoc capture,
original-player comparison, experiment or portable acceptance is claimed.

### DX12 execution-enable experiments and verified Draw undo

The experiment model now has a DX12 session overload with format
`FloraGPA experiment 2`, backend `dx12`, and event domain `execution_instance`.
Operations store execution IDs as decimal strings. Disabled IDs address a single
submission instance, including when a command-list generation is submitted more
than once. Native recording omits only the selected direct Draw/Dispatch call;
state commands, list boundaries, queue dependencies and execution numbering stay
intact. Planned disabled executions and actually suppressed submitted executions
are reported separately. A disabled execution cannot be measured as an ordinary
performance event. Its analysis marker has a separate disabled prefix, which
cannot map an adjacent action to the omitted execution.

Persistence binds the project to `dx12_stream_sha256_v1`: SHA-256 over the named,
length-prefixed metadata, thread and resources files in filename order, including
all resource contents. Paths, preview images and SDK binaries are excluded. Hashing
is requested only for experiment creation/load, reads mapped data in cancellable
16 MiB chunks, and is cached by the read-only session. Ordinary directory opening
does not hash the large resources file. Projects from another capture, floating
point event IDs, unsupported operation kinds and invalid redo entries are rejected
without replacing the active project. Version-1 DX11 project loading is preserved.

The native CLI accepts `replay --experiment <project.json>` and
`--disable <comma-separated execution IDs>`; the latter adds enable-history edits
to a new or loaded project. The resolved project is saved as `experiment.json` in
the output directory. Reports include capture identity, cursor and a SHA-256 of
the complete project document, distinguishing different edits at the same cursor.
For example:

```powershell
FloraGPA.Cli.exe replay "D:\CDXrepo\captures\Talos_2026_10_18__00_55_08.gpa_frame" --event 12675 --id 167081 --subresource 1 --disable 12675 --max-bytes 4294967296 --out talos-disabled
```

`talos-disable-experiment1` validates three serial CLI replays using projects
created, saved and undone by the actual C++ experiment model. The selected Draw
is execution **12675**; texture creation **167081**, object **4980699392**,
subresource **1**, is 2048 x 2048 R16 with 8,388,608 raw bytes.

| Project state | Executed direct Draws | Nonzero raw bytes | Raw SHA-256 |
| --- | --- | --- | --- |
| Baseline | 1 | 792,141 | `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217` |
| Execution 12675 disabled | 0 | 0 | `2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74` |
| Undo, saved and reloaded | 1 | 792,141 | `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217` |

All three run with zero D3D12 validation errors. Undo restores the exact raw bytes,
not merely a matching preview. The capture identity is
`328a72597054f17f08b9026ff3f0fe0409984514c81137640fbf0640aa0a6c72`.
The disabled report contains only `"12675"` in `suppressed_executions`; baseline
and undone reports contain none. Captures remain mapped read-only; edited bytes
and projects are written only to the chosen output directory.

The focused repeated-submission fixture verifies that disabling execution 2 does
not disable execution 1 from the same recording, project round-trip, undo/redo,
redo truncation, rejection of floating IDs/invalid undone operations, content
identity differences when only resource bytes change, and hash cancellation.
The real GPU experiment and this fixture pass (`dx12-experiment1.txt`); legacy
GF2 DX11 project identity checks pass without GPU replay
(`dx12-experiment-legacy2.txt`). Disabled-marker mapping is checked separately.
GUI/CLI/Worker and the focused tests build through `build126.log`.

This established the native/model Draw experiment; the following milestone connects
it to the workspace. Direct Dispatch suppression is implemented but lacks an
independent GPU-write experiment. Indirect/mesh suppression, resource patches,
and full modified history/debug/geometry coverage remain unfinished. The shader
import milestone below provides the first verified replacement path.
No BF1 or Hellblade2 GPU rerun was added for this change.

### DX12 workspace experiments, persistence and result invalidation

The existing Disable/Enable Event, Undo/Redo and Open/Save Experiment actions now
use the DX12 execution-instance model. First edit and project loading prepare the
capture content identity asynchronously, with cancellation and session/revision
checks before installing a candidate. Ordinary capture opening still avoids the
full content hash. Invalid projects leave the current experiment intact.

Saved `ui.dx12` state records the event domain, event, resource creation,
subresource, boundary and allocation budget. Identities use decimal strings;
loading validates the schema, numeric ranges and referenced objects before
restoring the workspace. Navigation restores filtered selections to visibility.
Experiment changes invalidate native output, analysis captures and view request
identities. Analysis at a disabled selected execution is unavailable. Worker
reports must match both the capture identity and complete experiment-document
digest, including changes that retain the same history cursor.

`dx12-workspace-experiment1.txt` records the real main-window test, with two serial
Talos Worker GPU replays at execution **12675**, resource creation **167081**,
subresource **1**, after-event boundary:

- Disable Event changes the exported 8,388,608-byte depth resource to all zeroes,
  SHA-256 `2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74`.
- Undo restores SHA-256
  `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`,
  exactly matching the prior native baseline. Display pixel (1527, 1240) changes
  from (0, 0, 0, 255) to (70, 70, 70, 255).
- Save, redo and reopening in another window restore the execution, resource,
  subresource, boundary and disabled state. This portion uses boundary None and
  performs no additional GPU replay.
- Cancelling preparation preserves the current project. Switching captures
  rejects an outstanding project load. Old debug, mesh, pixel-history and metrics
  request IDs are rejected after the edit. This verifies invalidation, not the
  correctness of analysis results for a modified later execution.

The test passed in 50.763 seconds. Its temporary-capture lifetime initially
caused a cleanup warning; the fixture now outlives the window holding its mapped
files. No GPU rerun was added solely for that cleanup change.

`dx12-workspace-experiment-compat1.txt` passes the report-identity checks against
saved real native reports (including same-cursor changed contents, wrong capture,
missing and unexpected experiments), plus one GF2 DX11 workspace replay. The
2560 x 1440 DX11 RGBA output retains SHA-256
`2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`;
existing project actions retain their expected initial availability. Build131
completes GUI, Worker and the focused test target. This milestone does not resolve
the recorded DX11 baseline faults or the remaining DX12 experiment types.

### Pipeline shader replacement with native PSO validation

The workspace now provides **Edit > Replace DX12 Pipeline Shader…**. Supply the
CPU PSO creation-record ID, choose one of its captured stages and import compiled
DXBC/DXIL bytecode. API record details expose the pipeline stages and original
digests. The replacement applies to every use of that exact pipeline creation,
including repeated command-list submissions. It does not target every pipeline
sharing the same bytecode or a later object lifetime at the same address.

Experiment history persists the `pipeline_shader` operation, decimal pipeline
ID, stage, `pipeline_lifetime` scope, original shader digest and checksummed
bytecode asset. All history entries, including undone edits, are validated.
Same-stage edits supersede earlier edits at the active cursor; undo/redo and
project loading reuse the existing model. Workspace imports are cancellable and
preserve existing experiment history. Original capture files remain read-only.

Container validation checks chunk ranges, executable program length and stage
for DXBC and DXIL. Replay patches the graphics/compute descriptor or the exact
pipeline-stream shader subobject and creates all affected PSOs before GPU
submission, retaining the captured root signature and remaining pipeline state.
This uses the native runtime's
[pipeline compatibility validation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc).
An incompatible replacement fails the request; it never silently falls back to
the original shader. Debug-layer diagnostics are included when available.
Embedded-only root signatures currently fail explicitly because that validation
path is not implemented. Reports distinguish PSO creation from GPU execution and
list the original/replacement digests and scope.

The focused Talos experiment targets pipeline creation **156847**, library name
`B5355971`, VS SHA-256
`81f4477676e4cb210da4408c77314811ab233c99192d3709a222fa8bdc3daec5`.
Execution **12675** uses this pipeline without a PS. The test reconstructs the
captured VS XY/W transform from its saved disassembly and changes NDC depth to
**0.25**, preserving the captured rasterizer state, including slope-scaled depth
bias **-2**. Resource creation **167081**, subresource **1**, supplies the actual
2048 x 2048 D16 GPU readback after the Draw.

`dx12-shader-workspace2.txt` passes in 37.554 seconds using two serial Worker
replays through the main window:

| State | Raw result | SHA-256 |
| --- | --- | --- |
| Imported VS | 396,850 pixels equal 16384; remaining pixels zero | `3f5d384f150e24e3b078e9c2aa316610a47595014f52dee9f77b5fbe2e738bcf` |
| Undo | Exact original 8,388,608-byte depth resource | `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217` |

The written-pixel mask is identical before and after replacement (zero mask
differences). Both project states and raw resources are saved in
`talos-shader-workspace2`; the comparison is `comparison.json`. The initial test
used depth 0.5 and observed uniform D16 value 32767 rather than its assumed
32768. That test failed and was retained as `dx12-shader-workspace1.txt`. The
accepted experiment avoids that half-step quantization boundary; it does not
relax the exact-value check or remove the captured depth-bias state.

Additional checks serve this replacement implementation:

- `dx12-shader-project4.txt`: model save/load, undo/redo, wrong-stage rejection,
  invalid original digest hidden behind undo, actual DXBC/DXIL stage decoding,
  and the existing repeated-submission enable-edit fixture pass.
- `talos-shader-incompatible2.log`: a VS referencing CBV b13 fails before replay
  with the runtime's explicit message that the range is not bound in the original
  root signature. This is an expected rejection, not a successful replay.
- `dx12-shader-pipeline-paths1.txt`: BF1 compute pipeline **4756** and Hellblade2
  mesh stream pipeline **1390** accept byte-preserving CS and MS/PS imports with
  zero validation errors. These create PSOs only; they do not prove changed CS,
  MS or PS execution, nor perform BF1/Hellblade2 frame replay.
- Build140 completes GUI, CLI, Worker and the focused test target. Cancellation
  during PSO preparation retains its cancellation status instead of being
  reported as a shader compatibility failure.

Changed DXIL shader execution, all-stage replacement coverage, source-editor
compilation integration and embedded-only roots remain unfinished. The following
milestone validates analysis of the modified Talos VS. The full portable-package
acceptance is still outstanding.

### Modified-shader debugging, geometry and depth history

The first modified-shader analysis attempt exposed a report-reader regression:
`history-capture` reports contain no output image, so the reader did not retain
their native JSON document. The new DX12 experiment identity check then received
null and rejected an otherwise completed native recapture before launching the
analysis service. This also affected unmodified DX12 recaptures after the identity
check was introduced. It is separate from the recorded DX11 baseline faults.

The reader now preserves native JSON for **every DX12 report**, including
image-free analysis recaptures. The identity/experiment acceptance checks remain
in place. `dx12-analysis-report1.txt` covers an image-free `history-capture` report,
execution ID `9007199254740993` and a native unsigned value of `UINT64_MAX` without
floating-point conversion. The DX11 reading condition is unchanged.

`dx12ModifiedShaderAnalysis` opens the persisted depth-0.25 VS project in the
main window, selects Talos execution **12675** and depth creation **167081**, and
requests VS debugging, post-VS geometry and Pixel History. It then undoes the
shader replacement and repeats these same queries. Each state shares **one exact
native recapture** among all three analyses. The accepted run uses two native
recaptures with serial RenderDoc queries; there is no extra display-output replay.

| Evidence | Modified VS | After undo |
| --- | --- | --- |
| Execution mapping | 12675 -> RenderDoc event 151 | 12675 -> RenderDoc event 151 |
| VS trace | 13 steps; instruction stepping works | Original 20 steps |
| Final Watch `SV_Position.z / SV_Position.w`, vertex invocation 0 | 0.25 | 0.3870071768760681 |
| Post-VS geometry | 8,053 vertices / 42,006 indices; every vertex satisfies `z = 0.25 * w` | CSV and OBJ exactly equal the existing original-shader baseline |
| Pixel (1527, 1240), mip 0 / layer 1 / sample 0 | Draw post-depth 16384 (D16) | Original post-depth 17980 (D16) |

Both D16 values match the independently verified native resource readbacks.
The history row retains source record **185550**, submission **188940**, queue
**175347920**, recording generation **2** and the exact execution identity.
The before-Draw value is zero in both states. No source files are invented for
either stripped shader: the test uses instruction stepping and Watch.

Undo clears the displayed trace and mesh and rejects their old request IDs.
The subsequent analyses use a different capture path and experiment key; each
state's debug, mesh and history results agree on both identities. A debugger
configuration from the modified shader is rejected against the restored shader.
All six analysis results report no loaded GPA modules.

Evidence is retained under `talos-modified-analysis5`, including both `.rdc`
files, debug/history reports, mesh CSV/OBJ exports and `comparison.json`:

- Modified mesh CSV SHA-256:
  `c06660cbe5defdf4cabb1b57416f8fe4e8f475916547ebe77fe7f3170e4f723c`.
- Restored mesh CSV SHA-256:
  `73d2c261ec870613329701d7cf73e567ba71b43d75a606118ab58bef98074755`.
- Restored OBJ SHA-256:
  `5d0ccb743b0ac923827ddc080910fea174baa9583d0a52930fbb51ee8754a639`.

`dx12-modified-analysis5.txt` passes in 50.535 seconds. The earlier attempts
`analysis1/2` stopped at test setup without GPU work; `analysis3/4` reproduce the
report-reader failure after recapture. Build146 includes the GUI fix and Build147
builds the final focused test. Saved traces/captures are retained for diagnosis
without repeating the native prefix.

Pixel History still reports **partial**: these are observed event-boundary depth
values, not complete per-fragment modifications or test-failure reasons. Modified
PS/CS/DXIL debugging, full history coverage, complete resource modifications, other sample
acceptance and portable-package verification remain unfinished.

### Resource data overrides at a submission boundary

The workspace's **Edit > Import DX12 Resource Data…** imports raw bytes into the
selected buffer or texture. The operation names the resource creation and the
selected execution's **submission**, with explicit scope `before_submission`.
It runs once after preceding captured CPU uploads and before every command in
that captured `ExecuteCommandLists` call. Later copies, clears and shader writes
can overwrite it. This is not a before-Draw injection or isolation of one use
within a submission. Repeated submissions retain separate identities.

Buffers accept an offset and byte range. Single-sample textures accept complete,
tightly packed rows for one subresource, including all depth slices. Native
footprints supply upload row padding; the import never interprets a PNG as raw
texture data. Default heaps use a bounded staging allocation and an auxiliary
copy with state restoration and submission decay tracking. Upload buffers have
a mapped write path. Reserved/tiled resources, MSAA imports and other heap types
are explicitly rejected. The mapped path and other texture formats have not yet
received separate GPU validation. Each asset is limited to 256 MiB.

Checksummed project assets preserve undo/redo and capture identity. History order
determines overlapping writes at the same boundary. All entries, including redo
entries, are validated. The original directory remains read-only. The native
report distinguishes requested patches from actually applied patches and records
the applied boundary, method, byte count and digest. Auxiliary copy markers do
not acquire an adjacent original execution identity. Experiment changes use the
existing output/analysis invalidation. A selected buffer no longer schedules an
unsupported automatic texture readback; select the output texture and Replay.

Targeted Talos evidence:

| Check | Observed result |
| --- | --- |
| Constant-buffer target | VS b0 row 2, creation **150566**, object **109459904**, byte offset **32058656**, default heap |
| Application boundary | Submission **188940**, before first Draw execution **12675** |
| Imported bytes | Float4 `(0, 0, 0, 0.25)`, 16 bytes |
| Modified depth creation **167081**, subresource **1** | 396,850 pixels at D16 16384, all other pixels zero; full raw SHA-256 `3f5d384f150e24e3b078e9c2aa316610a47595014f52dee9f77b5fbe2e738bcf` |
| Undo | Full 8 MiB readback returns to original SHA-256 `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217` |
| Texture import before Clear execution **12667** | Entire 8 MiB subresource equals imported D16 16384; SHA-256 `2e962200f1e36f1320e619289160e9013817bb975b8658526d9aabc500e49052` |
| Same texture import after Clear **12667** | Entire subresource is zero; SHA-256 `2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74` |

`dx12-resource-project1.txt` passes project preparation, save/load, undo/redo,
buffer bounds, invalid submission and invalid redo scope checks without GPU
execution. `dx12-resource-workspace2.txt` passes import, output export, project
save and undo in 40.593 seconds using two Worker replays. Its outputs are retained
in `talos-resource-workspace2`. The modified output exactly matches the earlier
depth-0.25 VS replacement output, obtained here without replacing the shader.
The first workspace attempt stopped at an incorrect test invocation before GPU
work; it is retained as `dx12-resource-workspace1.txt`.

`talos-resource-texture-before1` and `talos-resource-texture-after1` contain the
two serial CLI texture boundary checks, each using SDK 616 and the D3D12 debug
layer with **zero validation errors**. Their 773 warnings are the existing
buffer initial-state and ignored sampler comparison-function messages. CPU
comparison evidence is `talos-resource-workspace2/comparison.json`; no extra GPU
replay is used for these comparisons. Build152 builds GUI, CLI, Worker and the
focused test target with final report fields.

`dx12-resource-analysis1.txt` subsequently validates the modified buffer in the
workspace's VS debugger and Resource History using **one** native recapture and
two serial RenderDoc queries (28.947 seconds). The original VS still executes
20 instruction steps, and the final Watch `SV_Position.z / SV_Position.w` is
**0.25**. Its Draw maps to RenderDoc event **164**, not the unmodified capture's
event 151: auxiliary replay commands are allowed to shift native event numbers.
Both analyses share the same capture and experiment identity and report no GPA
modules loaded. The resource history contains one auxiliary CopyDst with
`unmapped_replay_action` and no selectable original execution, while the Draw's
constant binding maps to execution **12675**, source record **185550**. The
captured preceding Copy and barriers retain their own execution identities.
Undo clears both views and rejects their old request IDs. Saved evidence is
`talos-resource-analysis1` (`modified.rdc`, `debug.json`, `history.json` and
`comparison.json`); Build153 adds this focused integration check.

These checks establish default-heap buffer and D16 texture overrides at one
Talos submission and modified-buffer VS debugging. Resource History remains a
partial operation/binding index, with auxiliary copies explicitly excluded from
original execution mapping; it does not measure every content change. General
before-execution injection, reserved/MSAA resources, modified-resource Pixel
History and other shader stages, BF1/Hellblade2 resource experiments and full
experiment acceptance remain unfinished.

### Selected-execution buffer readback

Selecting a DX12 buffer creation now opens the existing **Buffer** tab. Choose
**Before event** or **After event**, enter a byte offset and length, then **Read**.
Hex/ASCII and 32-bit word interpretations use the actual GPU readback bytes;
**Export** writes exactly the displayed range. The default range is the first
256 bytes (or the entire buffer if smaller). Initial-state viewing, reflected
constant fields and DX12 UAV-counter interpretation are not connected here.
The existing texture-output Replay action directs buffer users to this tab.

The existing CLI `buffer` command detects DX12 directories and requires a
submitted execution identity:

```powershell
FloraGPA.Cli.exe buffer "D:\CDXrepo\captures\Talos_2026_10_18__00_55_08.gpa_frame" --event 12675 --before --id 150566 --offset 32058656 --length 16 --max-bytes 4294967296 --out talos-buffer
```

It writes `buffer.bin` and `report.json`, including resource creation/object,
execution, boundary, offset, length, SHA-256 and native replay/experiment evidence.
DX12 offsets and lengths are decimal strings throughout the report; DX11 keeps
its existing numeric range format. Omitting length means the remaining bytes,
and ranges larger than 256 MiB are rejected rather than silently truncated.
Use multiple explicit ranges for a larger buffer. DX12 CSV and initial-state
inspection remain unimplemented; `--no-buffer-csv` is accepted for callers
sharing the existing buffer interface.

The native implementation copies the requested range into a budget-checked
readback allocation, restores state, waits for completion and hashes/publishes
bounded chunks. Readback-heap sources have a direct mapped path. Mapping is
released on failure or cancellation, and a debug-layer readback error prevents
publishing success. The Worker verifies backend, execution, resource, boundary,
range, file length and hash. The existing session/request/experiment guards
remain active. Event, resource, boundary and experiment changes clear displayed
DX12 bytes and reject obsolete results; changing a buffer boundary does not
automatically launch another expensive replay.

Focused evidence:

- `talos-buffer-native1`: debug-layer CLI read before execution **12675**, buffer
  creation **150566**, offset **32058656**, length **16**, with the existing
  constant-buffer experiment. Exact bytes `0000000000000000000000000000803e`,
  SHA-256 `1498d267dc099c4ea99c89d5f647dfdc6063c6ed2403e92011f4145702614427`,
  **zero D3D12 validation errors**.
- `dx12-buffer-workspace1.txt`: two Worker replays in 38.601 seconds. Modified
  before-event bytes display/export exactly; undo followed by after-event
  readback returns `c7e4fc3a8f44d43a7af659baa9b25e3e`. The restored bytes equal
  an independent reconstruction from captured CPU uploads to source buffer
  **109473824** before the captured Copy at record **188930** / submission
  **188936**. Restored SHA-256:
  `af5ca3c3b4f8a2fc5b4db1c24be9c2739574885c15394d9ef6e8fe825c48af80`.
  Exported bytes and comparison are under `talos-buffer-workspace1`.
- `worker-buffer-dx12-1.txt`: 30 checks pass, retaining DX11 buffer artifact
  checks and adding decimal offset **9007199254740993**, overflow, size limit,
  backend/execution mismatch, hash rejection and cancellation. This is CPU
  evidence for wide identity/range transport, not a >4 GB native GPU allocation.
- Build159 builds GUI, CLI, Worker and both focused test targets; Build160 adds
  the final default-range clamp for buffers smaller than 256 bytes.

Actual GPU readback is verified for the selected Talos default-heap range.
Readback-heap sources, large native offsets, other formats/usages and the other
two samples have not received additional buffer-readback acceptance here. This
work does not resolve the remaining full-frame or analysis gaps below.

### Input assembler geometry

For a DX12 Draw, **Replay Mesh → Input attribute** now reads the selected input
layout attribute and draw instance from the isolated RenderDoc replay. The
attribute field is a zero-based layout index; **Details** records every semantic,
format, slot, binding, stride, instance step rate and the draw's index/base-vertex
parameters. The selected semantic is not assumed to be position. The table,
preview, CSV and OBJ use converted float32 components without perspective divide;
32-bit integer conversion can lose precision. Original bytes remain available
through **Raw input vertices (.vbin)** and **Raw input indices (.ibin)** export.

Vertex export contains the selected attribute's referenced byte span, including
stride gaps, starting at `raw_vertex_offset`; it is not necessarily the complete
bound vertex buffer. Index export preserves the original draw range and index
width. Preview indices are separately rebased to the minimum referenced vertex.
Source vertex numbers, byte offsets and sizes use decimal strings in metadata.
Artifact lengths/hashes, selected attribute/instance and existing execution,
session, request and experiment guards are checked before displaying results.

The adapter handles regular 8/16/32-bit scalar components, Float16/Float32,
integer and normalized formats, indexed/nonindexed draws, base vertices and
per-instance attributes. Packed formats, primitive restart and out-of-binding
reads explicitly fail. Input reads are bounded to 256 MiB and four million
references/vertices. Only the selected Talos layout below has actual GPU evidence;
other format and instancing paths are not claimed as sample acceptance.

Focused evidence reuses the existing native Talos RDC (no new frame replay):

- `talos-input-mesh2`: execution **12675**, source record **185550**, instance 0,
  attribute 0 (**TEXCOORD0**, slot 0, float3, stride 12). RenderDoc returns
  **8,053 vertices**, **42,006 uint16 indices** and **14,002 triangle faces**,
  with no loaded GPA modules. All converted float3 values and rebased indices
  match the retained raw bytes. The first vertex
  `(-10.244478225708008, 19.756996154785156, 61.500404357910156, 1)`
  also matches the earlier independent VS Debug input.
- `dx12-input-mesh-artifacts1.txt`: CPU comparison against the original capture
  confirms all **96,636 vertex bytes** equal initialization Copy record **8821**
  for resource creation **4595**, and all **84,012 index bytes** equal Copy record
  **113545** for creation **70438**. The same check loads the workspace table,
  exports original bytes and rejects results after the attribute changes.
- Vertex SHA-256:
  `5c75467d94575dabe780ba1ff12ecd6a194276809312c790c09f52355564bb24`.
  Index SHA-256:
  `268814027b2032c640a2b53679e9e5ee663b649a5043f9e6a81f7c5a86a14c22`.
  Full comparison is in `talos-input-mesh2/comparison.json`.
- Build164 builds GUI, CLI, Worker, RenderDoc service and both focused test
  targets. `dx12-input-mesh-artifacts2.txt` passes all three Qt checks in 147 ms,
  including exact vertex/index export. `FloraReplayMeshTests` also passes,
  covering invalid/overflowing decimal vertex identities and a valid identity
  above 2^53. These final checks are CPU-only.

This adds verified input geometry alongside the existing post-VS and MeshOut
paths. It does not complete all geometry stages, formats or samples.

### Measured replay queue timeline

DX12 **Replay Metrics → Measure** now also collects timestamp brackets around
each submitted command-list instance in the replayed frame prefix. The new
**Replay timeline** tab groups intervals by queue, displays begin time/duration
and proportional time bars, and supports double-click navigation to the first
execution in a nonempty list. Repeated submissions retain separate identities.
Empty lists have real brackets but a null `first_execution`; they must not jump
to an adjacent list. Export retains the native timeline and calibration evidence.
The DX11 metrics interface keeps its existing tabs and behavior.

Timestamp heaps/readback are shared by command-list type, bounded to 65,536 list
instances. Direct/Compute use timestamp heaps; Copy uses its separate heap type
only when `CopyQueueTimestampQueriesSupported` is reported. Unsupported Copy
intervals remain explicitly unmeasured. This follows Microsoft's
[D3D12 timing and calibration contracts](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing).
This does not extend the selected-event pipeline-statistics path to Copy lists.

Each actual frame submission has `GetClockCalibration` pairs before submission
and after its completion fence. GPU/QPC timestamps, frequencies, list identities
and offsets remain decimal strings. Integer ticks are subtracted before floating
point conversion. QPC alignment requires both pairs, matching GPU frequencies,
bracketed timestamps and a calibration drift check. The tolerance is the larger
of 100 microseconds and 0.1% of the calibration interval, plus the two API call
windows. Drift and call windows are reported; this is a consistency check, not a
guaranteed error bound. Failed calibration stays on a separate queue-local scale.
Time bars share a scale only for intervals with accepted QPC calibration.

The view explicitly describes a **serialized replay prefix**. Existing CPU
completion waits change concurrency and the inserted queries affect timing;
these results are not original-application queue overlap or frame performance.
Timestamp brackets describe bottom-of-pipe completion intervals. Initialization
and experiment helper submissions are excluded. Captured Queue Signal/Wait rows
record CPU-side observations after the replay API/completion operation, with
`gpu_wait_duration: null`. Actual GPU stall duration and full synchronization
analysis remain unfinished; no CPU interval is substituted for a GPU wait.

Focused evidence:

- `bf1-native-timeline2.json` / `.txt`: one workspace measurement of BF1 execution
  **21895** reaches **13 lists** in submission **364657**, queue **861708976**.
  All 13 intervals calibrate, including two empty lists with no event link.
  Original submission/list ordinal/generation/queue identities match the capture
  index; the selected list's interval encloses the independently instrumented
  Dispatch timestamps. CS invocations remain **4096**. Selected-event duration
  is **3968 ticks / 1 GHz = 3.968 microseconds** for this single run.
  One captured queue-signal observation is retained. The 13 lists share a single
  queue: this sample is not evidence of cross-queue capture fidelity.
- The first targeted run exposed the empty-list identity issue and failed its
  identity check; the implementation and test now use submission/ordinal for
  empty lists. The second run above is the verification of that concrete fix.
- `native-timeline-queues1.json` / `.txt`: a **298 ms** minimal GPU fixture uses
  three empty command lists on separate Direct, Compute and Copy queues. All
  three acquire timestamps and QPC alignment on the RTX 5090 Laptop GPU, with
  **zero D3D12 validation errors**. This checks query/clock plumbing, not captured
  multi-queue dependencies or workload performance.
- `native-timeline-ui1.txt`: five CPU UI checks pass, including the existing
  DX11 behavior, report identity propagation, queue-local fallback, one-tick
  differences above 2^53, exact event navigation/export and experiment
  invalidation. `native-timeline-ui3` renders the real BF1 report for visual
  inspection; all 13 intervals, two empty rows and the fence observation display.
- Build167 builds GUI, CLI, Worker and focused tests; Build168/169 add the
  minimal queue fixture and CPU rendering check. Build170 adds the visible
  serialized-prefix label; Build171 updates the capability description and
  builds the final GUI/CLI/Worker. No other full sample replay
  was run for this change.

### D16 Pixel History decoder patch

The optional patched RenderDoc 1.45 backend restores D16 values for the verified
Talos depth pixel. Upstream commit is
`2fc0bc04cb95499635f63986a55bc6f67849dd9f`. In
`renderdoc/driver/d3d12/d3d12_pixelhistory.cpp`, generic typeless conversion
selected `R16_FLOAT` for a depth resource; `GetDepthValue` then interpreted D16
storage bits as half floats. The patch preserves `D16_UNORM` for typeless D16
depth resources and explicitly divides their 16-bit storage value by 65535.
It leaves the separate D32 intermediate-fragment path intact. Two preliminary
fixes (format reporting alone, then only terminal decoding) still returned
**6.234375** and failed acceptance. The final fix addresses both conversions.

The isolated worker detects the explicit
`RENDERDOC_GetFloraD16HistoryPatchVersion` export and records
`backend_d16_history_patch: 1` plus `patched_backend_values`. The UI identifies
the patched backend. Stock 1.45 continues withholding unreliable fragment
depths and reporting separate raw event boundaries. A patch marker does not
establish correctness for other formats, MSAA or failure reasons.

Focused GPU evidence (serial, existing native RDC captures):

| Report under `artifacts/dx12-research` | Execution / primitive | Shader depth | Post depth | D16 quantization |
| --- | --- | --- | --- | --- |
| `talos-d16-patched-history3/result.json` | 12675 / 12999 | 0.2743619382381439 | 0.2743571996688843 | 17980 |
| `talos-d16-patched-modified1/result.json` | 12675 / 12999 | 0.25 | 0.2500038146972656 | 16384 |

Both query object **4980699392**, creation **167081**, pixel **(1527,1240)**,
mip **0**, layer **1**, sample **0**. Clear execution **12667** and Draw
execution **12675**, source **185550**, retain their exact mapping. Quantized
post depths match the previously verified native raw readbacks. The modified
capture contains the real constant-buffer override; no new full-frame replay
was needed. These two single-fragment passing draws do **not** validate multiple
fragments, rejected fragments, all depth-test reasons or full history coverage.

Build the backend from a dedicated upstream v1.45 checkout:

```powershell
./tools/build_renderdoc_d16.ps1 -SourceDirectory <dedicated-renderdoc-checkout>
./tools/package.ps1 -PatchedRenderDocRoot <checkout>/x64/Release -OutputDirectory <new-package>
```

The builder verifies the upstream commit and exact tracked patch, uses v143
Release x64, and emits source/patch/DLL provenance. Packaging validates this
manifest, retains the pinned official helper and license notices, includes the
patch, and distinguishes shipped DLL hashes from original archive provenance.
Default packaging remains the pinned official runtime. Tested patch SHA-256:
`c5c15d9b6694a732348952d70c1335777af5e72866bcdfe1aa0fac37efefb117`;
local DLL SHA-256:
`1e0eb5dc09208833988369859416333c304675d07de9b3b849952a4918ce92c1`.
Local rebuilds may differ in binary hash; each build records its actual hash.

`d16-portable1` successfully executes the modified query with PATH restricted
to Windows directories. Its module inventory uses bundled Qt, CRT, compiler
and patched RenderDoc, with no GPA/Python module. This validates this isolated
analysis path only; full portable native replay/analysis acceptance remains
unfinished. Build173 builds GUI/RDC; Build174 builds the focused CPU history
report check. `d16PatchedReports` checks quantization against the native values,
mapping/navigation, exact export, and the stock fallback without GPU execution.
`d16-history-ui1/test.txt` reports **3 passed, 0 failed, 89 ms**, including setup
and cleanup. No all-sample replay or unrelated regression suite was run.

### Two-fragment depth rejection experiment

`dx12DepthHistoryProjects` verifies Talos pipeline **156847** uses D16,
depth writes, `GREATER_EQUAL` and no culling before preparing two reversible
projects. The projects replace the VS with two overlapping full-screen triangles
and overwrite index-buffer creation **70438**, offset **0**, with **84012 bytes**:
indices 0..5 followed by degenerate zero-index triangles. The original indexed
Draw count remains 42006. Resource changes apply before submission **188940**;
the original files remain read-only. The replacement uses `SV_VertexID` and
preserves the captured PSO/root signature. Undoing both operations produces an
empty edit set; the saved restored projects contain the history with cursor 0.

Two serial native prefix replays and their independent Pixel History queries
test **execution 12675**, source **185550**, pixel **(1527,1240)**, mip **0**,
layer **1**, sample **0**:

| Experiment | Primitive 0 depth / rejection | Primitive 1 depth / rejection | Final native D16 |
| --- | --- | --- | --- |
| `talos-depth-rejected1` | 0.75 / false | 0.25 / true | 49151 |
| `talos-depth-passing1` | 0.25 / false | 0.75 / false | 49151 |

Both native runs finish with **zero D3D12 validation errors**. Each history has
one Clear record plus two Draw fragments, with fragment/primitive pairs **0/0**
and **1/1** mapping to the same correct execution instance. Both final history
values are **0.7499961853027344**, quantizing exactly to the native raw value
**49151**. Full native subresource SHA-256 is
`23ff8950288e504926c8de7422bb5e207017a7d6f0dec65a5743b4b1b5c281a9`
for both orderings. The source HLSL, projects, native reports, raw output and
`talos-depth-{rejected,passing}-history2/result.json` remain in ignored artifacts.
The existing RenderDoc patch is unchanged.

This experiment also exposes a remaining precision gap: RenderDoc's
`D3D12PixelHistoryPerFragmentCallback` uses a **D32 scratch DSV** for intermediate
fragment values. For the rejected case it reports intermediate depth **0.75**,
whereas actual D16 storage after that layer is **49151/65535**. The next
fragment's pre-value inherits that reconstructed value. No inferred rounding is
silently substituted. Reports now identify each D16 pre/post value as
`captured_format_event_boundary` or `backend_d32_intermediate`, count the latter,
and mark such histories **partial**. Backend depth-test reasons include
comparisons of reconstructed depths; the two separated-depth cases above
verify this rejection/pass distinction, not near-equality, depth bias or every
test-failure condition. Exact D16 intermediate storage remains unfinished.

The Pixel History table now displays depth (and stencil only when present),
visible fragment/primitive identifiers and explicit D32-intermediate labels.
Previously the table showed color fields for a depth target and hid fragment
identity. DX11 keeps its existing column defaults. The UI exposes the backend
depth-test failure and jumps from either fragment to execution **12675**.

Focused checks: Build175 prepares the experiments; Build177 builds GUI/RDC with
value provenance; Build179 builds the final table and CPU report checks.
`talos-depth-history-projects1.txt` reports **3 passed**, including setup/cleanup.
`d16MultiFragmentReports` compares history to actual native storage, checks both
outcomes and provenance, and checks displayed depths/identifiers and navigation.
The existing `d16PatchedReports` also checks original/modified single-fragment
values, exact export and stock fallback. No full-frame or all-sample regression
was run. The original captured geometry, other depth comparisons and other
rejection types still require separate evidence.
`d16-multi-ui2.txt` records **4 passed, 0 failed, 200 ms**, including setup and
cleanup. Its `rejected.png` was visually checked for visible fragment identities,
depths, rejection label and intermediate-value provenance.

### GPA screenshot alpha and BF1 intermediate comparison audit

The previously unexplained screenshot alpha discrepancy is now traced to the
local GPA **2025.1.3.1269** `Streams/layers/screenshot-x64.dll`, SHA-256
`b5a9414064c18a9450e509d5cccfa9c8b2aca25593526bc08753c682923343f3`.
Ghidra's `FloraDX12` project contains the independently imported module with
preferred image base `0x180000000`. Relevant RVAs:

- `0xc700`: D3D12 screenshot readback/dispatch. With `async=false`, it invokes
  the synchronous completion path at `0xbf20`.
- `0xbf20` → `0x1eb20` → `0x1e490`: the synchronous path passes an explicit true
  opacity flag through mapped readback/image conversion to the writer.
- `0xde30` → `0x1e6b0`: the asynchronous path also passes true.
- `0x1e150`: for formats 28/29 (RGBA8 UNORM/sRGB), walks width × height using
  row pitch and writes **255** to byte `row + 4*x + 3`. RGB and padding are
  untouched. This runs before `0x1e340` selects the writer; PNG uses `0x1ec60`.

A bounded research-only CPU fixture directly invoked the identified function
in the hash-checked DLL with a 3×2 RGBA8 image, 16-byte row pitch, six different
alpha values and nonzero padding. Every alpha became 255; RGB and both padding
regions remained exact (`screenshot-alpha-fixture1.json`). No capture or binary
was modified. This establishes the screenshot transformation, **not** equality
of native and original raw alpha. Flora keeps its raw channels unchanged.
Talos' previously exact screenshot RGB comparison remains valid, while its
screenshot alpha is unsuitable as a raw GPU reference.

BF1 RGB differences were investigated at an actual intermediate boundary.
`dump-resources-layer-x64.dll` SHA-256 is
`853913de932f235341655d7f55f1d38d68e56d60df45402acff87825eaf78b72`.
The first post-layer attempt crashed during keyframe restore (Windows reports
`D3D12Core.dll` 10.0.26100.9278, access violation at RVA `0x23bf54`); no output
from that run was accepted. Moving the dump to `--pre-layer` completed replay.
The changed state-tracker loading order was observed; the precise crash cause
was not established. A frame-1 filter produced no files. Targeted inspection of
the dump predicate at RVA `0xab30` shows it checks the current runtime frame at
**command recording** time. Removing only this frame filter exposed files
labelled **frame 0**, while screenshot numbering begins at 1.

The successful export retained command-list and resource filters:

```text
--offscreen --exit-after-frame 1
--pre-layer dump-resources-layer:path=<new-output>,resource=0x7e5335b0,command-list=0x27b2b8bb0,views=0x8,dimensions=0x8,no-async=true
```

The two exported RTV files are draw ordinals **59/60**, captured command list
**10656385968**, resource **2119382448** (creation **11399**). Walking that
list's recorded Draw/Dispatch/Indirect calls identifies source records
**363202/363213** respectively, including its earlier keyframe recording.
The first maps to Flora execution **37273** and RenderDoc event **47373**.
Both exports in `gpa-bf1-tonemap3` have identical decoded RGBA bytes. Their alpha
is opaque, so only RGB is compared. GPA reports invalid-binding/state warnings;
these two files do not establish completeness of its resource-dump service.

The existing independent native RDC was replayed to execution 37273 and its
texture read back (`bf1-tonemap-native1`). Across **1,937,280** pixels, the
same-boundary comparison has **1,868,269 exact RGB pixels**, mean absolute
channel error **0.0246044282**, maximum **4**. At (1674,485), native RDC returns
RGB **(59,77,76)** and GPA dump returns **(55,75,76)**. This is before subsequent
HUD/output draws, so later drawing alone cannot explain the difference.

There is also variation within each playback path: at that pixel, the earlier
native final output has **(55,75,76)**, whereas the earlier GPA screenshot has
**(59,77,76)**. Of the **68,673** RGB-disagreeing pixels in the original first-frame
comparison, the new GPA intermediate matches the earlier native final at
**68,163**; the remaining **510** lie in (1534,429)..(1623,461), where later
output operations prevent direct intermediate/final equivalence. This is
evidence against a single fixed color-conversion offset, not a fidelity pass.

One combined original run then recorded both the same filtered intermediate and
the final screenshot (`gpa-bf1-tonemap4` / `gpa-bf1-tonemap4-screen`). The two
GPA intermediate runs themselves differ at **87,079** RGB pixels, maximum **9**,
MAE **0.0456858757**. In the combined run, intermediate/final differ at **50,944**
pixels because subsequent operations still run. Native earlier final vs the new
GPA final has **1,850,538 exact RGB pixels**, maximum **9**, MAE **0.0454931657**.
No numeric tolerance was promoted to an acceptance threshold. Original runtime
variation and the unresolved native/original differences remain separate facts;
their cause in upstream inputs, state or synchronization still needs isolation.

Evidence includes `screenshot-{analysis,encoder,conversion,alpha,sync}1.txt`,
`dump-selection1.txt`, `bf1-tonemap-draws1.json`,
`bf1-tonemap-boundary-comparison1.json`, `bf1-tonemap-same-run-comparison1.json`,
and the original logs/images. This turn used four targeted original-player
invocations (one crash, one filtered-empty run, two resource exports), one
independent RDC texture query, and one CPU alpha fixture. No native full sample
suite, capture changes, runtime GPA dependency or unrelated tests were added.

### DX12 pipeline and binding inspection

The existing Pipeline workspace now selects **DX12 Bindings** for directory
captures and restores the existing DX11 tabs on a backend switch. Read uses the
same isolated recapture/analysis route and experiment-keyed cache as history and
shader debugging. A `PipelineInspection` capability requires a supported executed
Draw/Dispatch (including mesh dispatch); a disabled or unsupported execution is
not presented as a completed query. CLI analysis also accepts decimal-string
DX12 selectors without converting them through floating JSON:

```text
FloraGPA.Cli rdc-analyze <capture.rdc> pipeline --execution 37273 --out <new-directory> --renderdoc <renderdoc.dll>
```

The report includes PSO/root-signature identities, active shaders and entry
points, descriptor heaps, root constants/descriptors/table ranges/static
samplers, reflected shader bindings, descriptor view ranges/formats and sampler
parameters, vertex/index buffer bindings, output targets and predication.
Descriptor tables are not expanded blindly: only RenderDoc's reported potential
shader accesses are queried, with a 16,384-entry hard limit that fails explicitly
instead of silently truncating. Unbounded root table declarations remain metadata.
Descriptor byte sizes are RenderDoc's binding ranges (root descriptors can extend
to the end of their buffer), not measured shader read spans. A non-unused binding
does **not** establish that the GPU accessed it.

Resource links use the exact native replay resource marker's object/creation
pair, cross-checked against the open session before the workspace accepts the
result. Unknown PSO/root-signature/shader provenance remains RenderDoc-only;
no neighboring creation record is substituted. Selecting a resource opens its
existing buffer/resource inspection route. Read/cancel, JSON export and stale
request rejection are connected; event, backend, library and experiment changes
invalidate the displayed result. All resource identities and 64-bit offsets/sizes
remain strings. The report is explicitly **partial**: full fixed-function and
stream-output state, unaccessed descriptor entries and original pipeline-object
identity mapping remain gaps.

Targeted evidence under ignored `artifacts/dx12-research`:

- `bf1-pipeline1`: one query of the existing independent BF1 recapture, execution
  **37273**, RenderDoc event **47373**, **27** rows and **10** potential descriptor
  accesses. The tone-map PS references `mainTexture` creation **324859** (object
  **10738460352**), `colorGradingTexture` **299996** (**8092623872**),
  `distortionTexture` **325118** (**10737193296**) and `tonemapBloomTexture`
  **211712** (**870027664**). VS/PS constants use creation **156161**; RTV 0 is
  creation **11399**, object **2119382448**. These identify the upstream inputs for
  the unresolved RGB comparison; binding inspection alone is not a fidelity pass.
- `talos-pipeline-workspace1`: a real workspace recapture/read of execution
  **12675**, source record **185550**, RenderDoc event **151**, followed by resource
  navigation. VS `cbuffer0` links to creation **150566**, byte offset **32058624**;
  its reported root descriptor range is **1495808** bytes. DSV creation **167081**
  is preserved. The native report completed using SDK **616**; the debug layer
  was not enabled in this workspace run. The isolated analyzer loaded no GPA
  modules. Changing execution rejected the old pipeline request. Switching to
  DX11 hid the DX12 bindings tab, replayed GF2 with the existing exact RGBA hash
  `2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`,
  and retained the selected-draw geometry analysis entry. This focused workspace
  test passed in **10.035 s**.
- `pipeline-ui2.txt` / `pipeline-ui2/pipeline.png`: the real BF1 report was checked
  for populated shaders/RTV mapping, resource navigation, exact export, stale
  experiment/session rejection, and integer-string identity **9007199254740993**.
  A numeric JSON identity and a mismatched execution were rejected. The focused
  CPU UI test passed in **184 ms**, and the rendered view was inspected.

Only the BF1 analysis query, one Talos native recapture/analysis and the DX11
compatibility replay used the GPU. Hellblade2 was not replayed for this UI/API
increment. Neither complete pipeline coverage nor overall milestone acceptance
is claimed by these selected-boundary checks.

### BF1 upstream raw-input comparison and texture readback metadata

The tone-map input query now has a concrete producer boundary. Resource history
for creation **324859** through execution **37273** reports a color-target use at
execution **36758**, original record **213193**, RenderDoc event **45824**. The
captured command list is **0x2f8005630** and its Draw/Dispatch ordinal is **14**.
Later usage entries include mip-generation UAVs and a copy; resource-level usage
alone does not identify the modified mip. A targeted pipeline query at execution
**36956** confirms its `g_output` UAV is creation **324859**, **mip 1**, while
`g_input` is creation **324867**. It must not be called a mip-0 write.

The original dump layer at tone-map Draw 59 exported only the RTV and reported
invalid input bindings (`gpa-bf1-inputs1`). Selecting the earlier producer's RTV
instead succeeded: pre-layer `dump-resources-layer`, `command-list=0x2f8005630`,
`draws=14`, `resource=0x2800fe6c0`, `views=0x8`, `dimensions=0x8`, `no-async=true`,
offscreen playback through frame 1. It exported 11 mip PNGs in
`gpa-bf1-main-input1`; only mip 0 was compared. Ghidra identified the dump
layer's image-conversion wrapper at RVA **0x15ad0** and its DirectX::Convert call
at **0x173d0**. The installed 2025 R1 dump binary still hashes to
`853913de932f235341655d7f55f1d38d68e56d60df45402acff87825eaf78b72`.

A bounded local debugger probe then launched its own GPA child with that exact
filter, stopped once at the wrapper before display conversion, copied its
`DirectX::Image` source bytes and restored the original instruction before
continuing. The child exited 0; no installed binary or capture was edited.
The first probe stopped at layer discovery because GPA loads/unloads the DLL
before using it; handling its unload/reload allowed the second probe to complete.
`gpa-bf1-main-raw2` records **1920 × 1009**, DXGI format **26**
(R11G11B10_FLOAT), row pitch **7680**, and **7,749,120** raw bytes, SHA-256
`93d9816bdeca2d83bb68fdd00f32373d0540468a019e18467d8788f619859d58`.
CPU unsigned-float decoding and nearest UNORM quantization exactly reproduce
the **same-run** exported RGB PNG. A separate CPU call into the hash-checked
screenshot DLL's equivalent DirectX::Convert routine, RVA **0x20340**, also
matches this decoder on the native R11G11B10 input (flags 0, destination RGBA8).

The independently recaptured producer boundary (`bf1-main-input-native1`) has
raw SHA-256 `7449f5d3b9a96865a1080e43fc8c43493bb0885890ca67a8f1b5385be5f60cea`.
Compared with the raw original, **1,806,653** packed pixels match and **130,627**
differ, bounding box **(0,394)..(1919,865)**. Maximum absolute float differences
are **0.125 / 0.01953125 / 0.0234375** for RGB. At **(1674,485)**, original is
`(0.1044921875, 0.0908203125, 0.07421875)` and native is
`(0.1005859375, 0.087890625, 0.07421875)`. Thus differences already exist before
tone mapping, without relying on PNG quantization or rewritten alpha.

Variation remains unresolved: the two original producer PNG runs differ at
**56,254** RGB pixels. A separate independent query at the consumer event
(`bf1-main-input-native2`) differs from the producer query at **48,735** packed
pixels. These are separate replay queries, **not** a same-run before/after
measurement; they do not prove which intervening command changed mip 0.
The precise cause in upstream state, aliasing, synchronization or shader behavior
is still open. No tolerance or overall fidelity pass was inferred.

This comparison exposed a readback reporting defect: native RDC `texture` exports
previously supplied bytes without their texture description and did not validate
the subresource. They now include dimensions, selected mip extent, format with
BGRA/sRGB interpretation, resource object/creation identities, 64-bit texture byte
size as a string, and the raw file's SHA-256. Invalid mip/layer/sample selectors,
empty readbacks, and nonzero depth slices for a 3D texture are rejected (RenderDoc
returns a whole 3D mip and ignores that slice selector). Existing DX11 metadata
adapters retain their schema. A focused CPU test covers volume/array selection,
invalid boundaries and a byte size above 2^53; it passed. The real consumer query
verified the new metadata and file hash:
`821f6dd581a275fa3d882538df42e34dfd577c4630d155afcc71541aa18ab948`.

Evidence: `bf1-tonemap-input-history1`, `bf1-main-producer1.json`,
`bf1-main-mip-writer1`, `bf1-main-input-{comparison1,raw-comparison1}.json`,
`dump-{inputs1,convert1}.txt`, `screenshot-convert-direct1.txt`, raw probe metadata,
the original/native artifacts and `texture-readback-selection1.txt`. Research
helpers and binaries remain outside Git. This increment used four targeted RDC
queries, three completed original-player exports and one discovery-aborted probe;
no full sample suite or unrelated GPU tests ran.

### Pixel shader debugging with a pre-debug GPU reference

DX12 PS debug reports now read color target 0 at the selected invocation's
coordinates **before** calling RenderDoc DebugPixel. The report records the view's
mip/layer/sample, format, integer/float interpretation and returned pixel values;
the debugger panel displays the reference and preserves it in JSON exports.
This is post-event target data, including storage conversion, blending and other
fragments, not the selected invocation's shader return. DebugPixel may perform
additional replays. Multilayer or 3D output views are explicitly ambiguous;
missing targets/metadata, typeless interpretations and out-of-range coordinates
are reported unavailable. Older traces without this optional field still load.
Context changes invalidate the displayed reference with the rest of the trace.

The unresolved BF1 difference was followed to execution **36758**, original record
**213193**, RenderDoc event **45824**. Its DXBC PS hash is
`63c05020-6653250b-702de4a0-42d351aa`. A pipeline query identifies `mainTexture`
as creation **299115**, object **10432933744**; other bindings include depth
**325004**, velocity **325064**, tile velocity **211104**, and velocity-magnitude/
depth **165701**. At pixel **(1674,485)** the tile-velocity magnitude is
**0.0003445148468017578**, so the shader takes its direct-copy branch rather than
the motion-blur sampling loop. Instruction 40 loads `mainTexture`; instructions
45–47 return it with alpha 1. The trace executes **36** states out of the shader's
220 instructions. The earlier trace (`bf1-motion-ps1`) returned
`(0.1044921875, 0.0908203125, 0.07421875, 1)`; this was a separate replay.

The new single analysis request (`bf1-motion-ps-reference1`) reads GPU RT0 as
`(0.1005859375, 0.087890625, 0.07421875, 1)` and its subsequent debug trace returns
that exact tuple. It loaded no GPA modules. Agreement at this invocation rules
out a debugger/output mismatch **for this request**, not replay variation or
overall fidelity; the upstream creation **299115** remains to be investigated.
The focused UI check used this real trace, stepped to its return, evaluated
`o0.x`, checked export, unavailable-reference handling, legacy trace loading and
stale experiment rejection. `ps-gpu-reference-ui1.txt` passed in **592 ms** and
`ps-gpu-reference-ui1/debug-gpu-reference.png` was visually inspected. Evidence
also includes `bf1-main-producer-pipeline1` and `bf1-motion-ps-summary1.json`.
A final CPU check asserting the selected copy branch's `o0.x` equals its GPU
reference passed in **164 ms** (`ps-gpu-reference-ui2.txt`).
Only these three targeted GPU analysis queries ran for this increment; no
Hellblade2 replay or full regression suite was needed.

### Nonzero compute workgroups and explicit GPU texel references

The BF1 upstream investigation reaches Dispatch execution **36666**, source record
**213101**, RenderDoc event **45548**, command list **0x2f8005630**, Draw/Dispatch
ordinal **4**. Its temporal CS `a6745e61-c0a63ae2-5e15ec9e-f4b15799` reads current
color creation **324851**, history color **16551**, depth **16021**, velocity
**325064** and history velocity magnitude **325479**. It writes the motion-blur
input **299115**, history color **286299**, and velocity magnitude **298958**.
These are distinct committed resource creations. The selected group **(209,60,0)**,
local thread **(2,5,0)**, is global pixel **(1674,485,0)** in an **8×8×1** group.
The original CS trace has **432** states and uses shared memory and history sampling.

DX12 CS debug now optionally accepts an explicit original resource object/creation
and texel selector, reading it after the dispatch **before** DebugThread. It
reuses strict resource lifetime matching and texture subresource validation.
CLI selectors are `--resource`, `--resource-creation`, `--x`, `--y`, `--mip`,
`--layer`, and `--sample`. In the workspace, picking a supported output preview
sets this independent reference in the CS panel; **Clear reference** removes it.
Invocation coordinates are unchanged. Context/experiment changes clear it and
reject old results, including exact identities above 2^53. A selected texel is
not proof of a write by the selected invocation. R11G11B10 native preview was
unavailable in this increment, so this BF1 resource was selected explicitly
through the CLI. The following preview increment closes that navigation gap.

The reference exposed an actual RenderDoc 1.45 defect. In the pinned upstream
`renderdoc/driver/d3d12/d3d12_shaderdebug.cpp`, DebugThread initializes GroupID only
on `activeLane()`. `DXBCDebug::InterpretDebugger::BeginDebug` creates the other
workgroup lanes with their correct local ThreadIDs but zero GroupIDs. Those lanes
therefore load incorrect coordinates when contributing shared memory in a nonzero
group. `dx12-dxbc-workgroup.patch` initializes the selected GroupID on every lane
and exports `RENDERDOC_GetFloraDxbcWorkgroupPatchVersion() == 1`. The adapter records
this version and displays a concrete warning for affected stock DXBC shared-memory
traces; a nonempty trace alone is not accepted as reliable debugging evidence.

A minimal reproduction uses the existing CS experiment mechanism to replace BF1
PSO creation **179440**, preserving its `u3` output binding and **8×8×1** group.
Each lane writes `group.x + 256*group.y + lane` to shared memory, synchronizes,
and checks the next lane's value. All **1,937,280** valid GPU output pixels are
exactly **(1,0.5,0.25)** in R11G11B10 storage. Raw SHA-256:
`97bc49991a49c2a76383d607da01363aeb595af412fc3fa8b6f9e6466de42b1f`.
At the selected invocation, stock simulation returns **(0,0.5,0.25,1)** while
the patched simulation and both pre-debug GPU references return
**(1,0.5,0.25,1)**. The original capture is unchanged; project save/reload and
undo remove the replacement. This verifies an actual changed-CS execution,
not merely PSO creation. GPU undo restoration was not repeated here.

At this increment the original BF1 CS was **not yet validated**: the stock simulated store was
approximately **(0.155922,0.169648,0.175920)**; after fixing group IDs it is
**(0.101484,0.088130,0.075260)**, while that request's pre-debug GPU texel is
**(0.1044921875,0.0908203125,0.07421875)**. Further debugger/replay differences
remain. This defect and its minimal reproduction do not explain the earlier
original-GPA/native frame variation. The later stability investigation below
matches the selected stores after accounting for replay phase and float-format
truncation; it still does not establish stable full-frame replay.

Evidence under ignored `artifacts/dx12-research`: `bf1-premotion-{history1,pipeline1}`,
`bf1-temporal-{resources1.json,cs1,cs-reference1,cs-fixed1}`, `bf1-workgroup-project1`,
`bf1-workgroup-native2`, `bf1-workgroup-{stock1,fixed1}` and
`workgroup-regression1.txt`. The focused CPU checks verify all raw pixels, stored
shader values, backend warnings, reference exports, request invalidation and
legacy PS traces (**5 passed**, including setup/cleanup, **941 ms**). The rendered
CS view was inspected. One earlier native attempt stopped at the default 256 MiB
budget; the successful retry used an 8 GiB limit, peaked at **3,370,385,408** bytes,
and used SDK **616**. Seven targeted analysis queries and one successful modified
native replay ran; no whole-sample regression suite or GPA playback was required.
After widening the register value column, the same CPU checks passed in **917 ms**
(`workgroup-regression2.txt`); `cs-gpu-reference-ui4/compute-gpu-reference.png`
shows the full store vector alongside the GPU reference and was inspected.

The existing `tools/build_renderdoc_d16.ps1` now builds both fixes, verifies the
exact combined source tree via a temporary Git index, and records both patch
hashes. Optional patched packaging verifies/copies both. New workgroup patch SHA:
`00e9e95c158df11c6f806a37587705ec6a92e713b269dabee3c546bd29e45672`;
local combined DLL SHA:
`1dee5c374c07cc6046eb8c61558251a4b0a0231e4c9c225dc433ab484af33540`.
The analyzer loaded no GPA modules in the compared traces. Full portable package
acceptance remains unfinished.

### R11G11B10 preview and workspace compute-reference selection

Native texture readback now renders **R11G11B10_FLOAT** alongside its unchanged
raw storage export. Each unsigned floating component is decoded with its own
mantissa width (6/6/5 bits), including subnormals and nonfinite values. The preview
uses linear RGB clamped to **[0,1]**, rounds to RGBA8, and supplies alpha 1; NaN
displays as zero and positive infinity as one. No exposure or color-space
conversion is implied. The output status/tooltip and exported metadata state
this interpretation. Raw HDR bytes and their hash remain independent of the
display image. Other unsupported formats and volume previews are unchanged.

The focused CPU decoder check (`packed-float-preview1.txt`) covers known finite
values, minimum subnormals, the normal boundary, maximum finite values, infinity,
NaN and the earlier BF1 packed word **0x5996d2e7**, which decodes exactly to
**(0.1005859375,0.087890625,0.07421875)** and displays as **(26,22,19,255)**.
It passed in **1 ms**.

`packed-float-workspace1.txt` then opened the real BF1 directory, loaded the prior
workgroup-reproduction CS experiment, selected execution **36666** and resource
creation **299115**, and replayed once through the normal workspace worker.
All **1,937,280** preview pixels equal **(255,128,64,255)**. Exporting raw storage
from the workspace retains the previously verified SHA-256
`97bc49991a49c2a76383d607da01363aeb595af412fc3fa8b6f9e6466de42b1f`.
The exported metadata still reports DXGI **26** and explicitly identifies the
linear-clamp preview.

Selecting preview pixel **(1674,485)** populates the CS request with resource
object **10432933744**, creation **299115**, mip/layer/sample **0**, while preserving
group **(209,60,0)** and thread **(2,5,0)**. Switching to another dispatch clears
the reference and rejects an old result. The workspace check passed in
**45.922 s**. Its preview, raw bytes, metadata and exact debug request are saved
under ignored `bf1-packed-float-workspace1`. It verifies resource display and
selection; it does not rerun Shader Debug or resolve the original BF1 temporal
shader discrepancy. Only this one GPU replay ran for the preview change.

### BF1 debug replay stability and temporal-input investigation

DX12 PS/CS debug references now retain three observations of the same selected
texel: before debugging, immediately after trace creation, and after stepping.
The comparison uses the returned component bits, including integer values and
nonfinite floating values. Any difference marks the report partial and adds a
visible warning; exports preserve all three values. An unchanged selected texel
does **not** validate shader inputs, other pixels, or the simulated invocation.
Legacy reports still display without inventing missing observations. This adds
two selected-texel reads, without requesting another frame replay.

Pinned RenderDoc 1.45 source explains why all three observations matter:
`ReplayController::DebugThread` (`renderdoc/replay/replay_controller.cpp:1667`)
calls `SetFrameEvent(..., true)` after constructing the debugger. In the DX12
DXBC wrapper, `FetchUAV` (`d3d12_shaderdebug.cpp:1257` in the patched research
checkout) may subsequently replay without the selected action; its destructor
reexecutes the action. Thus constant-buffer capture, texture sampling and lazy
UAV fetching can occur across different replays. No new RenderDoc patch is
claimed here.

`bf1-temporal-cs-stability2` demonstrates a real transient change at execution
**36666**, group **(209,60,0)**, thread **(2,5,0)**, output creation **299115**,
pixel **(1674,485)**:

| Observation | GPU RGB |
| --- | --- |
| Before debugging | (0.1044921875, 0.0908203125, 0.07421875) |
| After trace creation | (0.1005859375, 0.087890625, 0.07421875) |
| After stepping | (0.1044921875, 0.0908203125, 0.07421875) |

The report correctly emits `selected_texel_changed`, `partial: true`, and a
warning that the earlier reference cannot validate the trace. Comparing only
the first and last values would miss this failure. In this same request, the
simulated instruction **373** stores approximately
**(0.1014838591,0.0881296322,0.0752600357)**. Applying R11G11B10's high-to-low
floating conversion with **round toward zero** gives exactly the RGB observed
after trace creation. This is a verified selected-invocation storage match,
not a stable-frame or complete temporal-shader validation. Conversion follows
[Microsoft's floating-point conversion rules, section 3.2.2](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
It must not be confused with the preview's nearest-integer RGBA8 display conversion.

Additional targeted evidence narrows, but does not resolve, the discrepancy:

- `bf1-temporal-history-store1`: history-output creation **286299** stores
  **(130,122,112)** at the selected location. The same request's simulated value
  before instruction **295** is approximately **(130.160110,122.438538,113.803062)**,
  which converts exactly to the GPU RGB using round toward zero. Values from
  another request must not be substituted for this trace's registers.
- `bf1-temporal-input-load1`: current-input creation **324851**, pixel
  **(1673,483)**, reads **(0.056640625,0.05078125,0.044921875)**, matching the
  active lane's instruction **66** texture load exactly.
- A separately replayed input export (`bf1-temporal-input-tile1`) differs from
  the debug input, so it is not accepted as a same-run shared-memory oracle.
- One research-only instrumented query (`bf1-temporal-input-snapshot1`) saves
  that input subresource before and after debugging. The first barrier's
  **100** shared-memory elements agree with the same-request pre-debug input
  and the disassembled color transform to within **1.45e-6**, using constants
  rounded to six decimal places. The selected **10x10** input region is unchanged
  between the snapshots, while **9,064** other pixels change (bounding box
  **1466,458–1919,743**). This excludes neither transient input changes nor
  other inputs. Snapshot SHA-256 values are
  `445e02bbe99d40fc7e89db906835a6ceed97f2902c7b7d7d6cf264e623ec6ce5`
  and `7e5c46eeebae0d97825b41525ace46dc5c3f1e5ae20c8d599758e03deafa2fbf`.

The bounded snapshot instrumentation and numerical comparison scripts remain
under ignored `artifacts/dx12-research`; the diagnostic instrumentation was
removed and the production analysis worker rebuilt. Six serial GPU queries
served these specific checks, with no GPA modules loaded. The focused
`dx12DebugReferenceStability` UI check uses the real transient report and tests
changed/unchanged references, intermediate-only changes, exports, legacy reports
and context invalidation. `dx12TemporalStoreReference` separately reconstructs
the registers before both real UAV stores and checks their truncated 6/6/5-bit
mantissas against the correct GPU reference phase, ignoring the unstored alpha.
Both focused checks passed (`temporal-storage-stability1.txt`: **4 passed**,
including setup/cleanup, **982 ms**). No full-sample or DX11 regression run was added.

### Stage-correct DX12 Resource History bindings

Following the unstable temporal input upstream identified execution **36659**,
source record **213094**, RDC event **45527**. Its PS
`d083aa7d-5718f32b-9adda33d-f550cc62` samples `mainTexture` at **t1** and clamps
the color. Pipeline inspection maps this SRV to object **10432919056**, creation
**324843**, and RTV0 to creation **324851**. At **(1673,483)**, the six-state PS
trace returns **(0.056640625,0.05078125,0.044921875,1)**, matching the GPU value
after trace creation and stepping. The pre-debug target differs. This moves the
variation upstream of the temporal compute shader; it does not resolve its cause.

The old Resource History omitted this known PS input. In pinned RenderDoc 1.45,
`D3D12CommandData::AddUsageForBindInRootSig` searched root parameters without
knowing the reflected shader stage. A matching VS-only table could consume the
PS register lookup and return before visiting the PS-only table. It also derived
the reported stage from root visibility, instead of the actual shader. This is
independent of the execution-marker mapping.

`third_party/renderdoc/patches/dx12-usage-visibility.patch` passes each reflection's
stage into that lookup, rejects incompatible root visibility, and labels CBV,
SRV and UAV usage with the actual shader stage. `ALL` visibility remains eligible
for each relevant stage. It exports
`RENDERDOC_GetFloraDx12UsageVisibilityPatchVersion() == 1`; reports record
`backend_dx12_usage_visibility_patch`. Resource History from an unpatched library
now displays the concrete visibility limitation, rather than silently implying
that its shader-binding index is complete. All usage rows remain **bound uses**,
not proof of a shader read or an observed content modification.

Targeted results from the same existing BF1 RDC:

| Resource / execution | Prior backend | Patched backend |
| --- | --- | --- |
| Creation 324843 / 36659 | PS input missing | One `PS_Resource`, mapped to source 213094 |
| Creation 299115 / 36758 | Misattributed `VS_Resource` | One `PS_Resource`, no spurious VS use |
| Creation 299115 / 36666 | `CS_RWResource` | Same compute write binding retained |

Evidence is under ignored `bf1-color-input-history{1,-stock1,-fixed1}`,
`bf1-premotion-history{1,-fixed1}`, `bf1-temporal-input-history1` and
`bf1-temporal-producer-{pipeline1,ps1}`. The stock-library query confirms marker
**0** and `known_stage_visibility_gap`; the rebuilt library confirms marker
**1** and `stage_visibility_matched`. Both retain `partial: true` for the other
history limitations. No GPA modules were loaded in these queries.

The focused `resourceHistoryShaderVisibility` check verifies the missing and
misattributed real rows, retained CS usage, exact event navigation, JSON export,
the actual stock warning, warning removal and rejection after a context change.
It passed (**3 passed** including setup/cleanup, **179 ms**,
`usage-visibility-ui1.txt`). Seven GPU queries served the producer investigation
and old/new history checks; there was no new native recapture or broad regression.

The build and optional patched-runtime packaging scripts now require all three
patches. The usage patch SHA-256 is
`d4ab0794dd4333cd8e09b0c7fbe0c9ad0c16732684903dd88e4aaeaa98c1b134`;
the combined Release DLL SHA-256 is
`02c8df93c5a5fb964648e1307db903bf60092a0afd8dcec08ecf80cab7493b4e`.
`rdc-usage-visibility-build1.log` records the successful pinned build. The package
script passed syntax inspection; a full portable package was not rebuilt or
accepted here. Full resource histories and the underlying replay variation
remain unfinished.

### Fixed-function pipeline inspection and BF1 composition

The existing DX12 pipeline panel now includes input-layout elements, color/alpha
blend equations and factors, write masks, logic-op settings, depth/stencil tests
and front/back operations, rasterizer state, sample mask, viewports/scissors,
shading-rate settings/image, and stream-output bindings/count-buffer references.
Blend factors, operations, comparisons and stencil operations retain both readable
names and backend enum values. Nonfinite float values are explicit strings rather
than silently becoming null. Buffer offsets/sizes and resource identities retain
their lossless representation. Graphics-only rows are omitted for a compute PSO;
the scope reports `graphics_state_applicable`.

Input layout and shading-rate image rows use the same captured-resource mapping
as existing bindings. Fixed-function rows without a resource cannot trigger a
resource jump. JSON export includes the new state. Remaining gaps still include
stream-output declarations, the PSO sample description, unaccessed descriptor
entries and original PSO/root-signature/shader creation provenance. Exposed
stream-output state does not constitute a verified stream-output replay.

The BF1 input pixel investigation now reaches execution **36640**, source record
**213075**, which writes creation **324843** before the simple copy at **36659**.
Its PS `7d991753-540003a6-9bd94a68-8b7fea61` reads color creation **325012** and
depth creations **325004 / 325048**. It computes a depth-aware color contribution
with alpha, so comparing its RGB directly with the target would omit blending.
At this draw the observed equation is:

`target.rgb = source.rgb + oldTarget.rgb * (1 - source.a)`

Depth testing and writes are disabled, independent blending is enabled, the
RT0 write mask is **15**, and viewport/scissor cover **1920x1009**. Input slot 0
has `TEXCOORD0` float2 at byte **0** and `TEXCOORD1` float4 at byte **8**. The
focused test independently decodes original PSO creation **179424**, object
**0x7eb2e6c0**, and verifies its use by `SetPipelineState` record **213069**.
The checked blend equation/write mask, depth/stencil enables, rasterizer
orientation/depth-clip flag, sample mask and layout semantic/slot/offset fields
match those captured settings.

For pixel **(1673,483)**, one Pixel History record for execution **36640** contains:

| Value | RGB / alpha |
| --- | --- |
| Pre-modification | (0.05712890625, 0.05224609375, 0.052734375) |
| Reconstructed shader output | (0.03888561949, 0.03455479816, 0.02888564393), alpha 0.6815996170 |
| Post-modification | (0.056640625, 0.05078125, 0.044921875) |

Applying the captured blend and R11G11B10 truncation exactly reproduces the
post-modification RGB. This verifies interpretation of this recorded composition;
Pixel History's own reconstruction limitations remain. A separately replayed PS
debug trace yields different source colors and is not substituted into this
record. The history also contains four indirect dispatch boundaries with unchanged
selected-pixel values; this does not imply those dispatches performed no work.

Evidence under ignored `artifacts/dx12-research`:
`bf1-color-input-pixel-history1`, `bf1-color-final-ps1`,
`bf1-color-final-pipeline{1,2,3}` and `bf1-compute-fixed-state1`.
Six targeted GPU queries served the investigation and adapter checks, without
another native recapture. `fixedPipelineComposite` reads the original directory
and these reports without GPU work, validates the captured PSO, composition,
compute-state exclusion, panel details, export and stale-result rejection.
It passed (**3 passed** including setup/cleanup, **162 ms**,
`fixed-pipeline-composite1.txt`). Full frame consistency and the upstream source
of the varying color contribution remain unresolved.

### BF1 particle boundary discrepancy and replay API diagnostics

The composition input investigation now reaches resource creation **325012**
(object **10432990048**, 960x504 RGBA16 typeless storage). At pixel **(836,241)**,
Pixel History reports a clear at execution **36391** and 22 candidate fragments
at execution **36560**, source **255931**, RenderDoc event **45218**. This is a
4974-index, one-instance particle draw. Its PS is
`12a8314a-03003d5d-ab7f51bd-f9527a6b`; input buffers are creations **300390**,
**163437**, and index buffer **300406**. The PS samples depth **325048** and
particle texture **222289**. Fragment reconstruction and shader debugging remain
subject to their documented backend limitations.

A new, distinct analysis-replay discrepancy was reproduced:

| Stop/readback path | Selected RGBA |
| --- | --- |
| Native replay after execution 36560 | (0.035003662109375, 0.031463623046875, 0.02667236328125, 0.65478515625) |
| RenderDoc texture readback at following barrier execution 36561 | Exactly the same |
| RenderDoc texture readback directly at draw execution 36560 | (0, 0, 0, 0) |
| PS debug 36560, primitive 1322, all three GPU-reference phases | (0, 0, 0, 0) |

The native and barrier readbacks match for **all 3,870,720 bytes**, SHA-256
**a1cec82b979735c74db4dd23e7b41bcd189128ae376bbcd6fae0abfc029b7138**.
They are separate executions; this equality does not establish general
determinism. The selected PS invocation returns nonzero color and alpha
**0.4272592366**, but that output alone is not a final-target oracle.

Temporary, subsequently removed research instrumentation also compared stop
points within a single controller. Re-selecting the Draw continues to produce
zero at this pixel. Selecting its immediate **EndEvent, 45219**, already produces
nonzero color, without executing the following barrier. All **64 exposed pipeline
rows** are identical between these two selections. DSV creation **325024** reads
**0.0011842424282804132** at the selected pixel in both cases. Other repeated
prefixes still vary in color, so this does not resolve the earlier whole-frame
variation. Temporarily disabling `D3D12_BindlessFeedback` does not eliminate the
zero/nonzero discrepancy; the setting was changed only in that worker's memory.

Source instrumentation of the pinned RenderDoc 1.45 build confirmed that the
single-event replay requests and resolves event **45218**, structured chunk
**44633**, file offset **2870016**, and dispatches
`ID3D12GraphicsCommandList::DrawIndexedInstanced`. Thus this probe did not reveal
an event-map or chunk-selection error. `ReplayController::SetFrameEvent` uses
`WithoutDraw` followed by `OnlyDraw`; moving to the end marker instead places
the Draw in the prefix. **The cause of their different outputs remains unresolved.**
An unchanged three-phase debug reference is insufficient evidence that this
event boundary is correct. Do not classify this case as a validated native
replay failure, a hardware limitation, or a completed Shader Debug comparison.

`rdc-analyze --api-validation` now enables the existing worker replay-validation
option and exports available backend diagnostics in `api_validation`: native
event, category/severity/source enums, message ID, and description. The default
remains disabled. Reports preserve a decimal total count and explicit truncation
after 4096 messages; absence of messages is not a validation pass. The D3D12
report now explicitly identifies these as **loading-stage** messages:
`WrappedID3D12Device::GetDebugMessages` returns the saved loading messages during
active replay without querying the live API message queue. They do not cover
subsequent single-event/debug replays. The BF1
reproduction collected **726 medium-severity messages**: 723 sampler comparison
warnings (1361), two buffer initial-state warnings (1328), and one duplicate
resource-barrier warning (1008). These messages did not identify the cause.

Evidence under ignored `artifacts/dx12-research`:
`bf1-half-color-history1`, `bf1-half-color-pixel-history1`,
`bf1-particle-pipeline1`, `bf1-particle-ps1322-1`,
`bf1-particle-{native-boundary1,after-barrier1,at-draw1,validation1}`,
`bf1-particle-{boundary-probe1,no-feedback1,boundary-depth1,event-offset1,boundary-state1}`.
These are targeted investigations of this draw, not full-sample acceptance runs.
The diagnostic backend/source hooks were removed, the production worker rebuilt,
and the prior three-patch RenderDoc DLL restored (SHA-256
**02c8df93c5a5fb964648e1307db903bf60092a0afd8dcec08ecf80cab7493b4e**).
The CLI/worker build passed. Focused job preparation checks cover default-off,
explicit-on with lossless DX12 identities, and rejection of a nonboolean option:
**5 passed**, including setup/cleanup, **12 ms**, `rdc-api-validation-options1.txt`.

### Particle depth isolation: unresolved original-attachment behavior

Further targeted probes of execution **36560** confirmed that the single-event
path invokes `DrawIndexedInstanced(4974, 1, 0, 0, 0)` on its outside command list.
The original PSO enables depth testing with **GreaterEqual (7)**, disables depth
writes, stencil and depth bounds. The DSV is **D32_FLOAT (40)**, Texture2D,
Mip **0**, flags **0**, resource **3115**, creation **325024**.

Temporary RenderDoc instrumentation made the following isolated changes just
before this Draw. These are research probes, not shipped replay behavior:

| Probe | Selected output at (836,241) |
| --- | --- |
| Clone the original PSO without changing its settings | Zero |
| Clone the PSO and disable depth testing | Nonzero, selected RGBA matches the previously recorded native boundary |
| Keep depth enabled and change only its comparison to Always | Same nonzero selected RGBA |
| Copy depth into an identically described temporary resource and bind that copy; preserve original PSO | Nonzero |
| Create a new DSV heap/view but bind the original depth resource | Zero |
| Copy depth to a spare resource, restore the source state and bind the original depth resource | Zero |

The copy probe uses tracked-state barriers, `CopyResource`, and a transition of
the temporary depth target from COPY_DEST to DEPTH_WRITE. Its selected RGBA is
**(0.037872314453125, 0.03363037109375, 0.0281219482421875, 0.65478515625)**.
This differs from the earlier native sample and does not validate frame fidelity
or all depth-test outcomes. The temporary allocation has no optimized clear
value. The probes establish a dependency on the original depth attachment; they
do **not** establish that resource contents, compression metadata, or a particular
driver defect caused the discrepancy. At this investigation stage no copy or
disable-depth workaround had been added; the later opt-in experiment is described below.

One initial PSO probe failed creation with **E_FAIL** because copying the stored
expanded descriptor does not restore shader bytecode pointers. It supplied no
valid depth-disable result. The subsequent probes use `WrappedID3D12PipelineState::Fill`
and check successful PSO creation before interpreting their output.

Temporarily allowing D3D12 `GetDebugMessages` to also read its active API queue
yielded **727** medium messages: the prior 726 loading messages and one additional
duplicate-barrier warning (1008), with event **0**. That internal-stage warning
is not mapped to an original execution. No additional diagnostic explains the
depth discrepancy. This probe does not turn the unmodified API's loading-stage
report into evidence about analysis-stage validation; the production report's
scope text and `message_phase` now make that distinction explicit.

The restored production backend's resource-history query identifies the latest
clear of depth creation **325024** at **36140** (source **330358**) and its next
depth-target draw at **36149** (source **330367**). Barrier **36191** changes it
from DEPTH_WRITE to shader-resource state; it is bound for PS reads at **36209**
and **36386**. Barrier **36387** (source **350770**) restores DEPTH_WRITE before
the particle draws. The original creation record is `CreateCommittedResource`,
not a placed-resource alias. This is the next producer boundary to investigate;
bound-use entries alone do not prove a write or validate those depth contents.

Evidence under ignored `artifacts/dx12-research`:
`bf1-particle-{draw-arguments1,no-depth1,no-depth2,clone-pso1,depth-always1}`,
`bf1-particle-{depth-copy1,depth-rebind1,active-validation1,copy-original1,depth-history1}`.
The local research sources, PSO substitutions, depth-copy probes and active-message
hook were removed. The three-patch production DLL was restored to the previously
documented SHA-256; the worker with corrected diagnostic scope builds successfully
(`build219.log`). The particle boundary, full-frame variation and complete debug
acceptance remain unresolved.

### Opt-in D32 depth snapshots for isolated analysis draws

The latest depth producer was checked before implementing a workaround. Readback
of creation **325024** contains **483,840** finite values in **[0,1]**: minimum
**0**, maximum **0.3189068138599396**, with **163,146** zeros and no NaN/Inf.
At **(836,241)** it is **0.0011842424282804132**. Producer **36149** has depth
Always/write enabled; its PS debug trace outputs that same depth and red color
**50.157657623291016**. The color matches the selected GPU reference. This rules
out nonfinite/out-of-range stored depth in this readback, not an attachment or
driver defect. Evidence: `bf1-particle-depth-storage1`,
`bf1-half-depth-producer-{pipeline1,ps1}`.

The fourth pinned RenderDoc patch, `dx12-readonly-depth-snapshot.patch`, adds an
**experimental, default-off** path selected by `rdc-analyze --readonly-depth-snapshot`.
It is restricted to `texture` and `debug-pixel` jobs; other analysis requests
reject it, so it cannot silently add copies/waits to a counter measurement.
Stock libraries reject the request because the versioned export is absent.
Eligible isolated direct draws retain the original PSO and depth comparison,
but bind a GPU copy of their depth attachment. Eligibility requires enabled
depth with zero write mask, stencil disabled, a comparison other than Always/Never,
D32 view of a single-mip/single-layer/single-sample 2D texture, and one GPU node.
Indirect/mesh draws and other depth formats are outside this path.

The source resource is transitioned from its tracked state to copy source and
back. A temporary resource without an optimized clear value is copied and
transitioned to the view's depth-read/depth-write state. Raw temporary descriptors
preserve the color attachments and do not alter original resource identities or
the stored pipeline state. The GPU is flushed before releasing the temporary
resource/heap. Allocation failure fails analysis rather than silently running the
known-faulty path. Full replay and the source capture remain unchanged.
Reports include `backend_readonly_depth_snapshot_patch` and, when requested,
`readonly_depth_snapshot` with a decimal copy count, experimental flag and scope.
These counters describe execution of the workaround, not fidelity acceptance.

A reproducible experiment replaces only PS in original pipeline creation
**221693**, selected by source **255921** for draw **36560**. It uses the existing
experiment overlay, verifies saved-project loading and undo, and creates two
shaders. GPU checks were serial and limited to this failing boundary:

| Replacement PS | Native vs default isolated draw | Native vs depth snapshot |
| --- | --- | --- |
| Constant `(1,0.5,0.25,1)`, original rasterized depth | **390,497** differing pixels; (836,241) incorrectly zero | **0** differing pixels, entire **3,870,720** bytes identical; one snapshot |
| Same constant color, `SV_Depth` 0 on even columns / 1 on odd columns | Entire output identical | Entire output identical; one snapshot |

The second control checks rejection as well as visibility: **(836,241)** remains
zero while **(837,241)** receives the constant color. It does not reproduce the
original defect and is not itself a fix acceptance test. The first reproduction
retains rasterized depth and does reproduce the fault. The matching native and
snapshot hashes are respectively
**c6cd1c0ede67cd0bc3a544f8977af84bd4aa3f0bb4b1b5b470ef52262957e871** and
**f01fdb28a31893419335a9ed48b072dbecfe78b36e760f315fa5099a41cea0b3**.
Default rasterized-depth analysis hashes to
**68fe8258f39b50e17b7f5b247cc9ee8275d6f8a19769f83dbe1ab3cb869cb0c9**.
This difference between rasterized and PS-written depth narrows the reproduction;
it does not identify a particular driver/compression defect.

On the original unmodified particle shader the option restores a nonzero selected
pixel, but its color remains different from the earlier native run. At this stage
the variation remained unresolved; the subsequent UAV-ordering experiment below
narrows the selected particle case. This patch is not enabled in the
workspace by default and does not complete BF1 fidelity or shader-debug acceptance.
The targeted original PS debug query at **(836,241), primitive 1322** executes
three snapshots. Its reference changes from
**(0.037872314453125,0.03363037109375,0.0281219482421875,0.65478515625)**
before debugging to
**(0.035003662109375,0.031463623046875,0.02667236328125,0.65478515625)**
after trace creation and stepping. The report correctly retains
`selected_texel_changed` and a partial result. Nonzero references do not validate
the simulated invocation.

Evidence under ignored `artifacts/dx12-research`:
`bf1-depth-snapshot-project{1,2}`, `bf1-depth-snapshot-original1`,
`bf1-{depth-snapshot,raster-depth}-{native1,disabled1,enabled1}`,
`bf1-depth-snapshot-debug1`, `bf1-depth-snapshot-stock-rejection1`,
`depth-snapshot-comparison1.json`. The native and analysis processes use C++/GPU
replay; Python was used only to compare saved research bytes.
The strict pinned-source build verifies all four patches. The fourth patch SHA-256
is **576e6907b2e8e9408d011e2c761b8bdaffdc88c8881c1be41510b24c632b991a**;
the combined DLL SHA-256 is
**0d82ba6d600e0b3f5ce4652af975ee93a7250f9d5b8d21a2df0efff0aaff951a**.
The package manifest verifies the extra patch version/hash and includes its source.
Build **223** passed. Focused default/explicit option, invalid-type and invalid-action
checks: **6 passed**, **9 ms** (`rdc-depth-snapshot-options2.txt`). Saved GPU evidence
comparison: **3 passed**, **68 ms** (`depth-snapshot-evidence1.txt`). The project
generator/loading/undo check: **3 passed**, **18,714 ms** (`depth-snapshot-project2.txt`).
Counts include setup/cleanup. The stock-library request failed before opening a
capture with the explicit missing-patch-version error. No broad sample regression
or portable-package acceptance run was performed for this change.

### Particle color variation and explicit UAV-ordering experiments

Comparison of the original and depth-snapshot PS traces at execution **36560**,
pixel **(836,241)**, primitive **1322** narrowed the changed shader input to
**v3.xyz**: `(751.333862,603.336853,457.764252)` versus
`(864.018738,685.226074,511.111755)`. All other PS input components and constant
blocks match. The VS (hash **1d014928-d79835e6-0add66ba-34d44e5c**) computes this
color from vertex lighting and the per-vertex shadow factor in VB1, creation
**163437**. The primitive references vertices **1428,1431,1430**. One VS trace and
an independent calculation from saved buffers account for the higher interpolated
color; the discrepancy is already present in GPU-generated PS inputs, not only
in PS instruction evaluation.

A temporary input probe compared both vertex buffers, the index buffer and
VS/PS constant ranges before trace creation, afterwards and after stepping.
Only VB1 changed in that run (four floats, indices **72–75**); the selected
primitive's three shadow factors remained **0.45074862241744995**. Its selected
target texel was unchanged, so this particular probe does not attribute the
selected color change to those four elements. The probe was removed.

Resource history shows VB1 is copied at **35772**, source **265612**, from
shadow UAV creation **300425**, object **8697204528**. That UAV is cleared at
**35703** and read/modified by four fullscreen particle-shadow draws:
**35713,35732,35751,35770**, source records
**265553,265572,265591,265610**. The last draw's PS reads `u0[4*x]`, combines a
shadow-map comparison and previous-frame shadow, then stores four entries starting
at `4*x`. Its previous-frame input is separate creation **300500**. The recorded
list contains no `ResourceBarrier` between the clear and these draws; source
**265611**, after the last draw, transitions the UAV for the following copy.
All four PS bytecodes (pipeline creations **293249,301176,301178,301188**) contain
typed UAV loads followed by four stores; this finding is based on their decoded
instructions as well as the binding history.

The intervening single-vertex draws are **not** a recovered synchronization API.
Their PSO creation **59082** has the captured name
`GPUBreadcrumbManager::m_pipelineStateVs` (source **59083**). Its VS has no output
and only stores integer `1` into a separate structured UAV. No matching shader
hash was found in the installed GPA binaries. These observations do not establish
its original author, but they do not supply a UAV-ordering guarantee. Microsoft
requires ordering barriers when UAV read/write dependencies cannot safely execute
in arbitrary order; see the [UAV barrier definition](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_uav_barrier).

The original captured calls remain the default. A new **explicit experiment**
inserts a global UAV barrier immediately before selected direct Draw/Dispatch
execution instances:

```powershell
FloraGPA.Cli.exe replay <capture-directory> --event 36560 --id 325012 `
  --max-bytes 8589934592 --uav-barrier-before 35713,35732,35751,35770 --out <new-directory>
```

The generated `experiment.json` uses the existing capture-bound DX12 project
format. Each operation has `kind: uav_barrier`, `scope: before_execution`, a
decimal-string `event`, and boolean `value`. Existing project loading, undo/redo
and operation hashing apply; a false value removes the inserted barrier. The
target is an execution instance, so a repeated submission of the same recorded
draw is not implicitly edited. Unsupported target kinds, incomplete recordings,
wrong capture identities and malformed redo entries are rejected. CLI insertion
is supported by DX12 `replay` and `buffer`; there is no new workspace insertion
button in this change.

The barrier is executed even if its selected draw is separately disabled. It is
outside that draw's performance interval and inside its command-list timeline.
Reports list requested, recorded and GPU-completed insertions separately under
`native_replay.uav_barrier_experiment`, with an explicit synchronization/performance
warning. In independent captures each added barrier has its own auxiliary marker,
outside the original execution's API range. Original sources and event identities
are unchanged. This is a synchronization modification, not proof that the captured
application used this ordering or that the original full frame is correct.

With the four explicit barriers, the **original particle shader** produces the
same **3,870,720 bytes** in native readback and independent single-draw analysis
with the opt-in depth snapshot. SHA-256:
**a1cec82b979735c74db4dd23e7b41bcd189128ae376bbcd6fae0abfc029b7138**.
Selected PS debug references before debugging, after trace creation and after
stepping all equal
**(0.035003662109375,0.031463623046875,0.02667236328125,0.65478515625)**.
The original unmodified capture still has the earlier variation; these results
support a UAV-ordering dependency at this boundary, not full-frame/debug acceptance.

Evidence under ignored `artifacts/dx12-research`:
`bf1-particle-{vs1322-1,input-probe1,shadow-history1,shadow-source-history1,shadow-pipeline1}`,
`bf1-shadow-producer-shaders1`, `bf1-shadow-uav-barriers-{native1,texture1,debug1}`
(temporary research insertion), and
`bf1-shadow-barrier-experiment{1,-texture1,-debug1}` (the generic saved experiment).
All temporary input-readback and hardcoded insertion code was removed. Builds
**227/228** cover CLI, analysis worker, workspace and focused tests. Repeated
submission/project/undo validation: **3 passed**, **215 ms**; auxiliary event
mapping: **3 passed**, **2 ms**, including setup/cleanup. GPU checks were serial
and confined to the failing particle boundary; no broad regression was run.

### GPU fragment reference in pixel debugging

The PS workspace now offers **GPU fragment reference**, also available as
`rdc-analyze ... debug-pixel --primitive <id> --pixel-history-reference`.
It requests additional Pixel History replays after instruction stepping and
reports the selected fragment's `shaderOut` separately from the blended target.
The option is off by default, requires an explicit primitive, and currently
supports DX12 color target 0 with a single draw instance and an unambiguous
target subresource. Missing/duplicate matches and rejected fragments retain
explicit reasons instead of supplying a successful zero output reference.
The debugger trace is released before the extra history query.

`gpu_invocation_reference` preserves the native event ID, primitive, fragment
index, target/subresource, original backend failure flags, pre/post values and
GPU shader output. An output reference is unverified if the selected target
changes during debugging/history or if the history's final event value does not
exactly match direct post-event readback. These checks do not prove that all
inputs are stable. The original `gpu_reference` still means the complete
post-event texel, including other fragments, blending and storage conversion.

The PS panel also exposes the existing **D32 depth snapshot (experimental)**
option. Both options reset when the event/session/experiment context changes;
they do not enable the depth workaround globally. The depth option still requires
the pinned patched library and its existing restricted D32 path.

On the four-barrier BF1 experiment, execution **36560**, pixel **(836,241)**,
primitive **1322**, the original shader's **33-step** trace ends with `o0` equal
bit-for-bit to GPU Pixel History fragment **20**:
`(0.019068362191319466,0.01583746448159218,0.012434612028300762,0.427259236574173)`.
Its float32 bits are **1016870209,1015135634,1011595915,1054523833**. This is not
the final target value: later primitive **1334** and target storage yield
`(0.035003662109375,0.031463623046875,0.02667236328125,0.65478515625)`, which agrees
with native readback. Primitive **265** at the same pixel is a negative control:
the backend returns a trace but Pixel History reports `depthTestFailed`; its
invocation reference is unavailable and has no successful `value` field.

A second negative control disables only the D32 snapshot: the selected texel
remains zero during debugging, while Pixel History reconstructs the nonzero
final value above. The new boundary check reports
`unverified_history_boundary` and `partial: true`, rather than treating the
unchanged zero target as sufficient validation. With the snapshot enabled, the
history boundary and direct target match exactly and the reference is `read`.

Evidence under ignored `artifacts/dx12-research`:
`bf1-shadow-barrier-history1`, `bf1-particle-fragment-reference{1,2}`,
`bf1-particle-fragment-rejected1`, `bf1-particle-fragment-boundary1`,
and `pixel-fragment-ui3` (rendered panel).
The UI check compares all four GPU/trace output bit patterns, verifies separate
fragment/target display and export, checks the rejected-fragment explanation,
and rejects a stale result after a context change. This validates the selected
particle invocation under the explicit synchronization/depth experiments; it
does not complete original BF1 frame fidelity or general shader-debug acceptance.
Builds **229–233** cover the workspace, CLI, analysis worker and focused tests.
Option/default validation: **4 passed**, **9 ms** (`pixel-reference-options1.txt`).
Saved GPU evidence and panel integration: **3 passed**, **228 ms**
(`pixel-fragment-ui3.txt`), including setup/cleanup. Offscreen Qt emitted only its
expected `propagateSizeHints` warnings; the rendered panel was inspected.
GPU queries were serial and limited to this particle failure and its controls.

### Queue dependency index for restoring submission scheduling

`command-lists` and the shared session submission report now include
`queue_dependencies`. The cancellable CPU index combines captured per-queue
Submit/Signal/Wait order with Fence producer edges. It distinguishes waits
satisfied by a captured initial value from waits requiring an earlier or later
CPU-recorded Signal. It analyzes a requested record prefix independently, so a
Signal outside that prefix cannot satisfy a selected-boundary dependency.

The supported proof is restricted to unique Fence lifetimes with one signal
queue and monotonically nondecreasing values. A Fence can legally rewind and
multiple queues can race to signal it; taking the maximum observed value would
invent a guarantee. See Microsoft's [multi-engine synchronization documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization).
Those cases remain unresolved, as do reserved `UINT64_MAX` values. BF1 has two
successfully captured Fence creations with that initial value (records **475**
and **667**) but no queue operations referencing them; they are retained without
misreporting the captured creation calls as failed.

Two topological traversals distinguish cycles/downstream nodes from missing
dependencies and their downstream operations. No CPU-global edge is invented
between separate queues. Queue/Fence/record/value identities are decimal strings;
the index limits queue operations and Fence objects to one million each and
caps detailed invalid-creation/blocked-operation examples at 32.

The native CPU implementation matches the independent record audit:

| Capture | Queues with indexed operations | Submit/Signal/Wait operations | Initial-value waits | Signal-dependent waits | Unresolved waits |
| --- | ---: | ---: | ---: | ---: | ---: |
| BF1 | 2 | 108 | 0 | 0 | 0 |
| Talos | 3 | 84 | 0 | 0 | 0 |
| Hellblade2 | 4 | 584 | 124 | 29 | 0 |

All 153 Hellblade2 waits are frame records. Its 29 producer Signals precede
their corresponding Waits in the captured CPU record sequence. For example,
Wait **594612**, queue **2131165364544**, Fence **2131166327360**, value
**449717**, depends on Signal **594216** from queue **2131165365824**.
The 124 initial-value waits do not establish new cross-queue ordering edges.
No indexed graph cycles or unresolved queue operations were found in these
three complete record ranges. This does not resolve Talos's separate missing
occlusion results, prove resource synchronization, or validate GPU execution.

Evidence under ignored `artifacts/dx12-research`:
`queue-source-audit1.{json,txt}`, `queue-dependencies-bf12`,
`queue-dependencies-talos1`, `queue-dependencies-hellblade1`.
The fixture covers 64-bit identities, prefix truncation before a future Signal,
cross-queue cycles, missing upstream producers, rewinds, multiple writers,
reserved values, cancellation and shared-session report integration:
**3 passed**, **12 ms** including setup/cleanup (`queue-dependency-fixture3.txt`).
Build **237** covers the workspace, CLI, Worker and focused test. This change
performed no GPU replay; metadata and thread records were inspected without
reading the large resource contents.

### Native frame queue scheduling and completion observations

Frame replay now consumes the dependency index and submits captured GPU
Signal/Wait operations without draining every ExecuteCommandLists on the CPU.
Per-instance command lists/allocators and their referenced storage stay alive
until completion. Submitted and completed operations are reported separately;
experiment barriers and suppressed executions become completed only after a
GPU completion drain. `gpu_order_resolved` remains false: this change does not
certify complete original concurrency or all resource hazards.

Conservative drains remain before captured CPU waits, mapped writes, descriptor
updates, tile mapping changes, resource experiments and final readback.
GetCompletedValue observations wait only for the captured Fence value. The
index propagates the latest required CPU record through producer dependencies;
an observed value whose transitive producer is not yet submitted is rejected
before execution. A future GPU Signal separated from its Wait by a required
host drain is also rejected rather than allowed to deadlock. Initialization
remains serialized. At this milestone resource-state validation still followed
CPU submission order; the later dependency-ready scheduling change below
addresses the resulting future-producer transition failure.

Cancellation/failure with outstanding frame work removes this isolated replay
device before releasing in-flight objects. It does not count pending work as
completed. A fresh Worker/device is needed for retry. This uses
[ID3D12Device5::RemoveDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice).
The report includes queued/completed/pending submissions, host drain reasons,
Fence observations and whether the device was aborted. Maximum host pending
submissions is bookkeeping, not evidence of simultaneous GPU execution.

Focused evidence under ignored `artifacts/dx12-research`:

- `native-queue-fixture3.{txt,json}`: **4 passed**, **853 ms**, including
  setup/cleanup. The CPU graph checks cover transitive observation readiness.
  The two-queue GPU fixture queues a consumer before its producer, observes
  initial/completed Fence values without global drains, and executes a repeated
  command list as a distinct instance. Three submissions complete in one drain.
  Cancellation after the first submission leaves **1 pending / 0 completed**,
  aborts the device, and a fresh-device retry completes with zero D3D12 errors.
  A synchronous CPU wait before the future producer is rejected in preflight.
- `hellblade-queue-schedule2`: execution **37104**, source boundary **613374**,
  completes **125** frame submissions, **0** pending, **0** D3D12 errors.
  There are **42** global drains (21 CPU wait, 15 descriptor update, 5 mapped
  writes, 1 selected boundary), **627** Fence observations, and just **1**
  observation requiring a value wait. Maximum host pending submissions is **32**.
  The earlier `hellblade-queue-schedule1` implementation drained on every Fence
  observation: **162** drains and maximum pending **1**. The second run checks
  the specific observation refinement, not repeated whole-sample regression.
  The RGB10 readback for creation **506294** has SHA-256
  **2b88da15fdfaff2233486d3eb2380eb4fe30eb287a7590e5945a2b4b2d02c800**.
  Against `hellblade-last-color3`, **7,151 / 2,304,000** pixels differ, maximum
  RGB10 deltas **3/3/4**, alpha identical. Earlier replay variation already
  reached these maxima; this is not a claim of full frame fidelity or an
  original GPA comparison.
- `bf1-queue-experiment1`: saved four-barrier experiment, execution **36560**,
  creation **325012**, all four barriers reported completed, zero D3D12 errors.
  The **3,870,720** raw bytes exactly retain the prior experimental SHA-256
  **a1cec82b979735c74db4dd23e7b41bcd189128ae376bbcd6fae0abfc029b7138**.
  The typeless output has no default display interpretation; raw readback is
  the comparison evidence. This validates the existing experiment at its
  selected boundary, not the unmodified complete BF1 frame.

Builds **243–244** cover workspace, CLI, Worker and focused tests. The existing
ReplaySession preview-variable shadow warning is unrelated. GPU checks were
serial; Talos's known historical-query failure was not rerun by this change.

### Dependency-ready resource transitions and a measured Hellblade2 prefix

The two-queue fixture now uses a real 4x4 RGBA8 render-target texture initially
in COPY_DEST. Its producer transitions it to RENDER_TARGET. The consumer
transitions RENDER_TARGET to COPY_SOURCE and back, and its command list is
submitted twice. CPU records submit the first consumer before the producer,
with a GPU Fence Wait requiring the producer's later Signal.

The previous implementation fails this valid dependency order before reaching
the boundary: `native-queue-state-repro1.txt` reports source record **11**,
submission **18**, resource **60**, captured barrier state mismatch. Queue
submission order and resource-state checks now both follow dependency readiness.
An operation becomes available only at its captured CPU boundary and can issue
only after its per-queue predecessor and sufficient Fence producer have issued.
Native GPU Wait still enforces GPU completion; readiness does not CPU-wait for
the producer to finish. Recordings, generations and execution IDs are unchanged.
Host mutations cannot pass deferred operations, so delayed consumers cannot
silently acquire later descriptors or mapped contents.

The graph retains at most two prerequisite record IDs per queue operation.
The ready set selects the earliest available record and propagates satisfaction
through successors without repeatedly scanning blocked operations. Reports
separate issued, deferred and pending queue operations from GPU submission
completion counts, and retain the first 128 issued-record/CPU-boundary pairs.
Both identities remain decimal strings.

`native-queue-state1.{txt,json}`: **4 passed**, **714 ms**, including setup/cleanup.
The native issue order is **19, 20, 16, 18, 22**: producer submission, Signal,
consumer Wait, consumer submission and its repeated instance. Two operations
were deferred; all three submissions completed, with zero D3D12 errors. The
cancellation pass stops after issuing the producer and retains **1** submitted
but unconfirmed GPU batch plus **2** deferred queue operations. The isolated
device is aborted and a fresh-device retry succeeds. CPU tests also retain
transitive Fence-observation, cycle, missing-producer and prefix checks.

One real GPU run checks the scheduler and deferred completion calibration on a
Hellblade2 cross-queue prefix, not another complete-frame replay:
`hellblade-queue-performance1/report.json`. Execution **16641**, source
**585376**, is `Dispatch(17,1,1)` on compute queue **2131165364544**, list
**2149339111872**, generation **2**, ordinal **1**, submission **594640**.
The prefix completes **27** submissions / **112** queue operations with zero
pending operations and zero D3D12 errors. It contains four Signal-dependent
GPU Waits. Its producers already precede consumers in CPU order, so no source
operation is deferred in this real sample prefix.

The selected Dispatch's native timestamps are **1792366468334118240** and
**1792366468334125760**, frequency **1,000,000,000 Hz**, duration **7.52 us**.
The timeline has **72** measured/calibrated list intervals: **69** on the direct
queue and **3** on the compute queue. `schedule-validation.json` checks all
four producer-last-list / consumer-first-list brackets: positive gaps of about
**27.0–40.5 ms**, greater than their reported calibration drift/call envelopes.
No cross-queue timestamp interval intersection was observed. These are replay
completion brackets from one instrumented sample; they do not establish
original application overlap, GPU wait duration or stable performance.

Compute-list pipeline statistics remain unavailable; the supported query table
in Microsoft's [Counters and Queries specification](https://microsoft.github.io/DirectX-Specs/d3d/CountersAndQueries.html)
restricts this query to direct lists. No invocation count or hardware counter
was substituted. No texture was requested in this timing run, so it does not
add image-fidelity evidence. Builds **245–247** cover the fixture reproduction,
fix, workspace, CLI and Worker. Only the fixture and selected Hellblade2 prefix
used the GPU; BF1/Talos were not replayed for this change.

Still incomplete: general memory/alias hazards without sufficient captured
ordering, arbitrary Win32 event semantics, serialized initialization, and
selected consumers whose required producer lies beyond the inspected CPU
prefix. Those cases are not certified by this transition fixture or timeline.

### Hellblade2 original presentation and serialized backbuffer indices

The original GPA player now supplies a real Hellblade2 reference, rather than the
directory preview. GPA **2025.1.3.1269** ran offscreen through two Presents with
the synchronous screenshot layer (`frames=1;2`, `async=false`), exiting with code
0. Its second image is 1920 x 1200 and shows the character, rocks and letterbox
bars. `gpa-hellblade-reference1/screen-2.png` has SHA-256
`a726ee679bd5dd3a4126b850873dbb3e8e96042aec7eed9dcdefca378d63e678`.
The player log includes WinPixEventRuntime and adapter-mismatch warnings; this
comparison does not independently establish the original rendering adapter or
attribute pixel differences to those warnings.

A temporary debugger probe at screenshot-layer RVA **0x1e490** reads the
synchronous image before conversion. The first attempt reached the 120-second
deadline without saving raw pixels and was terminated. A retry with
`_NO_DEBUG_HEAP=1` completed with exit code 0 and saved the second Present's
**1920 x 1200 R10G10B10A2_UNORM** image, row pitch **7680**, **9,216,000** bytes.
This does not establish why the first attempt timed out. The probe changes no
capture files or GPA binaries on disk. Raw SHA-256:
`5d1abb33b97771ba68e91ce29557ad0a3dd187d8310503b233ed4d744cff0426`.
Compared with the existing native `hellblade-queue-schedule2` readback,
**6,402 / 2,304,000 pixels differ**; maximum RGB10 channel deltas are **3, 3, 4**,
with identical alpha. These differences remain unexplained and are not accepted
as full output fidelity. This investigation reused the saved native output;
it did not add another full native replay.

Screenshot conversion is a separate, now measured difference. A CPU fixture
passes all 1024 RGB10 gray levels through the original screenshot conversion,
with COM initialized. Every output RGB8 value is the high eight bits (`v >> 2`),
and alpha is 255. The same-run original PNG matches the saved RGB10 image under
this conversion at every pixel. Flora's normalized UNORM display conversion and
raw output are unchanged. The screenshot module's SHA-256 is
`b5a9414064c18a9450e509d5cccfa9c8b2aca25593526bc08753c682923343f3`;
relevant RVAs are **0x20340** (conversion entry), **0x20580** and **0x208a0**
(conversion paths), **0x1ee20** (scratch-image access), and **0x1f0c0** (release).

Capture-layer analysis explains why the live current index cannot identify a
serialized backbuffer directly. `Streams/layers/capture-x64.dll`, version
**2025.1.3.1269**, has SHA-256
`a1afd0b41e6bf668381e4898b03460130e0b61432c88d56c8559aef5b1b72c62`.
Targeted Ghidra analysis at image base `0x180000000` identifies
`AdjustBackBufferIndex` at RVA **0x64940** and `SynthesizeGetBufferCall` at
**0x91d60**, supported by function-name strings and disassembly. At **0x91e93**,
the latter loads swapchain vtable offset **0x120** for the call that reads the live
current index; at **0x91ec3** it accesses the description method. Instructions
**0x91efe–0x91f17** compute:

```
(resourceIndex + modeCorrection + BufferCount - liveCurrentIndex) % BufferCount
```

`modeCorrection` is 0 or 1; the mode's full policy is not established. Helper
**0x85a030** reads the resource index at state offset **0xa4**, and **0x85a110**
reads its owning swapchain at **0xa8**. The initial whole-module analysis hit its
time limit; these findings come from the subsequent targeted functions and
instruction inspection, not a claim of complete module analysis.

The real stream independently records swapchain **2130654364640** with four
buffers. Current-index observations **595596** and **613348** return **2** and
**3**; CopyResource **595601** and **613353** write resources obtained at
serialized indices **0** and **1**, respectively. The latter is resource
**2149283441408**, GetBuffer creation **506294**, preceding Present **613510**.
This is consistent with rotation, but does not justify imposing a guessed global
offset on other captures. Native replay continues to follow GPU resource identity.

The API decoder now exposes GetBuffer's swapchain, index, interface GUID and
output resource; GetCurrentBackBufferIndex's returned index; and Present's
interval, flags and result. These parameters participate in existing API text
and resource filtering. Session details and backbuffer names explicitly identify
serialized GetBuffer indices, distinct from captured current-index observations.
All object identities remain decimal strings. Focused tests cover high 64-bit
identities, GUID decoding, truncated/trailing data, and the actual Hellblade2
GetBuffer/current-index/copy/Present relationship. Release build **249** succeeded;
the two selected CPU tests report **4 passed, 0 failed, 0 skipped** including
setup/cleanup in **117 ms**. No broad regression suite was run.

Ignored local evidence: `gpa-hellblade-reference1`, `gpa-hellblade-raw1`,
`gpa-hellblade-raw2`, `hellblade-original-raw-comparison1.json`,
`hellblade-original-artifacts1.json`, `screenshot-rgb10-fixture1.json`,
`hellblade-swapchain-audit1.json`, `hellblade-swapchain-normalization2.txt`,
`hellblade-swapchain-normalization3.txt`, and
`hellblade-presentation-arguments2.txt` under `artifacts/dx12-research`.
The original-output comparison remains partial; intermediate-resource fidelity,
remaining debug features and portable acceptance are still unfinished.

### Hellblade2 output producer and strict analysis resource selection

Offline localization of the existing outputs finds **7,151 RGB pixels** differing
between `hellblade-queue-schedule2` and `hellblade-last-color3`, versus **6,402**
between the former and the original GPA output. Both differences occupy scene
rows **199–1001**, not the letterbox bars. Of the 6,402 original/native differences,
2,751 occur where those two native outputs agree. This three-output observation
does not establish a deterministic error, acceptable tolerance, or root cause.

Following the actual copied resource identifies creation **544605**, object
**2149283438960**, as the RGB10 source of CopyResource **613353**. Its captured
debug name is `nv.sl.dlss_g.tex2d.fake-swapchain-buffer`; that name alone does not
prove which algorithm causes the difference. One targeted native replay captures
through the preceding full-screen Draw, execution **37053**, source record
**611838**, submission **613257**, list **2144334559152**, generation **6**,
ordinal **1**, direct queue **2131165365824**. It exports this resource and a
**2,852,854,375-byte** RDC. The capture is analysis evidence, not a preview image.

RenderDoc 1.45 maps the selected Draw to event **91276**, VS `ScreenPassVS` and
DXIL SM6.6 PS `LensSoftnessPS`. The pipeline query maps its RTV to creation
**544605**, `InputTexture` to **544656** / object **2149346361024**, and
`SoftnessMap1`/`SoftnessMap2` to creations **511126**/**511100**. Viewport and
scissor are **1920 x 803 at (0,199)**. PSO/root-signature creation mapping remains
unavailable; these are potential shader bindings, not observed per-instruction
accesses. The native raw output SHA-256 is
`3fda4fe6a245919c7ae1e4ea639ea12e2e91e669e23cd9dda465833aadc13749`.
It differs from original GPA's final Present at **6,297 RGB pixels**, maximum
RGB10 deltas **3,3,4**. At this earlier boundary, all 762,240 pixels outside the
viewport are still zero including alpha, whereas original final-Present alpha
is 3 everywhere. This expected boundary distinction is separated from the
unexplained RGB differences, not counted as another color-replay defect.

At pixel **(541,451)** the saved native RGB10 value is **(86,121,160)** versus
original **(85,120,156)**. One PS query yields **230 instruction steps** for
primitive 0, with no embedded source files or available PDB. The selected GPU
texel changes during debugger/PixelHistory replays. The existing consistency
checks correctly report **`unverified_replay_changed`**; neither the simulated
trace nor the fragment output is accepted as validated shader execution.

The input-history query also exposed a direct Worker validation defect. A job
containing `resource_creation: "544656"` without `resource` silently selected the
current RTV (creation 544605), then labelled the report with the requested
creation. The workspace's `prepareRdcJob` already rejected that combination, but
direct Worker requests bypassed it. The Worker now requires a nonzero decimal
string object/creation pair before opening RenderDoc, and rejects creation
selection for a DX11 capture. It can no longer fall back to the RTV for that
malformed explicit selection. Four process-level invalid-selection cases pass
without opening a GPU capture (**6 passed including setup/cleanup, 1,954 ms**),
with release build **250**.

After the fix, a complete object/creation request returns the actual input,
**ResourceId::693**, creation **544656**, and **11 mapped usage entries** with
zero unmapped uses. Its latest ColorTarget binding is execution **37041**, source
record **611825**; transition **37052** precedes PS binding at **37053** in the
same submitted list. These are resource-usage evidence, not measured content
changes. This identifies the next upstream comparison boundary. The malformed
`hellblade-output-input-history1` result is excluded from input-resource evidence.
The earlier debug query also used creation without object and is retained only
as a diagnostic of the implicitly selected RTV; it is not a validated explicit
resource-selection query.

Ignored evidence under `artifacts/dx12-research`: `hellblade-difference-localization1.json`,
`hellblade-output-producer1`, `hellblade-output-pipeline1`, `hellblade-output-ps1`,
`hellblade-output-input-history2`, `hellblade-output-evidence1.json`, and
`rdc-resource-identity1.txt`. All three successful analysis reports list no loaded
GPA modules. GPU capture/analysis ran serially; no full-sample sweep, repeatability
loop or additional original-GPA replay was performed. Output stability, original
intermediate comparison and full debugging acceptance remain unfinished.

### Hellblade2 changed DXIL PS execution and undo

Further queries of the existing RDC identify execution **37041** (source
**611825**) as another `LensSoftnessPS` variant. It reads creation **571418**,
object **2149346363472**, and writes creation **544656**. Its input history has
**15 mapped usage entries**, including multiple barriers at the same execution;
these are not 15 distinct GPU actions. The latest ColorTarget binding is execution
**37029**, source **611812**. That pipeline uses `ChromaticAberrationPS`, reading
creation **538909** plus displacement maps **264** and **511220**. This extends
the known dependency chain; it does not establish where the small original-output
differences first arise. All these queries reuse the saved RDC.

The branch now has actual changed-DXIL execution evidence for Hellblade2, beyond
the earlier byte-preserving PSO reconstruction. `hellbladeLensShaderProjects`
resolves the PSO selected at execution **37053** from the original command-list
recording: creation **516663**, object **2144063289680**, pipeline-stream kind,
root signature **2130678974784**. It replaces only the PS using the existing
`Experiment::setShader` overlay, saves the project, calls undo and saves the
restored project, then calls redo and verifies that the reloaded modified project
matches. No capture bytes are edited. The replacement has pipeline-lifetime
scope, applying to all uses of that captured PSO, not only the selected Draw.

The versioned source is `tests/fixtures/dx12-solid-magenta.hlsl`, compiled with the
installed Windows SDK 10.0.26100.0 x64 `dxc.exe`, entry `main`, target `ps_6_6`:

```hlsl
float4 main() : SV_Target0 { return float4(1, 0, 1, 1); }
```

Recompiling the versioned fixture yields the same **2,516-byte** DXIL asset used
in the experiment. Its SHA-256 is
`a055d6ca631bf89c706375397008ba5ce6efdc6ebbb597df1e4919fe932ccd12`;
the original PS is
`b7102e6005be2b941f05ae8f841fe539ed1b0d84d00fdf2a4e3e40b35556e366`.
DXC is a development fixture compiler, not a new replay dependency.

One native replay with `modified.json`, event **37053**, resource creation
**544605**, the debug device and a 12 GiB allocation budget reconstructs the
edited PSO and executes successfully. All **2,304,000 raw RGB10 pixels** match
the exact expected image: **1,541,760** pixels in the 1920 x 803 viewport at
(0,199) are packed **0xfff003ff**, and **762,240** pixels outside it are zero.
There are **zero D3D12 validation errors**. Modified raw SHA-256:
`323629e3a6909e4f676e16594780245d4c4ad16b02cc674627632615bda02404`.

One corresponding native replay with the saved undo state reports no active
shader replacements and restores the visible scene. Every viewport pixel differs
from the fixed magenta output, viewport alpha is 3, and pixels outside remain
zero at this boundary. It also reports zero D3D12 validation errors. Restored raw
SHA-256 is `170af2c6f2bf5d55fdd1af41bbe7b690e3a982bcd525867a464ea51fc2f239ff`.
Compared with the earlier unmodified producer readback, **4,633 pixels differ**,
with maximum RGB10 deltas **2,3,4**. Thus undo restores the original PSO and scene,
but exact original-output restoration remains unproven because replay variation
is unresolved. No tolerance is accepted. Redo project bytes equal the modified
project bytes; redo was not another GPU replay, and this check does not claim
Hellblade2 workspace cache-invalidation coverage.

Both reports retain capture identity
`bd0f79862269d4dc291ec0d8354c647c1f64d1b9122e7d7d8dd20f7efa161941`,
with distinct experiment document hashes and cursors **1** and **0**. Preparation
passes **3 tests including setup/cleanup, 50,096 ms** with release build **251**.
To reproduce preparation, set `FLORA_DX12_CAPTURE_DIR`,
`FLORA_HELLBLADE_SHADER_FILE` to the compiled fixture, and
`FLORA_HELLBLADE_SHADER_PROJECT_OUTPUT` to a new directory, then run the single
`FloraDx12SessionTests hellbladeLensShaderProjects` case. GPU validation uses the
normal CLI `replay` command with `--event 37053 --id 544605 --debug-device
--max-bytes 12884901888 --experiment <modified-or-restored.json>` and separate
new output directories. It is not added to the default GPU test workload.

Ignored evidence: `hellblade-prelens-pipeline1`, `hellblade-prelens-input-history1`,
`hellblade-prelens-source-pipeline1`, `hellblade-magenta-projects1`,
`hellblade-magenta-native1`, `hellblade-magenta-restored1`, and
`hellblade-shader-experiment-evidence1.json` under `artifacts/dx12-research`.
Exactly two native GPU replays served the change/undo check, serially; no new RDC
capture, original-GPA run, all-sample sweep or repeatability loop was added.

### Captured pipeline/root-signature identities and creation navigation

Native PSOs and root signatures now receive names containing their original
object identity and creation record. The common pipeline creation path applies
the marker after PSO construction, including shader replacement; the marker
continues to identify the original pipeline lifetime. Root signatures use their
resolved creation record, not a neighboring Draw or a raw pointer without lifetime.
No GPA runtime is involved.

The RenderDoc pipeline adapter parses only exact markers with canonical nonzero
decimal uint64 values. References expose `captured_object`, `object_creation` and
`object_kind` with `mapping_status: capture_object_marker`. Texture/buffer
`resource_creation` remains separate: pipeline objects must not be sent to the
texture/resource browser. Shader-stage ownership is addressed separately below.
Existing RDCs without these names retain their unmapped status; the adapter does
not infer creation identities from generated RenderDoc IDs or shader names.

The DX12 Bindings panel adds **Locate creation** for mapped PSO/root-signature
rows; double-clicking these rows has the same effect. It switches the API list
to CPU records, restores visibility when filters hide the creation, and selects
that exact record. Texture/buffer rows retain **Inspect resource**. Busy state,
selection/experiment changes and invalidated results disable the new action.

One Talos replay/capture at execution **12675** and one subsequent RenderDoc
pipeline query verify the complete marker path. The native replay reports zero
D3D12 errors and zero validation messages. RenderDoc 1.45 event **144** returns
29 pipeline rows, including:

| Object | Captured identity | Creation record | RDC identity |
| --- | --- | --- | --- |
| PSO | 1657098320 | 156847 | ResourceId::7825 |
| Root signature | 1658516128 | 102742 | ResourceId::7824 |

The workspace check decodes both original creation packets, compares their
object IDs with the analysis report, then navigates both rows while the API list
is filtered. Both land on the correct raw record, clear the obstructing filter,
and invalidate the old pipeline result (**3 passed including setup/cleanup,
1,656 ms**). CPU checks cover full-width 64-bit marker round trips, malformed
markers, distinct object/resource actions, busy state, numeric-JSON rejection,
legacy unmapped rows and stale experiment results. Release build **252** covers
GUI, CLI, Worker, analysis backend and tests; build **253** adds the focused
legacy-report assertion (**4 passed including setup/cleanup, 46 ms**). No
additional GPU replay was needed for that assertion.

Ignored evidence under `artifacts/dx12-research`: `talos-object-markers-native1`,
`talos-object-markers-pipeline1`, `pipeline-object-cpu2.txt`, and
`pipeline-object-workspace1.txt`. The analysis report lists no loaded GPA modules.
BF1/Hellblade2 were not replayed for this change. Their previously saved RDCs do
not retroactively acquire the new markers. Full pipeline coverage, shader stage
mapping, original-output fidelity and portable acceptance remain unfinished.

### Bound Shader stage ownership and direct replacement

Shader rows now retain their RenderDoc shader identity and separately describe
the original owning PSO and stage. `capture_pipeline_stage` contains a nested
`pipeline` reference and `shader_stage`; it does not pretend that a DX12 shader
blob has an independent captured resource-creation call. Ownership comes from
the currently bound PSO, so a shared RenderDoc shader ID cannot supply a global
PSO mapping. Without a valid PSO marker, old RDCs remain unmapped.

Each reflected Shader row also reports the currently bound bytecode's SHA-256
and byte count. The scope explicitly includes experiment replacements. Ownership
of an original PSO stage is not a claim that its current bytes equal the original
shader. Missing reflection/raw bytes leaves the digest unavailable.

The DX12 Bindings panel supports **Locate creation** on these Shader rows and
adds **Replace shader…**. The import targets the row's PSO/stage directly and
states that replacement affects all uses of that pipeline. It uses the existing
experiment overlay, stage validation and PSO reconstruction. Textures retain
their resource-inspection action. A busy worker disables import; changing the
session or experiment while the file chooser is open discards that old target.

The original Talos execution **12675** reuses the previously captured marked RDC.
Its VS maps to PSO creation **156847**, object **1657098320**, with **892 bytes**
and SHA-256 `81f4477676e4cb210da4408c77314811ab233c99192d3709a222fa8bdc3daec5`.
The workspace check compares this digest against the original pipeline recipe,
imports the existing depth-0.25 VS from the Shader row, then saves an experiment
targeting exactly that PSO/VS. Experiment change invalidates the displayed result
and rejects the previous request (**3 passed including setup/cleanup, 11,239 ms**).

One native replay/capture of that saved experiment reaches the same Draw with
zero D3D12 errors. The subsequent pipeline query still identifies original PSO
creation **156847** and VS, but reports **1,172 bytes** and SHA-256
`d064afb7f6c1ab01305773dc38feea9364fddc1f886a30a1c92e91748f40bb7a`.
This exactly matches the imported asset and differs from the original digest.
RenderDoc-local shader/PSO IDs change between captures without changing the
captured stage owner. Both analysis reports list no loaded GPA modules.

Focused CPU/UI checks cover shared shader IDs with different PSO owners, 64-bit
creation identities, invalid stage/row associations, numeric identity rejection,
busy/stale results, and cancellation of the old import target when its chooser
outlives the context (**3 passed including setup/cleanup, 110 ms**). Release build
**254** covers the application, CLI, Worker, analysis service and tests; build
**255** adds the modal-context check. The offscreen Qt plugin emits its existing
`propagateSizeHints` warning during file chooser tests; no test fails.

Evidence under ignored `artifacts/dx12-research`: `talos-stage-pipeline1`,
`talos-stage-workspace1`, `talos-stage-replaced-native1`,
`talos-stage-replaced-pipeline1`, `pipeline-stage-evidence1.json`,
`pipeline-stage-cpu2.txt`, and `pipeline-stage-workspace1.txt`.
Only the changed Talos Draw required a new native replay/capture; its original
query reused the preceding capture. BF1/Hellblade2 were not replayed. GPU
verification of this stage-mapping change covers VS; other stages still need
their own evidence. Full shader debugging, original-output stability and portable
acceptance remain unfinished.

### Indexed resource history and the Hellblade2 postprocessing chain

An explicit DX12 Resource History query now reads RenderDoc's capture usage
index without calling `SetFrameEvent` again. The query still filters entries
through the selected execution event and retains resource lifetime resolution,
original execution provenance and separate replay initialization records.
Implicit target selection still replays the selected event to resolve its RTV;
value/state analysis and the DX11 path retain their existing behavior.

This follows the pinned RenderDoc 1.45 source: `ReplayController::GetUsage`
delegates to `D3D12Replay::GetUsage`, which reads the command queue's
`m_Cmd.m_ResourceUses` index. No selected pipeline state is consulted for an
explicit resource. Reports expose `selected_event_replayed`, `query_source` and
`capture_loading_may_execute_gpu_work`; skipping the additional event replay
does **not** mean that opening an RDC performs no GPU work.

Release build **256** rebuilds the analysis service. One targeted query reuses
the saved Hellblade2 output-producer RDC, selecting execution **37029**, resource
object **2149346346336**, creation **538909**. Compared with the preceding result
that replayed the selection, all **14 usage entries**, **3 initialization
records**, identities, event mappings and existing scope fields are identical.
All uses retain mapped original executions; no GPA module is loaded. Only the
new scope fields and the process's loaded-module inventory differ. This verifies
index equivalence for this resource, not measured pixel changes or complete
resource-history semantics.

That input's latest recorded ColorTarget use is execution **37017**, source
record **611799**. Pipeline inspection identifies `MappedDistortionPS`, reading
`InputTexture` object **2149346343888**, creation **553172**, plus distortion maps
created at **511198** and **511208**. The established output chain is now
`MappedDistortionPS` (37017), `ChromaticAberrationPS` (37029), then two
`LensSoftnessPS` draws (37041 and 37053). These are binding/usage observations;
they do not yet locate where output variation first arises.

A follow-up explicit history query for creation **553172** returns **22** mapped
usage entries without replaying the selected event. Its latest ColorTarget use
is execution **37003**, source **611784**. That Draw's PS entry is `MainPS`, with
bindings for `ColorTexture` (creation **571530**), `BloomTexture` (**570329**),
`EyeAdaptationBuffer` (**512823**), `SceneColorApplyParamaters` (**551586**),
`BloomDirtMaskTexture` (**506117**) and `ColorGradingLUT` (**530084**). This provides
a concrete upstream output/input boundary for subsequent fidelity comparison;
the binding names alone do not establish its complete shader behavior.

A CPU comparison of two previously saved original GPA second-Present PNGs
finds **1,762 differing RGB pixels**, maximum per-channel RGB8 delta **1**,
within bounds `(0,199)` through `(1917,1001)`. One original run used the raw-image
debugger probe with `_NO_DEBUG_HEAP=1`, so this is an observation across those
existing runs, not a controlled repeatability experiment. It establishes neither
a cause nor an acceptable tolerance for Flora's raw RGB10 differences.

Ignored evidence: `hellblade-chromatic-input-history1`,
`hellblade-chromatic-input-history2`, `hellblade-prechromatic-pipeline1`,
`hellblade-distortion-input-history1`, `hellblade-predistortion-pipeline1`,
`hellblade-indexed-history-evidence1.json`, and `hellblade-original-variation1.json`
under `artifacts/dx12-research`. Full output fidelity and all unfinished feature
acceptance items remain open.

### Hellblade2 contraction-sensitive DXIL debugging discrepancy

A targeted `MainPS` query at execution **37003**, source **611784**, pixel
**(541,451)**, primitive **0**, returns **317** DXIL SM6.6 steps. The selected
target remains unchanged across debugger and Pixel History replays, and the
history's final event value exactly matches the target. The GPU fragment
reference is therefore eligible for comparison, but the simulated RGB output
does **not** match it. No source/PDB is available for this shader.

| Component | Simulated final output | GPU fragment output before storage |
| --- | --- | --- |
| R | 0.04267866164445877 | 0.04204192012548447 |
| G | 0.05200344696640968 | 0.05136670544743538 |
| B | 0.08223690837621689 | 0.08160016685724258 |

The last RGB operations add a shared quantization dither. Its hash starts with
`sin(y * 543.31 + x)`, followed by multiplication by `493013` and a fractional
part. Captured interpolant bits are **1049650790** and **1052812248**. The
RenderDoc trace separately rounds multiplication and addition; its angle is
**204.70242309570312**, hash **0.96875**, and dither
**0.00073313782922923565**. The pinned DXIL interpreter executes `FMul`/`FAdd`
separately, while `Sin` itself already uses a GPU helper.

The small SM6.6 compute reproduction in
`tests/fixtures/dx12-contracted-dither.hlsl` runs both fast and `precise`
expressions on the NVIDIA device (vendor **4318**, device **11352**). The fast
path produces angle **204.70243835449219**, hash **0.59375**, and dither
**0.000096395102445967495**; the precise path exactly reproduces the debugger's
angle, sine, product, hash and dither bits. Adding the fast dither to the three
pre-dither values from the trace reproduces the GPU fragment RGB bits exactly;
adding the precise dither reproduces the simulated output bits exactly.
The fixture is compiled with Windows SDK **10.0.26100.0** DXC, `-T cs_6_6 -E main`.
Its two input pairs intentionally use separate constant loads: the first
research attempt shared loads, allowing common-subexpression elimination to
make both paths precise, and did not distinguish contraction.

This localizes the selected discrepancy to contraction-sensitive dithering.
It does not directly observe intermediate values in the original GPU shader,
establish a general interpreter fix, or explain variation between whole-frame
runs. Applying fused arithmetic to every simulated multiply/add would not
establish equivalence to the driver's compiled program, so no such change or
numeric tolerance has been introduced.

The analysis service now compares the final mapped target-0 shader output with
an eligible GPU fragment reference as exact 32-bit components. It uses the
reflected color-output signature and debugger variable mappings, excludes input
mappings, and declines ambiguous, missing, removed or unsupported output shapes
and references from changing replays. Mismatches mark the result partial and
appear in the debugging panel. Exact output matches explicitly leave intermediate
instructions and input stability unverified. The comparison is attached to the
existing opt-in `pixel_history_reference` query; ordinary stepping does not add
another GPU query.

Release builds **257–258** cover the analysis service, application and focused
tests. CPU checks, including the saved real trace, pass (**4 passed including
setup/cleanup, 184 ms**). A single repeated query at this same invocation
verifies the new report flags all three RGB components as mismatched. The
workspace displays that report, preserves the warning while stepping and clears
it on session change (**3 passed including setup/cleanup, 486 ms**). No new full
native capture or other sample replay was needed.

Ignored evidence under `artifacts/dx12-research`: `hellblade-main-ps1`,
`hellblade-main-ps2`, `hellblade-main-trace-values1.json`,
`hellblade-dither-probe2.json`, `hellblade-dither-evidence1.json`,
`pixel-output-comparison1.txt`, and `pixel-output-ui1.txt`. The numerical
debugging discrepancy remains an explicit unfinished backend gap.

### Hellblade2 MainPS boundary comparison and failure reports

The MainPS output (creation **553172**) and `ColorTexture` input (creation
**571530**) were read at execution **37003** using independent native prefix
queries and the existing output-producer RDC. Both are 1920x1200 textures.
Captured `_RootShaderParameters` values at offsets **176** and **288** establish
both color-input and output viewport rectangles as **[0,1920) × [0,803)**.
The comparison uses that rectangle; it does not infer validity from the full
allocation extent.

| Resource | Format | Differing RGB pixels in the rectangle | Maximum channel differences |
| --- | --- | --- | --- |
| MainPS output, 553172 | RGB10A2 | 8,646 | 47, 50, 69 RGB10 levels |
| ColorTexture input, 571530 | R11G11B10_FLOAT | 10,842 | 0.0078125, 0.0078125, 0.015625 |

The largest output difference is at **(1596,0)**: RDC RGB10 `(243,300,455)` versus
native `(196,250,386)`. The input's largest decoded difference is at **(1549,42)**:
RDC `(0.6796875,0.6328125,0.84375)` versus native
`(0.6796875,0.6328125,0.828125)`. All compared input texels are finite. Outside
the declared rectangle, **358,202** raw input texels differ and the native
readback contains **17,820** nonfinite texels; these are reported separately,
not counted as valid-rectangle output failures. Whole-resource initialization
and sampling coverage are not established by this comparison.

These are separate replay queries, not same-run snapshots or original GPA
measurements. They establish an upstream input discrepancy between native and
RDC playback; neither its cause nor its contribution to native/original final
frame differences is established. Both successful native queries report zero
D3D12 validation errors and SDK **616**. The first output attempt used a 4 GiB
allocation budget and failed during initialization; it was retried with the
previously successful Hellblade2 **12 GiB** budget.

Resource History for creation **571530** reports **25** mapped uses. Its latest
potential compute UAV uses are four `ExecuteIndirect` executions **36766**, **36770**,
**36775**, **36779**, source records **611527**, **611532**, **611539**, **611544**.
The last maps to exactly one generated Dispatch at RenderDoc event **90443**,
submission **613234**, list **2152464446256**, recording generation **10**.
Pipeline inspection identifies `MainCS`, binding creation **571530**, mip **0**,
as `SceneColorOutputMip0`. Its other output bindings named `SceneColorOutputMip1`
and `SceneColorOutputMip2` refer to separate resources **570673** and **562718**,
both with view mip **0**. Names alone must not be interpreted as mip indices.

An original GPA intermediate-output attempt selected command list
**0x1f3444cb7b0**, Draw/Dispatch ordinal **17**, resource **0x1f46f067bd0**,
RTV views only, with the existing synchronous dump-layer raw-image probe.
GPA exits **0xc0000005** during restoration before producing an image. A second
diagnostic launch identifies both first- and second-chance access violations
at `dump-resources-layer-x64.dll` RVA **0x80c8**, reading address **0x8**.
The installed DLL's SHA-256 remains
`853913de932f235341655d7f55f1d38d68e56d60df45402acff87825eaf78b72`.
Read-only Ghidra inspection locates the fault in a tree-access helper at RVA
**0x80b0** (`MOV RBP,[RCX]`). One caller at RVA **0xa360** passes a device-private
data result plus eight without checking it. A subsequent debugger launch records
**RCX = 0x8** and return RVA **0xa476**, confirming that caller and a null
device-private state result. The reason the state is absent remains unresolved.
A targeted `--post-layer` launch, placing the dump layer after implicit playback,
fails at the same instruction with the same caller and produces no image. This
configuration change does not resolve the missing state. No installed binary or capture is
modified, and no original intermediate image is claimed.

The allocation failure also exposed a Worker reporting gap: replay exceptions
were rethrown before `report.json` or the response identity could be saved.
The native replay path now persists failed reports with the execution, requested
resource, allocation budget, failing phase, available device/replay diagnostics
and `completed:false`. It returns a failure exit code and retains the stderr
error, while the existing request envelope is written normally. Partial files
are explicitly not completed images. Capture hooks remain alive until replay
and device teardown. Errors before a valid replay request is prepared retain
their existing argument-validation behavior.

Release build **259** covers CLI, Worker and the focused session test; build
**260** corrects the test's expected failure phase to the observed
`initialize_transfers`. With a **1-byte** allocation budget, the Talos Worker
returns failure, saves the report before any resource allocation, and echoes
request **18446744073709551615** and experiment revision
**18446744073709551614** as strings. The old response is rejected after changing
the request. A fresh Worker retry with **4 GiB** reaches execution **12675** and
reproduces the existing texture SHA-256
`c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`.
This focused failure/retry check passes (**3 passed including setup/cleanup,
4,447 ms**); it does not exercise every possible device/driver failure. Build
**261** names the separate construction phase `create_replay`; the observed
allocation failure remains in `initialize_transfers`.

Ignored evidence under `artifacts/dx12-research` includes
`hellblade-main-boundary-evidence1.json`, the `hellblade-main-output-*` and
`hellblade-main-color-*` reports, `gpa-hellblade-main-raw1/2`, and
`hellblade-dump-crash*.txt`, `allocation-failure-report2`, and
`allocation-failure-test2.txt`. Original-output fidelity, upstream execution
semantics and complete initialization remain unfinished.

### Executed invocation parameters and compute bounds

DX12 pipeline reports now include an **Execution / Invocation parameters** row.
It records indexed/non-indexed Draw counts and offsets (including signed base
vertex), Dispatch dimensions, the generated indirect action index and the
RenderDoc event. Compute shader reflection supplies the thread-group dimensions.
The action's optional group-size override is separate: zero means no override,
not an unknown reflected shader size. Indirect dimensions come from RenderDoc's
indexed action metadata; nondeterministic arguments can differ on later replays.
This report does not certify stability across replay passes.

DX12 compute debugging checks all three Group and Thread coordinates before
invoking `DebugThread`. It rejects zero-workgroup actions, missing reflected
dimensions, out-of-range coordinates and a dispatch-thread coordinate exceeding
32 bits. A valid result includes the checked group, local thread and derived
dispatch-thread coordinates. Existing DX11 behavior is unchanged.

For Hellblade2 execution **36779** / source **611544**, RenderDoc event **90443**
reports **1174 x 1 x 1** groups and `MainCS` reflection reports **16 x 16 x 1**
threads per group. This is a nonempty indirect Dispatch. A targeted request for
Group **[1174,0,0]**, Thread **[0,0,0]** returns `ok:false` with
`Selected compute group is outside the executed Dispatch dimensions`, producing
only a failure report and no debug trace. A nonempty Dispatch and its UAV
bindings do not prove that every bound output is written.

Release builds **262–264** cover the analysis worker and focused tests. The CPU
boundary check covers each axis, the last valid invocation, zero counts, missing
shader dimensions, absent action overrides, coordinate overflow and signed Draw
offsets. The workspace check consumes the real pipeline report, verifies that
the invocation row displays its recorded dimensions without resource actions,
and rejects it after an experiment change. Both checks pass (**3 passed including
setup/cleanup** each, **1 ms** and **106 ms** respectively). These checks do not
launch additional GPU replays. The saved pipeline report and rejected GPU debug request are under
`hellblade-main-color-producer-pipeline3` and `hellblade-indirect-invalid-group1`;
GPA fault evidence is under `gpa-hellblade-main-raw3` and
`gpa-hellblade-main-post-raw1`. All are ignored research artifacts. Valid UAV
store validation and the upstream color discrepancy remain unfinished.

### Hellblade2 indirect compute invocation and selected stores

The nonempty indirect Dispatch **36779** / source **611544** / RenderDoc event
**90443** now has a real **332-step** DXIL SM6.6 `MainCS` trace for Group
**[0,0,0]**, Thread **[0,0,0]**. The shader hash is
`f5f587ed3dd543f56c253d8121285e2d`. No source/PDB is present; this evidence uses
the recorded instructions and SSA values. `TileListOffset` is **18360** and
the selected tile-list entry is **2424884** (`0x250034`). The shader's coordinate
calculation maps this invocation to **(832,592)**, despite a dispatch-thread ID
of **[0,0,0]**.

At trace step **325**, next instruction **612**, the store to
`SceneColorOutputMip0` uses creation **571530**, view mip **0**, coordinates
**(832,592)** and RGB **(0.046875,0.0400390625,0.0419921875)**. These operands
match the selected GPU texel's float32 bits exactly:
**1027604480,1025769472,1026293760**. The GPU selection is unchanged before
debugging, after trace creation and after stepping in this request. RGB-only
R11G11B10 storage synthesizes alpha **1** on readback; the shader's alpha **0**
is not compared. This verifies these selected operands against GPU output, not
every instruction, all input stability or every bound UAV modification.

The same invocation also reaches a store to the separate output creation
**570673**, mip **0**, at **(416,296)**. Its WaveReadLaneAt-derived RGB operands
are **(0.043701171875,0.0369873046875,0.0390625)**. In the targeted second-output
request, GPU RGB changes from **(0.04345703125,0.037109375,0.0390625)** to
**(0.04345703125,0.03662109375,0.0390625)** after trace creation; the latter value
persists after stepping. The existing consistency gate marks the result partial
and rejects this reference as validation. Storage conversion and changing
replay inputs have not been isolated, so this does not establish a Wave operation
defect. The third output's store coordinates are **(-1,-1)** and are not evidence
of an in-bounds texture modification.

The CS workspace displays the checked Group, local Thread, group counts,
reflected group size and dispatch-thread ID with the trace. It explicitly
distinguishes that ID from shader-computed texture coordinates and preserves
the indirect-argument stability limitation. Clearing/changing the experiment
removes the invocation information; old reports without it remain readable.
The shared PS fragment-reference display retains its mismatch warning.

Build **265** covers the workspace and focused UI test. The saved real reports
verify SSA Watch values at the first store, forward/backward stepping, invocation
display, stale-result rejection, legacy-report handling and the second output's
changing-reference warning. The related PS mismatch-display check also passes:
**4 passed including setup/cleanup, 963 ms**. The rendered CS panel was inspected.
Three serial analysis requests were used: one to identify the coordinates and
one per selected output; there was no new native replay or GPA launch.

Ignored evidence: `hellblade-indirect-valid-group1`,
`hellblade-indirect-reference1/2`, `hellblade-indirect-trace-values1.json`,
`hellblade-indirect-store-evidence1.json`, `indirect-compute-ui1.txt` and
`indirect-compute-ui1/replay-debug/indirect-compute.png`. The downsampled output,
other invocations and the upstream whole-resource discrepancy remain unverified.

### Hellblade2 indirect CS replacement and undo

The current native execution **36779** / source **611544** uses PSO creation
**512699**, captured object **2130520907296**, root signature
**2143972372848**. This ownership comes from the command-list recording and
captured pipeline recipe, not the older RDC's unmapped pipeline ID. Its original
CS is **12,936 bytes**, SHA-256
`ff26fc08d0880e1ef3dbac56bad4111a6b74853fa6561aeba882c17cb2906e93`.

The versioned fixture `tests/fixtures/dx12-tile-magenta-cs.hlsl` retains the
captured `t0` TileListsBuffer, `b0` viewport and tile-offset layout, `u0` output,
and **16 x 16** Morton lane addressing. It writes **(1,0,1,0)** to valid selected
tiles. The Windows SDK **10.0.26100.0** compiler, `cs_6_6`, entry `main`, `-O3`,
produces **4,300 bytes**, SHA-256
`886d2f0680d3ef3783f814fca40fe7a1385a76448f0d85169122bb96af526378`.
The original capture stays read-only; replacement applies through the existing
pipeline-lifetime experiment overlay.

A native replay to **36779**, reading creation **571530**, with a **12 GiB**
allocation budget and D3D12 debug layer creates the replacement PSO and produces
**295,552** exact R11G11B10 marker texels (`0x780003c0`). They form **1,150** full
16 x 16 tiles and **24** bottom-clipped 16 x 3 tiles, matching the indexed
**1,174** groups. All clipped tiles are at tile row **50** (pixel rows 800–802).
The selected **(832,592)** texel is the exact marker. No exact marker occurs
outside the declared **1920 x 803** active rectangle. The remainder of the
1200-row allocation is not claimed initialized or validated. The displayed
readback was inspected; acceptance uses raw storage, not PNG color conversion.

The saved undo project removes the shader replacement, and a second native
replay to the same execution restores the selected texel's original raw value
**1389662880**. No marker remains in the active rectangle. Both GPU runs finish
with zero D3D12 validation errors. The restored image differs at **7,336** active
texels from the earlier independent native readback at **37003**, after the
last recorded UAV use of this output. That comparison is not an exact same-run
baseline, and full-image undo repeatability remains unverified. The modified
and redone project files are identical; cursor 1/0 and capture identities are
preserved in the modified/undo replay reports.

Build **266** and `hellbladeIndirectShaderProjects` prepare and reload the
actual CS experiment, verify undo/redo and locate its original PSO (**3 passed
including setup/cleanup, 54,041 ms**). The existing PS preparation shares that
helper without changing its selected event or stage. Exactly two serial native
GPU requests were used for the changed and undo states; no new analysis capture,
GPA process or all-sample regression was needed. Native output SHA-256 values:

- Modified: `fdec1f94a0c34b8c564611bfdc462286445d2c5840345d4e187f70c1fbc2e1bb`.
- Restored: `dc2b4998b6d902fe0555de2bb3657118f81ff577616f963e1d92179847131dbd`.

Ignored evidence: `hellblade-cs-projects1`, `hellblade-cs-projects1.txt`,
`hellblade-cs-magenta-native1`, `hellblade-cs-restored-native1`, and
`hellblade-cs-experiment-evidence1.json`. This adds a verified DXIL CS modification
at an actual indirect boundary; full CS stage coverage, downsampled-UAV fidelity
and whole-frame repeatability are still unfinished.

### DX12 pipeline shader CLI export

`shader` now accepts a DX12 directory with `--id <PSO creation record>` and an
explicit `--pipeline-stage VS/PS/CS/HS/DS/GS/AS/MS` (case-insensitive). It exports
`shader.dxbc` or `shader.dxil`, plus `shader.json` and `report.json`. The reports
preserve the capture identity, PSO/root objects, selected stage, format, byte
count and SHA-256 as checked metadata; 64-bit identities and sizes use strings.
DXBC also gets system-compiler disassembly when available. DXIL bytecode is
validated as executable DXIL, not passed to the DX11 signature-only inspector.
The initial export reported missing DXIL disassembly; the native adapter below
now fills that gap. Replay debugging remains a separate operation.

`--experiment <project>` exports the selected stage's current replacement or
original bytes after undo, retaining the original digest, revision and project
digest. Stage, missing pipeline, out-of-range identity and incompatible project
errors remain failures. Export performs no GPU work and does not validate PSO
creation or root-signature compatibility. Example:

```powershell
FloraGPA.Cli.exe shader "D:/CDXrepo/captures/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame" --id 512699 --pipeline-stage CS --experiment modified.json --out shader-export
```

Release build **268** passes `dx12ShaderCliExport` (**3 passed including
setup/cleanup, 95,537 ms**): Talos VS **156847** exports the known 892-byte DXBC
and assembly; Hellblade2 CS **512699** exports the exact modified and undone
digests from the preceding GPU experiment. Missing stage, absent CS slot and
record **18446744073709551615** are rejected with specific errors. The first
build exposed an MSVC string/JSON comparison ambiguity, corrected before these
checks. A DX11 GF2 `shader --id 102` export also succeeds with the existing
bytecode, assembly and metadata outputs. An initial GF2 request using an older
research reference **887** correctly failed because it is not a shader in this
capture; inventory supplied the current **102** identity. No GPU tests ran.
Ignored evidence is in `shader-cli-export1`, `shader-cli-export1.txt`, and
`shader-cli-dx11-compat2`.

### Native DXIL disassembly and deployment

DX12 `shader` export now fills the preceding DXIL disassembly gap using
`IDxcCompiler3::Disassemble` in-process. It loads only the explicit `--dxcompiler`
path or `DXCompiler/dxcompiler.dll` beside the executable, checks the pinned
SHA-256 before loading, and reports the runtime release, path, digest and method.
Missing/mismatched runtimes and DXC errors leave the checked bytecode export
available with a specific disassembly failure reason. The output is DXC's LLVM
instruction text, not recovered HLSL or RenderDoc debug instruction numbering.

The official **v1.8.2505.1** x64 release ZIP has SHA-256
`9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4`;
the compiler DLL is
`5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9`.
`third_party/dxc/runtime-manifest.json` pins both DLLs and the original release
notices. `tools/deploy_dxc.ps1` validates and deploys an extracted release;
`tools/package.ps1 -DxcRoot <root>` includes it in the portable package.
Runtime binaries remain outside Git and license bytes retain upstream CRLF.

Build **269** and the focused test check the actual original/replacement
Hellblade2 CS exports (entry points, texture stores, original WaveReadLaneAt),
malformed bytecode, missing DLL and wrong DLL digest: **3 passed including
setup/cleanup, 491 ms**. Build **270** also compiles the updated CLI integration
test; the full capture export suite was not repeated. A fresh portable package
with the patched RenderDoc backend successfully exports original CS **512699**
with PATH restricted to Windows and System32. Its report selects the package's
DXCompiler DLL and its **52,521-byte** `shader.asm` hashes to
`334096b209f5004c4e58ffe5f274605089eab1fc60ab388877ffb872df71de12`.
No GPU query or replay was run. This verifies portable offline DXIL inspection,
not the still-unfinished three-sample portable replay/analysis acceptance.
Ignored evidence: `dxil-disassembly1.txt`, `dxil-portable-package1.log`,
`dxil-portable1`, and `dxil-portable-shader1`.

### Instruction breakpoint management in the shared debugger

The Breakpoints panel now lists instruction breakpoints alongside source rules.
Previously, DXIL without source metadata could stop on an instruction breakpoint,
but the panel remained empty and the instruction IDs appeared only in the summary.
Selecting an instruction row now locates its recorded disassembly line without
moving the trace. Remove deletes that instruction breakpoint. The panel preserves
the selected rule while stepping and reports instruction visits, including loop
revisits; assembly marks all mapped instruction breakpoints and the current step.
Source conditions and hit-count rules remain source-only and their controls are
disabled when an instruction row is selected. No source mapping is invented.

Release build **274** passes four focused cases (**6 passed including setup and
cleanup, 1,036 ms**). The actual Hellblade2 execution **36779** saved CS trace
sets instructions **612/614**, saves/imports their configuration through the UI,
stops at instruction 612 / step **325**, and shows Watch `_643 = 832`. Removing
612 leaves 614 as the next breakpoint. The check also covers busy-state controls,
experiment invalidation and rejection of the old result. A separate minimal UI
fixture covers mixed DX11 source conditions/instruction rules, repeated visits
and removal; it is not evidence of GPU source debugging. Existing compute
invocation display and selection invalidation cases pass too. The breakpoint
screenshot was inspected. Ignored evidence is `dxil-breakpoints-ui2.txt` and
`dxil-breakpoints-ui2/replay-debug/dxil-instruction-breakpoints.png`.

This work consumes saved traces and performs no GPU replay. It does not resolve
the outstanding DXIL numerical mismatch, changing UAV results, absent source
data or full stage-coverage requirements.

### DX12 vertex invocation bounds and BF1 half-precision inputs

VS Debug now checks the selected vertex ordinal and instance against the indexed
Draw parameters before calling RenderDoc DebugVertex. Indexed selections require
a bound 16/32-bit index buffer and an in-binding, overflow-safe read; truncated
data, negative/overflowing base-vertex results and strip-cut markers are rejected.
An explicit DX12 index must agree with the resolved invocation. DX11 retains its
existing index handling. The report and VS workspace label distinguish Draw-local
ordinal, instance, raw index, base vertex, actual vertex and index-buffer byte
offset. The 64-bit byte offset remains a decimal string. Bounds use the captured
action index; changing indirect arguments remain an explicit limitation.

The RenderDoc 1.45 API contract defines `vertid` and `instid` as Draw-relative
ordinals and `idx` as the input index with Draw offsets applied (vendored
`third_party/renderdoc/replay/renderdoc_replay.h`, DebugVertex). Strip-cut handling
uses the current strip topology and configured marker; a matching value in a
triangle list remains an ordinary index. See Microsoft's
[strip-cut definition](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_index_buffer_strip_cut_value).

Three serial analysis jobs on the existing BF1 recapture provide focused evidence:

- `bf1-input-half1`: execution **36560**, VSIn attribute **1 / TEXCOORD1**,
  16-bit float4 at byte offset **16** in an **80-byte** stride. The input has
  **3,316** vertices, **4,974** 16-bit indices and **1,658** triangle-list faces.
  All **13,264** components convert bit-exactly from retained half bytes to the
  float32 table/export. Raw vertex SHA-256 is
  `f351e596c83ed3ef92052aa24821019f401d806775e9828c4c271df791f9170f`;
  raw index SHA-256 is
  `59dd2e1242c7a945d00e5d6ee0bad70f934e5b49254e7b94b5a436c146d833db`.
- `bf1-vertex-invocation1`: ordinal **3966**, instance **0**, reads index **1428**
  at byte offset **7932**, with base vertex **0**. Its **159-step** VS trace input
  `v1` matches geometry vertex 1428 exactly:
  `[0.498046875, 1, 0, 0.43994140625]`.
- `bf1-vertex-invalid1`: ordinal **4974** is rejected as outside the Draw, with
  `ok: false` and no trace. Capture loading/event replay still occurs; the check
  prevents starting the invalid vertex debugger, not all GPU work.

Build **277** fixes the initial missing namespace qualification and builds the
backend; **278/279** build the application and focused UI tests. CPU vertex and
compute boundary cases pass (**4 including setup/cleanup, 1 ms**), covering
non-indexed offsets, negative base vertices, offsets above 4 GiB, empty Draws,
bad strides, truncation, strip cuts and overflow. The real input/trace UI case
and selection test pass (**4 including setup/cleanup, 300 ms**): vertex 1428 is
shown in the table, the invocation label reports the mapping, CSV/raw exports
retain their bytes, and stale/legacy results behave correctly. Both screenshots
were inspected. Evidence is `vertex-invocation-bounds1.txt`,
`bf1-vertex-input-evidence1.json`, `bf1-vertex-ui1.txt` and `bf1-vertex-ui1/`.
This verifies the selected input and invocation mapping; it does not establish
all transformed geometry, VS instruction numerical fidelity or whole-frame
replay correctness.

### Mesh selection to VS Debug and BF1 post-VS position evidence

Replay Mesh now offers **Debug first reference** for a selected DX12 input or
VS-output vertex. The decoder records the first Draw-local index/vertex ordinal
that references each displayed row; repeated indices keep their first occurrence
and unreferenced rows remain unavailable. Navigation opens VS Debug with that
ordinal and the selected instance without starting a worker. It does not assume
that a displayed vertex row equals the Draw invocation ordinal. GS/DS and mesh
shader output are excluded because they need a different invocation mapping.
VS-output reports now include the Draw parameters and instance. The UI requires
their index/vertex count to agree with the returned mesh before enabling this
navigation; legacy output reports still display but do not guess the mapping.
The backend rejects out-of-draw mesh instances, and the UI rejects a mismatched
returned instance.

For BF1 execution **36560**, displayed vertex **1428** is referenced at Draw
ordinals **3966** and **3969**. Both input and post-VS navigation select **3966**,
instance **0**. Main-window verification checks tab selection and invocation
fields, busy state, stale event invalidation, mismatched instances and old-report
behavior. Model checks cover negative base vertices, repeated indices,
unreferenced rows and nonindexed ordinals. Build **280** found a shadowed
`request` method call in the UI; build **281** fixes it and builds the main
application. Three focused UI cases pass (**5 including setup/cleanup, 1,397 ms**),
and the mesh model checks pass. Local evidence is `mesh-reference-model1.txt`,
`bf1-mesh-navigation1.txt` and `bf1-mesh-navigation1/`.

Two serial post-VS queries (`bf1-particle-vsout1/2`) return **3,316** vertices in
a **112-byte** output stride and **4,974** indices. All position components agree
between those GPU reads; **12 other output components differ**, so the complete
output buffer is not deterministic. Vertex 1428's GPU clip position is
`[1.4245448112487793, 1.2639257907867432, 0.05993271619081497, 5.648245334625244]`.
Compared with `bf1-vertex-invocation1`'s final simulated `o0`, X/Y/Z match exactly,
but W is one ULP higher: GPU bits **1085587053**, trace bits **1085587052**.
The trace W comes from instruction **55**, `dp4 r2.w, r1.xyzw,
crViewProjMatrix[3].xyzw`, then moves to `o0` at instruction **265**. At this
milestone the cause was not established; the investigation below addresses this
specific discrepancy. `bf1-postvs-evidence1.json`
retains the comparison and raw output hashes. These selected position and
navigation checks do not complete full geometry or Shader Debug fidelity.

### DXBC GPU dot products and BF1 position correction

The selected instruction's exact `r1` input bits are
`[1077645600,1062452736,1084430336,1065353216]`; its matrix operand is `cb2[17]`,
with bits `[1060697147,1007756523,1060174738,0]`. RenderDoc 1.45's CPU DP4 path
rounds the products separately and adds X/Y/Z/W sequentially. A native DX12
microprobe using `tests/fixtures/dx12-bf1-dot-order.hlsl`, compiled by SDK
10.0.26100.0 FXC as `cs_5_0` with `/O3`, reproduces the discrepancy with these
runtime inputs: actual GPU `dp4` gives **1085587053**, while precise separate
sequential arithmetic gives **1085587052**. A different addition order also gives
1085587053, but this does not justify imposing that CPU order on every adapter.
The fixture's source-level `mad` chain was compiled into separate precise
operations; its fourth component is **not** evidence of GPU fused MAD behavior.
Probe output, shader/assembly and the native helper remain in
`artifacts/dx12-research/bf1-dot-probe1*` and `bf1_dot_probe.cpp`.

`dx12-dxbc-gpu-dot.patch` extends the pinned RenderDoc 1.45 math helper with
two operands and GPU DP2/DP3/DP4 operations. DX12 non-`precise` DXBC dot
instructions use this path; destination masks and saturation still pass through
the existing interpreter. DX11 and `precise` dot instructions keep their existing
CPU path. Missing math PSO, device removal or command/readback failure stops the
affected thread and increments a failure counter. Flora's isolated backend
rejects the incomplete trace if any GPU dot failed, including helper-lane
failures; there is no CPU fallback after a failed GPU operation. Result metadata
records the patch version and decimal-string operation/failure counters. The
build and packaging scripts require the additional patch hash in the manifest.

The patched runtime builds successfully; Flora backend build **282** passes.
One serial BF1 VS query (`bf1-vertex-gpu-dot1`) reuses the existing RDC and exact
execution **36560**, ordinal **3966**, actual vertex **1428**, instance **0**.
It completes **159 steps**, with **13** GPU dots (DP2/DP3/DP4, including saturated
DP4 destinations) and **0** failures. Final `o0` bits are
`[1068914556,1067567186,1031109634,1085587053]`, exactly matching all four position
components in both previously saved GPU post-VS reads. Instruction 16's DP3
also changes a temporary value by four ULP; this value is overwritten on the
following instruction. The selected position match does not validate every
temporary or other output. `bf1-dot-evidence1.json` records the before/after
states and probe. Runtime SHA-256 is
`814a0c02f7626cbb296b1158fafb4e5ad00952dee48fc6ca926004fb81c548b1`;
the job's module inventory contains neither GPA nor Python.

Only the reproduced dot discrepancy is resolved. Precise/minimum-precision
coverage, cross-adapter results, GPU failure injection, large-workgroup cost,
other arithmetic and complete Shader Debug fidelity remain unverified. This
change does not fix Hellblade2's DXIL contraction-sensitive dither. No full
capture replay, DX11 GPU regression or portable-package acceptance was added
for this focused numerical check.

### VS position reference in the debug workspace

DX12 VS Debug now reads an instrumented GPU `SV_Position` after stepping and
compares its four raw float32 components with the final reflected debugger
output. It uses `GetPostVSData(VSOut)` for the selected instance/view 0, then the
returned post-VS index mapping and base vertex. The original capture's vertex
index is not assumed to be the post-VS row. CPU readback requests cover only the
selected 16-byte position and, for indexed draws, one 16/32-bit index;
RenderDoc's preceding stream-output extraction still processes the mesh.

Missing position semantics, unsupported formats, missing/truncated index data,
count mismatches, invalid instance offsets and out-of-range positions report
an unavailable reference with a reason. Checked 64-bit offsets are serialized
as decimal strings. Bounds follow the pinned D3D12 backend's full buffer size
and equal instance blocks, preventing an index from reaching a neighbouring
instance. GPU fatal errors reject the job. Output comparison reuses the existing
reflected mapping logic used by PS debugging, handles nested output variables,
and never substitutes an input or removed/stale variable for the final output.

The existing VS invocation label shows the GPU position, exact match/mismatch,
or unavailable reason; its tooltip retains the mapping and raw bits. A mismatch
marks the result partial. A match explicitly leaves other outputs, intermediate
instructions and input/replay stability unverified. Legacy reports still show
their invocation information, and session/request invalidation clears the
reference. No new panel or separate replay workspace is introduced.

Builds **283/284** pass for the backend, application and focused tests. One GPU
query, `bf1-vertex-position1`, selects execution **36560**, Draw ordinal **3966**,
instance **0**. Returned index **1428** at byte **7932** maps to position byte
**159936** in the 112-byte vertex stride. All four position components match
the **159-step** trace exactly, including corrected W bits **1085587053**;
the job records **13** GPU dots and **0** failures. The UI case and selection
invalidation pass (**4 including setup/cleanup, 354 ms**), and its screenshot
`vs-position-ui1/replay-debug/bf1-vs-position.png` was inspected.

Build **285** adds the instance-block guard; CPU addressing/comparison checks
pass (**4 including setup/cleanup, 1 ms**) with offsets above 4 GiB, negative
base vertices, index remapping, cross-instance bounds, truncation, missing
semantics/mappings, nonindexed reads and exact one-bit differences. Saved
Hellblade2 PS mismatch evidence and its UI still pass (**3 including setup/cleanup
each, 146/339 ms**); neither check replays the GPU. Evidence is
`vs-position-bounds2.txt`, `vs-position-ui1.txt`, `vs-position-ps-compat1.txt`
and `vs-position-ps-ui1.txt` under `artifacts/dx12-research`.
No additional full-capture replay or multi-instance GPU validation was performed.
Other VS outputs and complete geometry/debugging acceptance remain unfinished.

### VS output attribute inspection

Replay Mesh can now select individual DX12 VS output attributes for its table
and CSV export. The selector uses the pinned RenderDoc 1.45 D3D12 stream-output
layout: reflected signature order, position moved first and expanded to four
components, and four bytes per component. The backend records semantic,
register, type and byte offset, and requires the packed size to match the GPU
stride. Unsupported types retain a reason in Details. This mapping does not
claim support for GS/DS/MeshOut layouts. Float32, uint32 and sint32 values are
decoded without rounding integers through float32.

Attribute changes use the already-read vertex buffer without another GPU job.
The preview and OBJ keep vertex positions; CSV follows the selected attribute.
First-reference VS invocation navigation remains available, and a new session
or request clears the selector and exports. DX12 VSOut readback now bounds the
requested bytes to the selected instance block instead of requesting the entire
multi-instance buffer from an instance offset. The block size and offset must
agree with the executed Draw; multi-instance GPU validation remains pending.

Builds **286/287** pass for the application, backend and focused tests. One GPU
query, `bf1-output-attributes1`, reuses the existing BF1 RDC at execution
**36560**, instance **0**, returning **3,316** vertices and **4,974** indices.
Its seven float4 outputs are SV_Position and TEXCOORD0–5, at byte offsets
**0/16/32/48/64/80/96** in a **112-byte** stride. The **371,392-byte** raw vertex
buffer SHA-256 is
`09853694ebd511f0d963a261a9baa11262b2a351406e97ea4be8b630a2b28893`.
The UI check compares all six non-position attributes of vertex **1428** with
the raw bytes, verifies unchanged OBJ and attribute-dependent CSV, restores
the original position CSV, and navigates to Draw ordinal **3966** without a new
worker request. It also checks stale-session rejection (**4 tests including
setup/cleanup, 177 ms**). The inspected offscreen screenshot demonstrates the
selector/table, not rendered preview geometry. CPU checks cover output packing,
integer extrema, stride boundaries and truncated vertices. Evidence is
`vs-attribute-layout1.txt`, `vs-attribute-model1.txt`, `vs-attribute-ui1.txt` and
`vs-attribute-ui1/replay-mesh/bf1-output-attribute.png`.

Comparison with the saved `bf1-vertex-position1` debug trace confirms exact
position and TEXCOORD0/1/3/5 values for that vertex. **Five other components
differ**: TEXCOORD2.xyz and TEXCOORD4.xy. Their GPU/trace raw bits are respectively
`[1138534039,1136693506,1134585707]` /
`[1139849511,1137649475,1135208482]`, and `[949519973,1058042225]` /
`[949518336,1058042226]`. `bf1-output-attributes-evidence1.json` preserves this
comparison. These are separate geometry/debug jobs; the differences have not
yet been attributed to replay variation or interpreter arithmetic. They remain
open fidelity issues, without tolerance-based acceptance. No additional full
capture replay or portable-package acceptance was performed.

### DXBC GPU multiply-add and BF1 output correction

The TEXCOORD4.xy discrepancy above comes from VS instruction **269**,
`mad o5.xy, v2.zwzz, l(4.001250,...), l(0.500000,...)`. The exact operand bits
are **3187663360/1015251207**, multiplier **1082133053**, addend **1056964608**.
The old debugger evaluates separate CPU multiply/add, yielding bits
**949518336/1058042226**. All three saved GPU post-VS buffers instead contain
**949519973/1058042225**. A native DX12 microprobe using
`tests/fixtures/dx12-bf1-mad.hlsl`, compiled by SDK 10.0.26100.0 FXC `/O3`
as `cs_5_0`, retains a DXBC `mad` and reproduces the GPU bits exactly. The
fixture also tests instruction 267's other two input components; those already
agree. Probe source, compiled assembly and JSON are `bf1_mad_probe.cpp` and
`bf1-mad-probe1.*` under ignored `artifacts/dx12-research`.

`dx12-dxbc-gpu-mad.patch` adds a third operand to the existing GPU math helper
and routes DX12 non-`precise` DXBC MAD through it. It does not fuse separate
MUL/ADD instructions, change DXIL, or change the DX11/precise CPU paths. MAD
and dot operations share command submission/readback checks but have separate
decimal-string operation/failure counters. A failed MAD stops the affected
thread, and Flora rejects the incomplete trace, including failures from helper
lanes. The six-patch build and packaging manifest now require the MAD patch
hash. Missing MAD support remains visible as patch version zero in reports.

The patched runtime and backend build **288** pass. One serial BF1 debug query,
`bf1-vertex-gpu-mad1`, selects execution **36560**, ordinal **3966**, vertex
**1428**, instance **0**. It completes **159 steps**, **24** GPU MADs and **13**
GPU dots, with **zero** failures. The executed instruction list accounts for
all 24 MADs; captured inputs, constant blocks and control flow match the prior
trace. TEXCOORD4.xy now matches all three saved GPU buffers exactly; other
final outputs are unchanged. The same job's four-component GPU position
reference still matches. The complete selected vertex matches both older
`bf1-particle-vsout1/2` buffers; the latest `bf1-output-attributes1` still differs
at **TEXCOORD2.xyz**. No GPA/Python module is loaded by the debug job.
`bf1-mad-evidence1.json` records these comparisons. Runtime SHA-256 is
`b94e33a20335dedd947a7b1f91de193c60b53c30f649315f7d20b8b1600621f2`.

The remaining three components are known to vary between GPU reads; this turn
does not attribute that specific run to an input or synchronization change.
Their VS dependency includes the slot-1 per-vertex shadow factor, whose earlier
ordering investigation is recorded under Particle color variation above.
Precise/minimum-precision behavior, other adapters, failure injection and
large-workgroup cost remain unverified. Hellblade2's DXIL contraction discrepancy
is unaffected. Validation was limited to one microprobe and one saved-RDC debug
query; it does not complete full Shader Debug or portable-package acceptance.

### VS output comparisons and selected input observations

DX12 VS Debug now reads the selected post-VS vertex's reflected output layout
and compares each supported output with its final debugger mapping. This extends
the existing remapped-index/instance-bounded position reference; it reads one
vertex stride, not the complete output buffer. Float32, uint32 and sint32
components are compared as exact bits. Missing mappings, unsupported types,
noncontiguous component masks and incomplete/duplicate signature layouts remain
unavailable; an empty comparison cannot pass. Mismatches mark the trace partial.
The existing workspace invocation area reports matching, differing and
unavailable output counts, names differing semantics, and exposes component bits
and provenance in Details/the tooltip. Legacy position-only reports still work.

The backend also observes the selected original index and regular input-layout
attributes before DebugVertex, after stepping and after the instrumented output
replay. It bounds each read to the selected attribute, checks binding limits and
64-bit address arithmetic, handles per-instance step rates, and serializes
resource identities/offsets as strings. It checks the first snapshot's index
against the invocation actually supplied to DebugVertex. Unsupported packed
formats and truncated data are explicit missing observations. Changed complete
observations mark the trace partial. Unchanged observations are not a claim that
inputs remained stable during execution; constants, textures and other vertices
are outside this check.

The new typed-layout check found and fixes a regression in the preceding mesh
attribute implementation: RenderDoc `CompType::UInt/SInt` are **4/5**, whereas
**2/3** are UNorm/SNorm. Integer mesh decoding and selector labels now use the
correct values, and normalized formats are rejected instead of being interpreted
as integers. BF1's seven float outputs were unaffected by that bug. CPU tests
cover signed/unsigned extrema and values beyond float32 integer precision;
integer GPU-output coverage is not claimed.

Builds **289–292** pass for the affected backend, application and tests. The
initial typed-output check failed on the enum mismatch and passes after the fix.
Final addressing/layout/comparison checks pass (**6 including setup/cleanup,
1 ms**); the added signed/unsigned comparison case passes (**3 including
setup/cleanup, 1 ms**). The mesh model checks also pass. These cover offsets above
4 GiB, instance step rates, append offsets, truncation, binding overflow, output
packing/remapping, unavailable references and exact one-bit differences.

One GPU query, `bf1-vertex-all-outputs1`, uses execution **36560**, ordinal
**3966**, vertex **1428**, instance **0**. All **28 components** of its **7**
outputs match in this query; the **159-step** trace executes **24** GPU MADs and
**13** GPU dots without failure. All **9** selected input attributes (**84
bytes** per observation) and the index are unchanged at the three points.
Slot-1 TEXCOORD8 at byte **5712**, resource **5189**, contains shadow-factor bits
**1055312006** (`0.45074862241744995`) at every point. The job loads neither GPA
nor Python. This observation does **not** resolve the earlier cross-query
TEXCOORD2.xyz variation.

The workspace check verifies the real report, injected mismatch/unavailable
display states, old position-only reports and stale-session rejection (**4
including setup/cleanup, 452 ms**). The inspected screenshot is
`vs-all-output-ui1/replay-debug/bf1-vs-outputs.png`. Other local evidence is
`bf1-vertex-all-outputs-evidence1.json`, `vs-output-reference-bounds3.txt`,
`vs-output-integer-comparison1.txt`, `vs-output-integer-model1.txt` and
`vs-all-output-ui1.txt`. No repeated GPU query, full-frame replay or portable
acceptance run was added. Complete shader instruction fidelity and replay
stability remain unfinished.

### Explicit ExecuteIndirect action selection

The analysis index now retains a zero-based generated-action ordinal and action
count in each ExecuteIndirect action's reverse provenance. It keeps the original
execution, source record, submission, queue and command-list generation; API
begin/end wrappers remain excluded. A new selector resolves an explicit ordinal
within that execution. Automatic selection still requires exactly one action;
empty expansions, out-of-range ordinals, duplicate markers, and an indirect
selector on a direct call are rejected. Multiple actions no longer have to be
collapsed into one execution-map entry to become individually selectable.

`rdc-analyze` accepts `--indirect-action N` together with `--execution`. VS/PS/CS
debugging exposes the same optional **Indirect action** field. Results retain
the selected ordinal, count and replay EID. Changing the field invalidates the
trace; the view rejects mismatched ordinals and ambiguous automatic results, and
clears the field when switching execution/session/backend. The subsequent
navigation milestone below extends this identity to histories and Replay Mesh.

DX12 inventory now exports `indirect_executions`, including generated EIDs and
their original provenance. It lists capture-loading metadata without an extra
SetFrameEvent replay; DX11 inventory retains its previous path. Loading the RDC
can still perform GPU initialization/work. The inspected BF1 RDC contains **11**
single-action indirect Dispatches. The inspected Hellblade2 RDC contains **1,270**
single-action expansions: **374 Dispatch**, **122 MeshDispatch**, and **774 Draw**.
Neither inspected RDC contains a multi-action expansion, so actual multi-action
GPU replay is **not verified**. These counts describe the saved RDCs, not every
possible capture or an unexamined tail beyond their recorded boundaries.

Builds **293–296** pass. Mapping checks cover automatic/specific selection, two
distinct generated Draws, repeated-source provenance, wrapper exclusion, invalid
ordinals and duplicate markers (**4 including setup/cleanup, 2 ms**). Job
preparation preserves a decimal-string execution above 2^53 and rejects invalid
or missing-parent selectors (**3 including setup/cleanup, 9 ms**).

One actual CLI query selects BF1 execution **35808**, indirect action **0**, and
CS group/thread **(0,0,0)**. It completes **256** steps at replay EID **42909**,
source **265648**, submission **364657**, queue **861708976**, list **10653671952**,
generation **1**. API wrappers **42908/42910** are excluded. The indexed dispatch
has **1226×1×1** groups with **256×1×1** threads. The query's MAD counter includes
**6,400** GPU operations across the simulated workgroup, with no failures; this
is not a performance benchmark or full output-fidelity check. Neither GPA nor
Python is loaded. Evidence is `bf1-indirect-selection1`,
`indirect-selection-evidence1.json`, and `indirect-selection-inventory-evidence1.json`
under ignored `artifacts/dx12-research`.

The UI test uses that actual report and separately injects multi-action metadata
to check rejection/selection behavior; it does not manufacture multi-action GPU
evidence. Stale responses, wrong ordinals, automatic ambiguity, malformed input
and DX11 reset pass (**3 including setup/cleanup, 889 ms**). The inspected
screenshot is `indirect-selection-ui1/replay-debug/bf1-indirect-action.png`.
Logs are `indirect-selection-mapping1.txt`, `indirect-selection-options1.txt`
and `indirect-selection-ui1.txt`. This turn ran two targeted inventory loads
and one debug query, with no new native recapture or broad regression.
Changing indirect arguments across replay phases, actual multi-action GPU
validation and full shader/replay fidelity remain unfinished.

### Generated-action navigation and VS input request transport

Pixel and Resource History rows now retain the generated-action ordinal when
locating an execution. An indirect Draw's pixel modification can prepare PS
debugging with its pixel, sample, primitive and child ordinal. Replay Mesh offers
the same optional **Indirect action** selector and forwards it when opening VS
debugging at a vertex's first reference. Navigation prepares requests without
launching GPU work. Returning to an ordinary history row clears the previous
child selection. Native execution selection remains the parent ExecuteIndirect;
this does not implement native stopping inside one of its generated actions.

The mapping records generated Draw/Dispatch/MeshDispatch kinds. Each uniquely
mapped child can be selected even when its multi-action parent cannot be selected
automatically. A shared validator checks ordinal/count, parent API, and agreement
with the replay event array. History, mesh and debugger views reject inconsistent
child provenance; changed selection invalidates pending results. Older direct
reports without child metadata remain accepted. Actual multi-action GPU behavior
remains unverified: the inspected BF1/Hellblade2 inventories contain only
single-action expansions, and the multi-action navigation checks use explicit
synthetic metadata rather than claiming GPU evidence.

This work also exposed a real geometry transport gap: Replay Mesh constructed
`stage: VSIn` and `attribute`, but `prepareRdcJob` rejected both. Job preparation
now accepts unsigned input attributes specifically for DX12 postmesh requests;
CLI exposes `--stage VSIn --attribute N`. DX11 input-stage requests and attributes
on other stages remain rejected.

Builds **297–298** pass. One actual CLI/isolated-worker query,
`bf1-input-cli1`, reads execution **36560**, input attribute **1**, instance **0**:
**3,316 vertices**, **4,974 indices**, **1,658 candidate faces**. The raw selected
vertex bytes (**265,208**) have SHA-256
`f351e596c83ed3ef92052aa24821019f401d806775e9828c4c271df791f9170f`; the index bytes
(**9,948**) have SHA-256
`59dd2e1242c7a945d00e5d6ee0bad70f934e5b49254e7b94b5a436c146d833db`.
Both match the earlier independent input read. Its module list contains neither
GPA nor Python. This verifies the request path and selected input bytes, not
whole-frame fidelity or a complete portable installation.

Focused mapping/options checks pass (**3 / 4 including setup/cleanup**).
History navigation passes (**3, 54 ms**), mesh selection and MainWindow navigation
pass (**4, 1,267 ms**), and debugger provenance checks pass (**3, 1,017 ms**).
The MainWindow test loads the fresh VSIn report, verifies direct vertex navigation,
then injects child identities to check all three navigation paths and clearing,
without starting a replay. An initial test assertion incorrectly requested a
complete CS job while a Draw was selected; the unavailable CS context correctly
rejected it, and the assertion now inspects the propagated field instead.
Evidence under ignored `artifacts/dx12-research` is
`bf1-input-cli-evidence1.json`, `indirect-navigation-mapping1.txt`,
`indirect-navigation-options1.txt`, `indirect-navigation-history-ui1.txt`,
`indirect-navigation-mesh-ui2.txt` and `indirect-navigation-debug-ui1.txt`.
No new native recapture, full-frame replay or broad regression was run.

### Opt-in DXIL fast-contraction experiment

The MainPS discrepancy now has an executable, opt-in model in the pinned
RenderDoc backend. `--dxil-fast-contraction` and the VS/PS/CS workspace checkbox
**Fast DXIL contraction (experimental)** enable GPU multiply-add for adjacent
float32 FMul/FAdd instructions in one basic block, only when both have fast flags
and the multiply has one semantic use. Precise, shared-product, non-adjacent,
cross-block and other-type chains remain on the existing path. This follows
the transformations permitted by [DXIL precise/fast semantics](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst#precise-qualifier),
but does not observe the driver's actual contraction choices. Default debugging
is unchanged. Intermediate FMul steps retain uncontracted IR values; eligible
FAdd steps consume saved multiply operands through the GPU helper. Results stay
partial and disclose this distinction even when their final output matches.

One actual Hellblade2 query (`hellblade-main-contract1`) at execution **37003**,
pixel **(541,451)**, primitive **0**, completes **317** steps with **24** queued
GPU contractions and no queue failures across the simulated quad. The selected
target is unchanged and the history boundary matches. Final target-0 bits are
**[1026307110, 1028810213, 1034362365, 0]**, exactly equal to the GPU fragment
reference. Previously all three RGB components differed. The dither angle changes
from bits **1129100242** to **1129100243**, matching the earlier fast GPU
reproduction; the preceding IR multiply remains unchanged. **22** final variable
values differ from the previous trace. This does not establish other invocations,
intermediate machine values, whole-frame stability or general DXIL fidelity.

The focused `tests/fixtures/dx12-fast-contraction.hlsl` reproduction snapshots
four constants before its stores, preserving distinct fast/precise/shared chains
under DXC `-T cs_6_6 -E main`. One native micro-capture and one CS debug query
produce exactly **one** contraction. Fast angle bits **1129100243**, precise
angle bits **1129100242**, and the shared-product sum **1123188759** all match
GPU readback. The precise and shared products remain separate. Neither this
debug query nor the Hellblade2 query loads GPA or Python.

The new seventh patch is `dx12-dxil-fast-contraction.patch`; the build script
verifies the complete patched tree and package checks include its version/hash.
The built DLL SHA-256 is
`be0f45accedf9f666ba3835eb7085a2fa443407230e56dbe7a820bdfe33e961d`.
Builds **299–302** and the pinned runtime build pass. Focused option checks pass
(**3 including setup/cleanup, 11 ms**); UI checks cover explicit enabling,
mode-mismatch and stale-result rejection, visible scope, and session/DX11 reset
(**3 including setup/cleanup, 435 ms**). Non-DXIL shaders reject the experiment.
The inspected screenshot is `dxil-contract-ui1/replay-debug/hellblade-contraction.png`.
Other ignored evidence is `hellblade-contract-evidence1.json`,
`dxil-contract-probe-gpu1.json`, `dxil-contract-probe-debug1`,
`dxil-contract-options1.txt` and `dxil-contract-ui2.txt`. No additional full native
sample recapture or broad regression was run. The default MainPS mismatch and
full shader numerical fidelity remain unfinished; this experiment is not a
replacement for those acceptance requirements.

### Device keyframe finalization and historical queries

The additional read-only Ghidra audit uses the same GPA **2025.1.3.1269**
`Streams/layers/capture-x64.dll`, SHA-256
`a1afd0b41e6bf668381e4898b03460130e0b61432c88d56c8559aef5b1b72c62`.
RIP-relative diagnostic-string references and PE unwind ranges locate
`ID3D12DeviceApiCallCache::EnumerateKeyframeCalls` at RVA **0x83480** and
`PostVisit` at **0x840d0** (image base **0x180000000**). The former includes
device/interface reconstruction; its callees have not all been audited.

The inspected gated `PostVisit` path retrieves device state, creates an
`ID3D12Fence` with initial value **0** and flags **0** (call at **0x84198**),
enumerates queue handles through **0x85a930**, then schedules event waits.
The loop calls Fence `SetEventOnCompletion` at **0x842b9** and queue `Signal`
at **0x842d0**; the shared target starts at **0** and increments at **0x84326**.
Its callback at **0x845c0** waits indefinitely, closes the event and releases
the retained Fence. The inspected routine/callback contain no query-result
Map/read/serialization path. This is not evidence that every queue was drained,
that the path was active when Talos was captured, or that every possible GPA
query-restoration path has been excluded. In particular it supplies no missing
query values and does not justify bypassing the existing MissingData guard.

Ignored evidence is `query-device-refs1.json`, `query-device-cache2.txt`,
`query-device-postvisit1.asm`, `query-device-assembly1.txt` and
`query-device-wait-callback1.txt`. No Talos replay was repeated for this audit.

### Explicit replay counter catalog and selection

The isolated adapter now exposes `rdc-analyze <capture.rdc> counter-catalog`.
It enumerates the active replay device's descriptions without `FetchCounters`
or an additional `SetFrameEvent`; capture loading itself may execute GPU work.
Reports distinguish **available_not_sampled** metrics from diagnostic entries
or unsupported value formats, include source and unit, and explicitly report
`measurement_status: not_requested`. A successful catalog is not a successful
measurement.

`rdc-analyze <capture.rdc> counters --counter <id>` accepts repeated explicit
IDs, including enumerated vendor metrics. Omitting IDs preserves built-in-only
selection. Unknown IDs, duplicates, diagnostic entries and unsupported formats
are rejected. Fetch results must contain each requested counter and cannot
contain duplicate event/counter pairs or unrequested counters. Existing fatal
error/empty-result checks remain. DX12 integer measurements use decimal strings;
legacy DX11 integer storage is unchanged. Reports disclose that collection covers
**capture actions**, potentially over multiple passes: an event selector does
not constrain the backend's sampling range.

Replay Metrics retains native DX12 queries as its default and adds a RenderDoc
mode with **List counters**, a comma-separated ID field and double-click selection
from the catalog. The table exposes IDs, source, unit and availability status.
The mode uses the existing isolated recapture/cache/identity path; a workspace
recapture covers the selected execution prefix. Catalog-only results cannot be
accepted as measurements, changed selections reject stale results, and switching
backends/captures clears the selected counter set. Missing SDK diagnostics cannot
be selected by double-click. Full hardware counter acceptance remains open.

One actual BF1 catalog query against the existing independent capture
(`bf1-counter-catalog1`) reports **15** built-in metrics and **one** NVIDIA
diagnostic: **ERROR: Could not find Nsight Perf SDK library**, ID **3000000**.
The diagnostic is non-measurable. The current runtime's NVIDIA path looks for
`plugins/nv/nvperf_grfx_host.dll`; no such library was found in the inspected
RenderDoc, NVIDIA or local tools directories. NVIDIA's
[official SDK entry point](https://developer.nvidia.com/nsight-perf-sdk)
redirected its download/get-started flow to account login during this audit.
No SDK was downloaded and no system developer-mode setting was changed. These
are environment/dependency gaps, not established hardware limitations; neither
vendor measurements nor original Intel-specific metrics are marked restored.

Builds **303, 305–307** pass after fixing a compile-time name shadow and the
toolbar source-selector visibility found during screenshot inspection. Focused
counter conversion/selection tests pass (**6 including setup/cleanup, 1 ms**),
request validation passes (**3, 9 ms**), and catalog selection, stale-result
rejection, DX11 values and recorded native DX12 metrics pass (**5, 132 ms**).
The UI uses the actual BF1 catalog; its synthetic measured row tests display and
identity rejection only. `counter-diagnostic-rejection1` uses the small existing
DXIL reproduction capture and confirms explicit ID **3000000** fails before
counter sampling with the SDK diagnostic. BF1 catalog loading and this rejection
load neither GPA nor Python. No native sample recapture or broad regression was
run. Ignored evidence includes `counter-catalog-evidence1.json`,
`counter-selection1.txt`, `counter-options1.txt`, `counter-catalog-ui2.txt`, and
the inspected `counter-catalog-ui2/rdc-counters/dx12-counter-catalog.png`.

### Hellblade2 compute inputs and invocation instability

A bounded temporary probe of execution **36779**, Group **[0,0,0]**, Thread
**[0,0,0]** reads the five reflected CS SRV views before debugging, after trace
creation and after instruction stepping. It covers buffer creation **571506**
(`TileListsBuffer`, **122,400 bytes**), color texture **531690** (two mips,
**9,216,000 / 2,304,000 bytes**), flat velocity **573318** (**9,216,000 bytes**),
velocity tiles **555318** (**48,960 bytes**) and the single-texel translucency
texture **510486** (**4 bytes**). These are observation points, not captured
values of every simulated load. The temporary code and raw bytes remain outside
Git; production no longer includes that texture probe.

`hellblade-compute-input-probe1` produces **588** steps. Buffer word **18360**
starts with tile **0x250034**, coordinate **(52,37)**, both before debugging and
after trace creation. After stepping it is **0x300030**, coordinate **(48,48)**.
The trace's actual `_102` is **3145776** (`0x300030`), and its `_130/_131` are
**768/768**; the downsample store coordinates `_658/_659` are **384/384**.
The earlier trace used **832/592** and **416/296**. Thus the same selected Group
and Thread do not identify a stable shader-computed output location across these
replays. The separately requested GPU texel **(416,296)** cannot be assumed to
belong to this trace's invocation.

The indexed **1,174-entry** window starting at word 18360 is not merely an equal
multiset in a different order at these observation points; later actual indirect
counts were not read, so this is not a claim about each later complete work list.
Across the entire buffer, **6,518** words change before-to-trace and **5,865**
trace-to-steps. The color input changes **342,028 / 12,503** words in mip 0 and
**85,930 / 2,958** in mip 1. Flat velocity and velocity tiles change **1,372**
and **302** words before-to-trace, then remain unchanged. Translucency's four
bytes remain unchanged. This establishes input variation before judging the
WaveReadLaneAt-derived output; it does not establish a wave-instruction bug,
missing barrier, or the initial source of the variation.

The pinned RenderDoc source replays the prefix without the selected Dispatch in
`D3D12Replay::DebugThread`, lazily prepares resources in
`D3D12APIWrapper::PrepareReplayForResources`, and restores the Dispatch through
`ResetReplay`. `ContinueDebug` also calls that restoration after a returned
batch. A follow-up actual run with the production buffer-only check reports
**one batch of 332 steps**. Consequently inter-batch replay is **not** established
as this case's cause; no speculative runtime replay-freezing patch was applied.

DX12 CS reports now include `compute_buffer_inputs`: the potential read-only
buffer bindings and byte-range SHA-256 snapshots at the three points. Each
point is bounded to **16 MiB / 256 views**; malformed ranges, incomplete reads
and budget exclusions remain explicit. Offsets, sizes and resource IDs retain
exact 64-bit text. Changed or incomplete observations mark the trace partial
and display a warning in the existing debugger. Textures, UAVs and constant
buffers are outside this production check, and unchanged snapshots do not prove
complete or continuous input stability. `debug_batch_sizes` records actual
nonempty ContinueDebug return sizes instead of inferring batch boundaries.

The production query `hellblade-compute-buffer-stability1` reads only the
**122,400-byte** buffer at each point. Its SHA-256 values begin
**f52eaf8b**, **d4e999a6**, **8d3dda40**; all differ, and the returned trace and
workspace warning explicitly remain partial. Both queries use the existing
independent RDC; no native full-sample recapture or GPA process was needed.
The original capture is unchanged. Ignored evidence includes
`compute-input-probe1.patch`, `hellblade-compute-input-evidence1.json`,
`hellblade-compute-input-probe1`, and `hellblade-compute-buffer-stability1`.

Builds **311–313** pass. The bounded-range test verifies byte changes, exact
identities/offsets above 2^53, short reads and budget rejection before reading
(**3 passed including setup/cleanup, 1 ms**). The workspace consumes the actual
new report, shows the instability warning, and rejects it after experiment
context changes (**3 passed, 269 ms**). Its inspected screenshot is
`compute-input-ui1/replay-debug/compute-input-stability.png`; logs are
`compute-input-snapshot1.txt` and `compute-input-ui1.txt`. The production query
loads neither GPA nor Python. Full compute numerical fidelity and stable
invocation-to-output correspondence remain unfinished.

### Retain DXIL compute inputs through a debug trace

**Historical version-1 evidence, superseded below:** the later UAV read/write
fixture found that ReplayController re-executed the event after DebugThread.
The buffer hashes and numeric observations in this section remain recorded
facts, but the claims that those observations were pre-dispatch are invalid.
Version 2 fixes that additional controller boundary; old reports are visibly
marked obsolete and the current worker disables version-1 retention.

Resource history locates the Hellblade2 tile-list writer at execution **36748**
(record **611505**) and the selected consumer at **36779** (record **611544**),
both in submission **613234**. Starting analysis capture at that submission
would still replay the writer, so it is not an input-stability fix.

The eighth pinned RenderDoc patch, `dx12-dxil-retained-compute.patch`, adds an
explicit API enabled only by Flora's isolated `debug-thread` worker. After
DXIL DebugThread's pre-dispatch replay and scratch-resource lane probe, it
retains the application resource state through constant-buffer loading and
all ContinueDebug calls. ResetReplay is deferred until FreeTrace destroys the
wrapper, which executes only the original dispatch to restore post-event state.
The worker checks that exactly one retained trace was activated. An unpatched
1.45 library keeps the previous behavior and input-instability diagnostics;
DXBC and VS/PS paths do not activate this mode. No native command-list split,
extra application barrier, shader rewrite or work-list reordering is introduced.

Reports distinguish the earlier post-event `before_debug` observation from
the retained pre-dispatch observations after trace creation and stepping.
Only the latter pair is used for retained-buffer stability. The worker does
not read or label an intermediate texel as post-event while the trace is alive;
it releases the trace before reading the restored dispatch output. Workspace
text explains this scope, and earlier-reference mismatch warnings remain.
The bounded buffer observations still exclude textures, UAVs and CBVs; they
are not continuous validation or proof of every interpreter load.

One actual Hellblade2 query (`hellblade-retained-compute1`) returns **332**
steps in one batch. Tile-list buffer **661**, **122,400 bytes**, has identical
SHA-256 **74e83198c2f0c16f504b8fc9953a791af641f24a16815e46e07342a204567a3f**
after trace creation, stepping, and dispatch restoration. The earlier replay's
hash starts **8c8eee41** and is intentionally excluded from that comparison.
The simulated tile entry is **2424884**, full-resolution coordinate **(832,592)**,
and mip-output store coordinate **(416,296)**. Store operands
**(0.043701171875, 0.0369873046875, 0.0390625)**, with positive R11G11B10
mantissa truncation, match the restored GPU RGB
**(0.04345703125, 0.03662109375, 0.0390625)**. The earlier green value was
**0.037109375**, so the report correctly remains partial. This verifies one
selected store against the restored phase; it does not validate all UAV stores,
multiple debug batches, cross-query stability or full-frame fidelity.

The existing tiny fast-contraction fixture checks the DXIL CS path without
wave probing: **24** steps exactly equal its previously GPU-checked trace,
with one contraction and no failures. Both queries load no GPA/Python modules.
No new full native capture or whole-sample replay was run. Ignored evidence:
`hellblade-tilelist-history1`, `hellblade-retained-compute1`,
`hellblade-retained-compute-evidence1.json`, `retained-compute-simple1`, and
`retained-compute-ui2/replay-debug/retained-compute.png`.

The rebuilt eight-patch runtime SHA-256 is
**c18c724b8d794ea7b9a46332b52b37d58afb2e62d6562ff7bbd62b3a7c182d1e**.
Build/package manifests verify the additional patch and exact source tree.
Worker/application builds **315–317** pass (314 used an invalid target name).
The focused UI test consumes the actual new report, checks phase separation,
visible notices and stale experiment rejection: **3 passed**, including
setup/cleanup. Complete portable-package acceptance remains outstanding.

### DXIL UAV texture addressing and corrected retained-input phase

Tracing actual texture writes exposed two independent RenderDoc 1.45 defects:
DXIL TextureLoad/TextureStore and texture atomics narrow coordinates through
`int8_t`; D3D12 FetchUAV always reads subresource zero and derives pitches from
the full resource. A new standalone GPU fixture reproduces them on a 512-wide
R32_UINT texture with a view of mip **1**, starting at layer/depth slice **1**.
The fixture's first read is at x **200**, followed by a store, read, atomic add,
read and out-of-range/sentinel checks. A second valid read at x **128** checks
the signed-byte boundary. Out-of-range x **256** must not alias the next row;
negative coordinates must not wrap into the view.

The ninth patch, `dx12-dxil-uav-texture.patch`, preserves 32-bit coordinates,
uses 64-bit byte pitches/offset products, and checks each coordinate axis and
the remaining byte span. Cached texture data now contains the selected mip
and only the UAV's array or W slices, including the 1D-array y-coordinate
convention. The same address validation covers texture atomics. Unsupported
nonzero planes, invalid view ranges and incomplete readback increment a failure
counter; the worker rejects the trace rather than accepting missing-data zeros.
Unpatched DXIL shaders declaring UAV textures receive a visible warning.
Buffer-address handling and DXBC texture-view loading are not changed here.

The first repaired-view reproduction uncovered a flaw in input-retention
version **1**: `ReplayController::DebugThread` called `SetFrameEvent` after the
driver created the trace, so the first UAV load observed **784** (post-dispatch)
instead of **21300** (pre-dispatch). Read-only buffer hashes could not distinguish
these phases. Version **2** registers the exact retained trace through an internal,
thread-local hook and skips that controller replay only for that successful trace.
It does not change the public replay ABI. FreeTrace restores the dispatch as
before. The current worker disables version-1 retention, preserving unrelated
analysis and DX11 jobs; workspace loading of an old report
warns that its input-phase claims are invalid.

The three targeted fixtures each return **34** debug states. Every selected
store operand matches the eight GPU result words:

| View | GPU and corrected debugger words |
| --- | --- |
| 1D array, mip 1, layers 1..2 | 21200, 777, 777, 784, 21000, 0, 0, 21128 |
| 2D array, mip 1, layers 1..2 | 21300, 777, 777, 784, 21100, 0, 0, 21228 |
| 3D, mip 1, W slices 1..2 | 21300, 777, 777, 784, 21100, 0, 0, 21228 |

The pre-fix 2D debugger returned eight zeros. With the view fix but retention
version 1 it returned the wrong first word, 784; the final fixture comparisons
require retention version 2 and verify all eight words. `FloraRdcDebugTests
dxilUavTextureViews` consumes these real GPU/trace files and passes (**3 including
setup/cleanup, 25 ms**). Source is in `tests/fixtures/dx12-dxil-uav-view.cpp` and
`.hlsl`. The standalone fixture is deliberately not an automatic GPU CTest.
Compile its C++ file in a VS developer shell with `/std:c++20 /EHsc`, include
`third_party/renderdoc`, and link `d3d12.lib dxgi.lib`. Compile HLSL with DXC
`-T cs_6_0 -E main -Od`; define `VIEW_1D=1` or `VIEW_3D=1` for those variants.
Run the executable with shader path, RenderDoc DLL, capture prefix and
`1d|2d|3d`; it checks its GPU words before returning success. Then use
`rdc-analyze <capture> debug-thread --execution 1 --group 0 0 0 --thread 0 0 0`.

One actual Hellblade2 integration query (`hellblade-uav-texture1`) returns
**332** steps, reads **three** UAV texture views with no failures, and uses
retention version 2. Tile buffer 661's SHA-256 is
**266b3da37d835dce4f703af03d972df107b4b6f08770f263d18e2ed546d27fe9**
at trace creation, after stepping and after dispatch restoration. The selected
store remains at **(416,296)**; its RGB operands and restored GPU RGB match the
previously recorded positive R11G11B10 truncation comparison. The earlier replay
still differs, so the report remains partial. Automatic write-to-resource
navigation, all-store validation and whole-frame fidelity remain unfinished.

Worker/application/test builds **318–322** pass. The UI test uses the new
Hellblade2 report, verifies the current notice, checks obsolete-version handling
and stale experiment rejection (**3 passed, 343 ms**); its inspected screenshot
is `uav-texture-ui1/replay-debug/retained-compute.png`. The fixtures and real query
load no GPA/Python modules. GPU checks ran serially; no full native sample was
recaptured or added to a recurring test loop. Evidence directories are
`dxil-uav-view-before1`, `dxil-uav-view-after2d`, `dxil-uav-view-final1d`,
`dxil-uav-view-final2d`, `dxil-uav-view-final3d` and `hellblade-uav-texture1`.
GPU words are `dxil-uav-view-gpu1d.json`, `dxil-uav-view-gpu2d.json` and
`dxil-uav-view-gpu3d.json`; the CPU comparison log is `dxil-uav-view-check1.txt`.
These queries used runtime SHA-256
**e236df968e32ed08494f74beec93963a572cf4f2659d6d9d714505eb125a8c34**;
the final runtime additionally stops unbound atomic evaluation before using
missing format data, and has SHA-256
**2ad054e713857063638f59017c8ee3235f03fea0cb41bb2c23dc9531289c4636**.
That unbound branch was compile-checked, not GPU-validated. Build/package
manifests verify all nine patches; complete portable-package acceptance remains
outstanding.

### DXIL texture-store observations and navigation

The tenth pinned RenderDoc patch records executed `TextureStore` attempts for
the selected compute lane. A versioned, fixed-width export carries the exact
instruction and debug-step occurrence, resolved resource/view, coordinates,
mask and raw operand bits. Resource IDs and byte offsets remain decimal strings
in worker JSON. The worker rejects an inconsistent instruction occurrence or
more than 4,096 observations. Loop iterations remain distinct even when their
instruction index is identical. This is debugger observation, not GPU store
instrumentation; buffer stores, atomics, other lanes and `TextureStoreSample`
are outside its scope.

After freeing the retained trace and restoring the dispatch, the worker samples
up to 64 distinct valid texels at their resolved physical mip/layer. These GPU
values can include later stores, atomics and other threads. The result therefore
explicitly retains `validation: not_validated`; it does not promote operand
agreement to per-store or whole-shader correctness. Out-of-bounds attempts
retain their raw coordinates and receive no GPU texel query.

Hellblade2 execution **36779**, group/thread **(0,0,0)**, returns **332** states
without manually specifying a target resource or pixel:

| Instruction | Debug step / trace position | Resource creation | View coordinate | Observation |
| --- | --- | --- | --- | --- |
| 612 | 326 / 325 | 571530 | (832,592,0) | RGB operands and post-dispatch RGB agree |
| 614 | 328 / 327 | 570673 | (416,296,0) | RGB operands are (0.043701171875, 0.0369873046875, 0.0390625); GPU RGB is (0.04345703125, 0.03662109375, 0.0390625) |
| 616 | 330 / 329 | 562718 | Negative coordinates represented as unsigned bits | Out of bounds; no GPU read |

The second row agrees with the previously observed positive R11G11B10 storage
truncation for this selected value. The first two stores supply alpha zero,
whereas the RGB texture readback supplies alpha one. Neither observation
establishes general format conversion fidelity. An initial observer used a
block-level instruction field and incorrectly labeled all three stores 570;
the final patch derives the actual executing instruction and verifies it against
the trace before accepting the report.

The `Texture writes` workspace tab displays these observations and their scope.
Double-clicking an instruction selects its exact trace occurrence; double-clicking
a mapped resource selects the capture's creation record. Tooltips retain physical
subresource coordinates, raw bits and GPU-reference details. This resource link
does not yet select a native output subresource/pixel or trigger a replay. Busy
requests cannot navigate, and session/experiment changes clear the table and
reject stale results.

Focused evidence under `artifacts/dx12-research/`:

- `hellblade-texture-stores2`: final real query; no GPA/Python modules loaded.
- `dxil-texture-stores2d` and `dxil-texture-stores3d`: mip 1, physical slice 2,
  x=200 observations; store operand 777 and GPU value 784 correctly remain
  distinct because a later atomic adds seven. Two out-of-bounds attempts are
  not sampled.
- `dxil-texture-stores-loop1`: the fixture compiled with `STORE_REPEAT=1`
  executes one store instruction twice, with operands 777 and 778. Both rows
  sample the final 778 but have distinct debug steps. Five total attempts are
  retained. The standalone fixture checks its eight GPU result words.
- `texture-store-check2.txt`: coordinate/64-bit ABI and actual loop-occurrence
  checks, **4 passed including setup/cleanup, 8 ms**.
- `texture-store-ui2.txt`: actual Hellblade2 instruction/resource navigation,
  busy-state guard and stale-experiment rejection, **3 passed, 272 ms**.
  `texture-store-ui2/replay-debug/texture-stores.png` was visually inspected.

Final worker, UI and application builds pass (325–329). Build 324 failed because
two builds overlapped on `Backend.obj`; the subsequent serialized build completed.
The final analysis DLL SHA-256 is
**4d4186475becf40704a3a0d130225a6154c4327f0d78d010a0e510045391c8ae**.
Build/package manifests now validate all ten patches. GPU queries ran serially
against existing sample RDCs plus the small loop fixture, without another full
native sample replay. Whole-frame fidelity, complete store validation and full
portable-package acceptance remain unfinished.

### DXIL R11G11B10 store conversion

Following the actual Hellblade2 stores exposed a separate debugger defect:
the D3D12 DXIL typed-store helper converted float32 operands to float16 first,
then discarded mantissa bits to pack R11G11B10. Intermediate rounding could
raise a value to the next representable level, turn finite overflow into
infinity, or change a subnormal boundary. A subsequent load in the same shader
therefore read the wrong cached value. The original capture and native GPU
execution are unaffected by this debugger defect.

The eleventh pinned patch converts the float32 bits directly to the unsigned
11/10-bit representation. It truncates toward zero, saturates finite overflow,
preserves representable subnormals, clamps negative values to zero, preserves
positive infinity and retains NaN classification. These rules follow Microsoft's
[floating-point conversion specification, section 3.2.2](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
and its unsigned float format definition in section 3.1.6. NaN payloads are not
claimed to match the GPU. The shared upstream conversion utility, DXBC and other
typed formats are unchanged; their complete conversion fidelity is not proven.

The new HLSL fixture `tests/fixtures/dx12-dxil-uav-packed.hlsl` uses the existing
UAV-view harness with `2d r11g11b10`, a mip-1/slice-2 UAV, one compute thread and
explicit device-memory barriers between stores and loads. It exports 32 raw
float32 words. Compile with DXC `-T cs_6_0 -E main -Od`; compile the harness as
documented above, then supply shader, DLL, capture prefix and those two arguments.
Format load/store support is queried before resource creation. Packed-mode
success means capture/readback succeeded; the evidence comparison separately
checks the debugger against those GPU words.

The actual Hellblade2 RGB operands form the first case. Additional values
exercise the identified rounding boundary, finite overflow, negative values,
infinities, subnormals, smallest normal, NaNs and zero:

| Input / condition | Before fix | GPU and corrected debugger |
| --- | --- | --- |
| R float32 bits `3f81ffff` | 1.015625 | 1.0 |
| G float32 bits `3fffffff` | 2.0 | 1.984375 |
| B float32 bits `3f83ffff` | 1.03125 | 1.0 |
| Finite overflow, R/G | Infinity | 65024 |
| Finite overflow, B | Infinity | 64512 |
| G float32 bits `35ffffff` | 0.0000019073486328125 | 0.00000095367431640625 |
| G float32 bits `387fffff` | 0.00006103515625 | 0.00006008148193359375 |
| B float32 bits `35ffffff` | 0.0000019073486328125 | 0 |

Both traces contain **133** states. Before the fix, **9** numeric components
differed. Afterward, all **29 non-NaN words match exactly**, and all **3 NaNs**
match classification. The patch counter records eight cached packed stores.
Evidence is `dxil-uav-packed2_capture.rdc`, `dxil-uav-packed-gpu2.json`,
`dxil-uav-packed-before2` and `dxil-uav-packed-after1` under the ignored research
directory. The initial two-store diagnostic is retained as `dxil-uav-packed1`
and `dxil-uav-packed-before1`; it established three mismatches before expanding
only the conversion boundary cases.

A separate query with the existing older `renderdoc-before-dxil-contract.dll`
verifies that the worker detects a bound R11G11B10 UAV and marks the result
partial with a conversion warning (`dxil-uav-packed-unpatched1`). This check
does not use the old trace as conversion evidence, since that library also
lacks subsequent view and input-retention fixes.

Hellblade2 execution **36779**, group/thread zero, remains **332** states with
the same selected instructions **612/614/616** and captured resource mappings.
The new conversion path executes **40** times across all simulated lanes; this
is not a selected-lane write count. Three UAV views are read with no failures.
The selected RGB operands and restored GPU values retain the observations above.
`hellblade-packed-store1` loads no GPA or Python modules. This verifies integration,
not full shader equivalence, input stability or full-frame fidelity. GPU queries
ran serially against existing RDCs plus the small conversion reproduction.

The final DLL SHA-256 is
**c6b369ee674037f84a4190f93df0b3adfdd7b66bd9539b7013961c642f7c3e80**.
Worker/CLI and focused-test builds **330–331** pass. `FloraRdcDebugTests
dxilPackedTextureStores` checks the actual before/after reports, all GPU words
and the older-library warning (**3 passed including setup/cleanup, 62 ms**;
`packed-store-check2.txt`). Set `FLORA_DXIL_PACKED_STORE_REPORTS` to the research
directory to run that evidence check. No UI code changed in this correction.
The isolated worker reports patch version and store count; package/build
manifests verify all eleven patches. Full portable-package acceptance remains
outstanding.

### Texture-write coordinates to native output

Double-clicking the coordinates in the CS `Texture writes` table now selects
the capture resource and physical mip/array layer for native replay. It retains
the current execution and Before/After boundary, opens the resource/output
workspace and prompts for Replay. Resource-name double-click still selects the
creation record; instruction double-click still selects the exact debug occurrence.
The source trace remains available while selecting the texture.

The navigation request carries the session/experiment analysis context, execution,
resource creation and captured object without floating-point identity conversion.
The workspace rejects stale contexts or a mismatched object/lifetime. It resolves
the D3D12 subresource from the captured description, including a zero `MipLevels`
full chain, and rejects invalid mip/layer/sample/coordinates before changing the
selection. Current display support covers single-sample 1D/2D resources on plane
zero; 3D slice display remains explicitly unsupported. Out-of-bounds or unmapped
debug stores cannot generate a pixel navigation request.

The pending pixel is tied to the selected execution/resource/subresource and
boundary. Native readback still passes the existing request identity, resource
lifetime and subresource checks; the pixel target additionally requires matching
mip/layer, plane, samples and dimensions before the marker is drawn. Changing the
event, resource, subresource, budget, boundary or experiment clears the pending
target and old output. The Pixel History coordinates are prepared for the selected
resource; navigation does not automatically run a history query or claim that a
compute store has a verified Pixel History entry.

Targeted verification, builds **332–333**:

- `texturePixelSubresource`: nonzero mip/layer, implicit full mip chain, 1D
  coordinates and invalid/unsupported cases; **3 passed including setup/cleanup,
  1 ms**, `texture-pixel-mapping1.txt`.
- `textureStoreNavigation`: actual Hellblade2 report emits `(416,296)` with the
  correct context, execution and resource, while the out-of-bounds row emits an
  error; existing instruction/resource and stale-result checks also pass;
  **3 passed, 184 ms**, `texture-pixel-ui1.txt`.
- `textureStorePixelWorkspace`: actual Hellblade2 capture, execution **36779**,
  creation **570673**, mip/layer **0/0**, subresource **0**. Before selection is
  preserved without replay; then one After replay with a **12 GiB** allocation
  budget produces the **960×600** native image and marker **(416,296)**. The
  history request retains the correct creation and coordinates. A stale-context
  navigation is ignored; changing subresource clears image and marker.
  **3 passed, 46,993 ms**, `texture-pixel-workspace1.txt`.

The inspected screenshot is
`artifacts/dx12-research/texture-pixel-workspace1.png`. Its lower image area has
unexplained abnormal colors at this intermediate boundary. This navigation test
does not establish whether that area was written/defined at the selected event,
nor whether it matches original GPA; retain it for replay-fidelity investigation.
The test proves routing and readback provenance, not complete image correctness.
Only one native GPU replay was used for this workspace change. Full 3D navigation,
whole-frame fidelity and the remaining acceptance items below are unfinished.

### Discard ranges and the Hellblade2 intermediate image

The coordinate-navigation screenshot's abnormal lower area prompted a targeted
definition-of-contents investigation. Existing trace evidence distinguishes the
allocation from the shader's active rectangle: creation **570673** is **960×600**,
whereas `_RootShaderParameters` register **c2** is `(0,0,1920,803)` and c3 records
the same active full-resolution dimensions. In the actual DXIL, `_642` checks
the full-resolution coordinate against that maximum; `_649/_650` require even
coordinates, and `_645/_646` shift them right by one for mip-1 output instruction
**614**. Thus this Dispatch can write only within **[0,960) × [0,402)**, despite
the 600-row allocation. Its out-of-rectangle coordinates become `-1` and the UAV
store is out of bounds. This is a bound on writes, not proof that every pixel
inside it was written.

The existing output-producer RDC's resource-usage index identifies a more
fundamental definition boundary: execution **36710**, source **611455**, is
`DiscardResource` for captured object **2149346324304**, creation **570673**.
The original capture payload is **17 bytes** at `thread-34368-0` offset
**1706788**: command-list pointer, resource pointer and a null region flag.
An earlier whole-resource discard is execution **23039**, source **593584**,
at offset **842843**. Both decode fully without extension bytes. After 36710,
the usage index reports CS read/write bindings at **36766**, **36770**, **36775**
and **36779**, with source records **611527**, **611532**, **611539**, **611544**.
Binding evidence alone does not establish their texel coverage.

`DiscardResource` tells the implementation that old contents need not be
preserved; it is not a clear-to-zero operation. See Microsoft's
[DiscardResource semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-discardresource)
and [discard-region definition](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_discard_region).
The last Dispatch cannot define the padded rows, and earlier contents were
discarded. The other three Dispatches' coverage has not been proven, so these
findings narrow the investigation without proving the abnormal colors harmless
or attributing every difference to undefined data. Raw output remains unchanged;
no pixels are zeroed, cropped or excluded from fidelity acceptance automatically.

The implementation now decodes null/explicit discard regions, signed rectangles
and first/count subresource ranges with checked array lengths and complete-record
consumption. Discard resource references participate in existing API/resource
search. Resource-history enrichment uses the captured session, checks resource
lifetime and source-record identities, and attaches the exact discard range.
The panel displays that range and the discarded-content semantics, while keeping
`content_change: not_measured` and later bindings as potential uses. Existing
captured barrier details are retained. It does not yet track later per-texel
definition or infer a complete validity mask.

Evidence under the ignored research directory: `hellblade-mip1-writers1` is
the indexed-history query; `hellblade-mip1-discard-enriched1.json` contains the
captured-call enrichment; `hellblade-mip1-discard-evidence1.json` summarizes the
constant-buffer/guard bound and discard records. No additional native frame
replay was performed for this investigation. Builds **334–335** cover parser,
application and focused tests. The new parser fixture covers whole-resource,
rectangle and subresource-only regions, a resource identity above 2^53, missing
arrays and truncated records. The real-capture UI check verifies both discards,
preserved barrier enrichment, navigation to execution 36710 and rejection of
wrong resource/source identities. Whole-frame fidelity remains unfinished.
`discard-history-check2.txt` records **4 passed, 0 failed, 0 skipped** in **271 ms**
(including initialization and cleanup). The inspected
`hellblade-mip1-discard-ui2.png` shows the selected whole-resource discard and
its original command payload in the existing history panel.

### Indexed work and the remaining mip-1 producer failure

The four post-Discard binding rows now have exact action metadata from the
existing output-producer RDC. This metadata belongs to capture loading, not a
fresh measurement of every subsequent replay:

| Execution / source | RDC action | Indexed groups | Focused result |
| --- | --- | --- | --- |
| 36766 / 611527 | 90398 | 4902 × 1 × 1 | Group/thread zero produces a 412-state DXIL trace |
| 36770 / 611532 | 90412 | 0 × 1 × 1 | Debug request correctly rejects a zero-workgroup action |
| 36775 / 611539 | 90429 | 44 × 1 × 1 | Initially failed GPU lane probing; restored by single preparation below |
| 36779 / 611544 | 90443 | 1174 × 1 × 1 | Previously recorded trace remains the reference |

For 36766, the actual constants again give c2 `(0,0,1920,803)` and c22
`(12,1,0,0)`. Its disassembly computes `_346/_347` by clearing the low bit of
`_344/_345`; `_594` checks those coordinates against the viewport maximum.
The mip-1 coordinates `_601/_602` are arithmetic right shifts by one, selected
only when `_594` and OutputMip1 are true. Consequently in-bounds mip-1 writes
cannot reach rows 402–599. The selected invocation records four mip-0 writes,
one mip-1 write at **(0,384)** (instruction **564**, creation **570673**), and an
out-of-bounds disabled mip-2 write. This bounds the shader's output region; it
does not measure coverage of all 4902 groups or establish whole-image fidelity.

36775 failed twice with `No debugger trace at the selected invocation`. After
adding failure-log preservation, the reproduction records RenderDoc
`d3d12_shaderdebug.cpp:3876`, **“No hit for this event”**: the instrumented GPU
lane fetch returned zero hits. That is a different failure from the explicit
zero-workgroup rejection at 36770. Whether its indexed 44 groups differ from
replayed indirect arguments, or another probe/replay defect prevents the hit,
remains unproven. Do not replace the failure with an empty successful trace or
use it to conclude that this Dispatch writes no pixels.

Resource History now includes `indexed_action` for exact Draw/Dispatch action
IDs and an **Indexed work** column. Zero group dimensions are labeled as no
shader invocations **in the index**; nonzero dimensions still retain
`bound_use` / `content_change: not_measured`. Draw rows show element and instance
counts. Tooltips retain the source and indirect-replay limitation. Older reports
without these fields remain readable and get no invented work counts. Lookup
uses the existing bounded action-tree traversal, with no extra per-use replay.

Analysis failures preserve up to the last **1 MiB** of the backend log before
RenderDoc shutdown, with an explicit truncation notice when applicable. The
worker's failure result references `backend-diagnostic.log`; stderr and CLI
errors expose its job-local path. Failure to save diagnostics does not replace
the original exception. Forced process termination cannot use this unwind path.

Focused evidence: `hellblade-mip1-writer36766`, `hellblade-mip1-writer36770`,
`hellblade-mip1-writer36775`, `hellblade-mip1-writer36775-diagnostic1`,
`hellblade-mip1-work-index1`, and compact `hellblade-mip1-work-evidence1.json`.
Builds **336–339** passed. `hellblade-indexed-work-ui2.txt` records **4 passed,
0 failed, 0 skipped** in **188 ms**, including initialization/cleanup, the real
four-row metadata check, old-report compatibility and indirect navigation.
The first fixture incorrectly included earlier pre-Discard uses; its scope was
corrected before the passing run. `hellblade-indexed-work-ui2.png` was inspected.
The tiny invalid-RDC fixture `diagnostic-invalid1` verifies constructor-failure
log retention and CLI error-path reporting without opening a GPU capture.
The real 36775 failure verifies preservation during analysis unwinding.
All capture queries were serial; no additional native frame replay was run.

### Single preparation restores Hellblade2 execution 36775 debugging

The failure was localized to an extra prefix replay, rather than absent debug
information. All four indirect calls use captured buffer **2149344466640**,
creation **514808**, RDC resource **424**, signature **5491**, stride **12**,
maximum count **1**, no count buffer, at byte offsets **0, 12, 24, 36**.
The selected-event argument read at offset 24 is **(44,1,1)**, matching the index;
pipeline inspection also confirms no predication. A separate research DLL
logged the buffer immediately after DebugThread's `WithoutDraw` replay:
**(5292,1,1), (0,1,1), (0,1,1), (828,1,1)**. Thus 36775's lane probe really had
zero groups in that later replay. The research changes and DLL stay outside Git;
the production source was restored before applying the versioned fix.

The twelfth pinned patch, `dx12-dxil-prepared-compute.patch`, adds an explicit
local compute-debug preparation. It replays once up to the selected event,
fetches the pre-dispatch pipeline, and leaves application resources in that
state. DXIL DebugThread consumes a one-use token tied to the exact driver/event
instead of replaying the prefix again. Any other event selection invalidates the
token. The existing version-2 retention patch then preserves those inputs during
lane probing and stepping; FreeTrace runs the original dispatch to restore the
corresponding post-event state. Normal event selection and unrelated debug
paths retain their usual behavior. The worker requires exactly one consumed
preparation and reports the preparation version and phase. Older libraries use
the legacy path with a DXIL warning; they are not silently treated as equivalent.

Indirect compute debugging now saves a bounded `debug_indirect_inputs.json`
snapshot, including current and indexed dimensions, action ordinal, count,
signature, resource and byte offset. Its current decoder supports dispatch-only
signatures; other layouts report the gap. It reads 12 argument bytes and, when
present, four count bytes with checked bounds. Prepared out-of-range groups are
rejected before probing. Capture/resource/offset identities remain lossless.
Explicit GPU texel references are deferred until trace release, labeled
`single_post_dispatch_observation`; they no longer describe pre-dispatch data
as post-dispatch output or claim an unmeasured unchanged-value comparison.

`hellblade-mip1-writer36775-prepared1` now succeeds with **413 states**, one
consumed preparation, one retained trace, three UAV view reads and zero view
failures. Its read-only tile-list bytes match before debug, after trace creation,
after stepping and after dispatch restoration. The selected lane writes four
mip-0 texels at `(1408/1409,256/257)` and one mip-1 texel at **(704,128)**,
creation **570673**, instruction **706**; the disabled mip-2 attempt is out of
bounds. Mip-0 RGB operands match the sampled GPU values; mip-1 GPU values include
R11G11B10 storage conversion. Observations remain distinct from general proof
of every thread's writes. Constants c2 `(0,0,1920,803)`, c22 `(12,1,0,0)` and
guards `_733/_745` with shifted coordinates `_740/_741` also bound this shader's
in-bounds mip-1 writes to **[0,960) × [0,402)**. Together with the other two
examined nonzero writers, this narrows the padded-row investigation; it does
not establish complete resource history, aliasing behavior or frame fidelity.

Focused validation used the existing captures serially. The small 2D UAV view
fixture (`dxil-uav-view-prepared2d`, 34 states) matches all eight independently
recorded GPU words: **21300, 777, 777, 784, 21100, 0, 0, 21228**, retaining the
original pre-store value rather than a replayed post-store value. Its compact
comparison is `prepared-compute-fixture1.json`. The real zero-workgroup event
36770 still fails explicitly; `hellblade-mip1-writer36770-prepared1` saves
`(0,1,1)` at offset 12 and rejects group zero before GPU probing. The UI test
checks the 36775 trace, phase labeling and exact resource/pixel navigation:
**3 passed, 0 failed, 0 skipped**, **312 ms**, including initialization/cleanup,
in `prepared-compute-ui2.txt`. The screenshot was inspected. Builds **340–344**
passed, as did the exact twelve-patch RenderDoc source/build check.
The new runtime SHA-256 is
`b2fec618661edbcb464e392f6f1efd2d1a552211403a72ee71c76d4ad5f6d1a7`.
The successful Hellblade2 analysis loads no GPA/Python modules. Package scripts
now validate and ship the twelfth patch; full portable acceptance is unfinished.

The earlier prefix's changing tile classification is still unresolved. Preparing
one coherent state fixes this debugging failure without proving that the state
matches original GPA in every resource. No indirect counts are forced, no draw
is skipped to manufacture success, and full-frame fidelity remains incomplete.

### Pixel-history replay consistency and the upstream velocity variation

The next bounded investigation follows the changing motion-blur tile list back
through execution **36748** (tile classification) to **36670** (velocity
flattening, RenderDoc event **90084**). The latter dispatch reads scene depth
creation **563063** / object **2149346439360** / RDC **55**, scene velocity
creation **511789** / object **2152463529088** / RDC **383**, and previous
scattered subpixel depth creation **571700** / RDC **1247**. These identities
come from the actual pipeline report, not texture names alone.

In `hellblade-velocity-prefix-inputs1`, a temporary worker prepared that exact
pre-dispatch state twice with the prepared-compute export. It read only those
three bound SRV textures, with bounded readback, and did not execute the selected
dispatch or debug a shader between observations. Scene depth changes at **392**
pixels and scene velocity changes at the **identical 392 coordinates**, bounding
box **(0,206)–(1605,596)**, all within the active image. Stencil changes at
**4,289** pixels; packed padding does not change. Previous scattered depth is
byte-identical. At **(456,206)** the depth changes from
**0.0021878723055124283** to **0.0015414863592013717**, while velocity changes from
raw UNORM16 words **34608,30148,46932,9359** to all zero. This locates variation
before velocity flattening. It does not yet identify the responsible draw,
aliasing, synchronization, or driver behavior. Constants were not sampled in
this probe. Its temporary source and worker were restored before production
changes; the binary dumps and research patch remain outside Git.

The actual Pixel History query at this texel exposes a misleading omission:
`hellblade-scenevelocity-consistency1` returns only four records, at executions
**11551,12574,25301,26320**, all with zero velocity. The selected post-event
texel is nonzero before history, but zero after history and event restoration.
Thus these records cannot explain one consistent replay state. Resource usage
still identifies possible bindings; it is not promoted to observed writes.

The worker now records `history_scope.replay_consistency`, comparing the four
PickPixel component bit patterns before history and after the already-required
event restoration. It adds two selected-texel reads, no extra prefix replay.
Changed or unavailable observations mark the report partial; the panel explains
the mismatch while preserving records, exact event navigation, and export for
diagnosis. Equal observations say only that this selected texel was unchanged;
they do not validate intermediate fragments, all pixels, or complete history.
Old reports explicitly show that consistency was not measured. Depth targets
use an explicit depth type cast, since RenderDoc's default typeless R16 picker
otherwise interprets D16 bits as half-float. These converted observations are
not advertised as raw texture storage or an independent original-GPA match.

Focused validation reused existing captures serially. The Hellblade2 query
detects the actual nonzero-to-zero change and remains partial.
`talos-depth-consistency2` reads **49151 / 65535** at mip 0, layer 1,
**(1527,1240)** before and after the two-layer depth history, and its complete
history rows exactly match `talos-depth-passing-history2`. Both load no GPA or
Python modules. The UI check covers both real reports, export, navigation to
execution **26320**, old-report labeling and stale-session rejection:
**3 passed, 0 failed, 0 skipped**, **222 ms**, including initialization/cleanup,
in `history-consistency-ui1.txt`. Its screenshot was inspected. Builds
**346–348** passed. Full replay fidelity and the upstream missing velocity
contribution remain unresolved; this check detects the inconsistency rather
than repairing or concealing it.

### Narrowing the missing velocity contribution to indirect particle work

The same fault is now localized further without recapturing the frame.
`hellblade-velocity-before-lastdraws1` observes zero velocity both before and
after history at execution **35926**, before the final 72 velocity-target draws.
Six targeted raw texture boundaries then narrow pixel **(456,206)**: executions
**36169,36207,36222** produce zero, while **36328,36253,36226** produce the exact
UNORM16 words **34608,30148,46932,9359** seen in the earlier varying input.
Executions **36222** and **36226** are adjacent velocity-target operations.
The latter is source **608815**, submission **613105**, command list
**2149339136512**, generation **4**, ordinal **1**, queue **2131165365824**,
RenderDoc child event **88708**. Its indexed ExecuteIndirect action has six
indices and **18 instances**. These observations locate a changed contribution;
they do not prove all earlier writes are stable or identify its exact instance.

A bounded temporary worker compared the same selected Draw twice, saving its
bound buffers and argument buffer. `hellblade-particle-indirect36226-inputs1`
finds the 5,120-byte argument buffer creation **571657**, object
**2149338317744**, RDC **578**, at byte offset **200**. Signature RDC **5713**
has one argument and stride **20**; there is no count buffer. The five argument
words change from **(6,18,0,0,0)** to **(6,1,0,0,0)**. GPU particle float data
creation **549918**, object **2145650368480**, RDC **230**, changes **103 bytes**
of its **12,288 bytes**; the complete argument buffer changes **39 bytes**.
The other five bound buffers and the reported pipeline rows are identical.
The earlier direct particle draw **35953** instead has five identical buffers
(56 MiB total) in `hellblade-particle-prefix-inputs1`; its CPU particle buffer
RDC **53** is not the affected GPU particle buffer. Its two CPU updates at RDC
events **26146/71773** are complete 8 MiB Unmap payloads. Neither observation
establishes that all texture inputs or all upstream execution are stable.

`hellblade-gpuparticle-history1` maps the affected float buffer to an earlier
compute read at **20069** and a compute write binding at **33732** (source
**608027**, RDC **80984**, one workgroup), followed by VS uses **35228/36226**.
`hellblade-gpuparticle-producer33732-pipeline1` identifies InputFloat creation
**572516** / RDC **80**, InputInt **556514** / RDC **505**, output float **549918**
/ RDC **230**, output integer **535315** / RDC **506**, and RWInstanceCounts
**543675** / RDC **485**. The constants include current and previous Niagara
external parameters. These are the next upstream inputs to inspect. The float
buffer is placed; the lack of a per-resource Initial Contents chunk is **not**
evidence of missing initialization, since this RenderDoc version stores placed
buffer initial contents on the heap. Heap restoration, aliasing and the actual
simulation inputs still need verification before assigning a cause.

This research used only serial queries of the existing independent capture.
Temporary probes from builds **350–352** were removed and the production worker
restored; an initial research-only compile error in build **349** was corrected.
No indirect counts, simulation data or barriers were modified to force matching
output. Pixel History now reports the changing state, but the particle replay
variation and full-frame fidelity remain unfinished.

### Restoring modified placed heaps between analysis replays

The particle variation has a confirmed RenderDoc initial-state cause. In the
pinned 1.45 source, `D3D12ResourceManager::Prepare_InitialState` deliberately
stores placed-buffer contents on the heap rather than each buffer. During log
loading, `WrappedID3D12Device` builds `m_ModResources` from resource flags and
recorded writes. However, those entries do not include the owning heaps.
`Apply_InitialState` then treats the heap as read-only during active replay and
skips its initial copy, retaining GPU writes from the preceding replay.

The thirteenth patch, `dx12-placed-heap-restore.patch`, propagates each modified
placed resource to its owning heap after resource-usage classification. This
includes writes through a placed texture that can alias a placed buffer. It
preserves the existing optimization for heaps with no modified placed resource;
it does not synthesize initial contents, alter captured counts, or insert frame
barriers. The isolated worker reports
`backend_dx12_placed_heap_restore_patch`; build and package scripts check and
ship the version-1 patch and its hash. The new runtime SHA-256 is
`66b344ba61e2900a77bd672a2bf3bd10da69c135b810a8ac3bee19d953885aeb`.

The focused source fixture is
`tests/fixtures/dx12-placed-heap-restore.cpp` with its adjacent HLSL. Compile
the shader with DXC `-T cs_6_0 -E main -Zi -Qembed_debug`, and compile the C++
with MSVC C++20, `third_party/renderdoc` on the include path, `d3d12.lib` and
`dxgi.lib`. Pass the shader, RenderDoc DLL and capture prefix. It initializes
two non-overlapping buffers on a 128 KiB default heap **before** starting
capture. The captured dispatch adds 7 to a value initialized to 10 and reads
99 from the other placed buffer. Independent GPU readback is **(17,99)**.
The small capture and readback are in `placed-heap-restore1`.

First-time compute debugging alone passes with either runtime, so it is not
used as the regression proof. A bounded temporary worker selected and fully
replayed the same dispatch twice within one controller, reading both buffers
after each selection. `placed-heap-fixture-repeat-before1`, using the previous
twelve-patch DLL, changes the writable buffer from **(17,99)** to **(24,99)**.
`placed-heap-fixture-repeat-after1` stays **(17,99)** for both observations and
matches independent GPU output; the read-only buffer remains **99** in both
runtimes. This reproduces and distinguishes the actual repeated-replay fault.

The real sample confirms the effect at the previously failing boundary.
`hellblade-particle-indirect36226-restored1` reads **(6,18,0,0,0)** from the
argument buffer at offset **200** after both selections. All **seven** bound/
argument buffers are byte-identical between them, including particle float data
RDC **230** and indirect arguments RDC **578**. The corresponding old probe had
18 then 1 instance and changes in both buffers. `hellblade-scenevelocity-heap-restored1`
now retains the nonzero **(456,206)** velocity through Pixel History and event
restoration, adding a fifth history row for execution **36222**, action **0**,
RenderDoc event **88694**. Its final value matches the selected texel observation.
The two repeat probes and real analysis load no GPA/Python modules.

An additional boundary check exposes a **separate, unresolved selection issue**:
`hellblade-velocity-restored36222` still reads zero when directly selecting that
indirect child, while `hellblade-velocity-restored36223` reads the exact nonzero
UNORM16 words **34608,30148,46932,9359** at the following API event. Thus the
earlier boundary localization to 36226 cannot be treated as verified attribution;
the split event-before/OnlyDraw path needs investigation. The restored history
row and UI navigation are preserved as backend observations, not complete
validation of indirect event-after replay. No selection is silently shifted to
a neighboring API to conceal the discrepancy.

The focused UI check confirms the fifth row, nonzero value, unchanged-texel
label and exact reported execution/action navigation: **3 passed, 0 failed,
0 skipped**, **161 ms**, including initialization/cleanup, in
`placed-heap-history-ui1.txt`; its screenshot was inspected. Builds **353–355**
and **357** passed; build 356 hit an in-use worker executable and was rerun after
the analysis finished. Temporary probes were removed, the production source
was recompiled, and the thirteen-patch source/build check and package-script
syntax checks passed. `placed-heap-restoration-evidence1.json` records hashes,
raw comparisons and the remaining boundary discrepancy. Full portable and
whole-frame fidelity acceptance is still unfinished.

### Indirect command ordinal and selected-event boundary follow-up

The worker now locates a dispatch-only indirect argument with the mapped
generated command ordinal, cross-checking the synthetic structured command's
`CommandIndex` and `ArgumentIndex`. RenderDoc 1.45's D3D12 `drawIndex` is an
argument position within the signature, not a generated command ordinal. It is
retained only as `backend_draw_index` in invocation metadata; reports no longer
mislabel it as `generated_action_index`. Invalid or inconsistent mappings yield
an unavailable argument snapshot with a reason, never a guessed offset.

The existing heap fixture has an optional `--indirect` mode with
`dx12-indirect-command-index.hlsl`. Compile that shader as `cs_6_0` with debug
information, then pass the mode after the output prefix. Its two dispatches
use group counts **(1,1,1)** and **(2,1,1)**, stride **12**, and initial offset
**32**. Atomic additions give independent GPU words **(24,7)**. Selecting
execution **1**, indirect action **1**, group **(1,0,0)** reproduces the bug:
the previous worker reads offset **32**, claims action index **0**, and rejects
the valid group as outside the prepared dispatch. The corrected worker reads
offset **44**, confirms command index **1** and groups **(2,1,1)**, then returns
**5** debug steps for RenderDoc event **9**. The fixture and reports are in
`indirect-command-index1`; this verifies multiple-command argument selection,
not the unrelated Hellblade raster boundary. Build **360** passed after fixing
the missing namespace qualifier reported by build 359. No GPA/Python modules
were loaded by the successful analysis.

A bounded diagnostic of Hellblade execution **36222** rules out a skipped
synthetic child: partial replay resolves **88694** to that exact API event,
finds indirect parent **88693**, and issues one `ExecuteIndirect` at argument
offset **180**. The selected velocity texel is still zero. The disagreement
with following execution **36223** remains unresolved; state/resource behavior
across the prefix/OnlyDraw split needs further investigation. Diagnostics are
in `indirect-boundary-probe1`. Temporary logging changes were removed and the
production thirteen-patch DLL restored. This is not an event-after correctness
pass and does not change the acceptance table below.

`indirect-state-probe1` compares those two selections without shifting the
requested event. Both read indirect words **(6,380,0,0,0)** at offset **180**.
The six shared buffers (**54,120,244,578,2471,3168**) are byte-identical. The
following event changes VS root table **2** from descriptor offset **339779**
to **339784**, switching particle-float input **231** (capture creation
**575886**) to **230** (creation **549918**). The other reported pipeline-row
changes are the corresponding VS descriptor accesses; PSO, output and
depth/stencil rows agree. The first selection still reads zero velocity and
the following selection reads the previously recorded nonzero value. This
narrows the investigation but does not prove the actual GPU state used by the
split Draw. Buffer 231 was sampled only at the first selection, so its equality
across both boundaries has not been established. The temporary bounded probe
was removed and production code recompiled in build **362**. The existing
`computeInvocationBounds` check passed (**3 passed**, including setup/cleanup,
**1 ms**); no unrelated regression suite or repeated full-frame replay was run.

### Selected-event history boundary contradiction

The follow-up `indirect-state-probe2` reads both particle buffers and the full
depth/velocity textures before the indirect child (**88693**), after it
(**88694**, execution **36222**) and after the following API (**88698**,
execution **36223**). The first two boundaries have byte-identical depth and
velocity images. The following boundary differs at **53** pixels in each
image, within **[206,206,1052,483]**. At **(456,206)**, depth changes from
**0.0015414863592013717** to **0.0021878723055124283**, and velocity from zero
to the previously recorded UNORM16 values. A second observation of execution
36222 reproduces both entire images exactly.

Particle buffer **231** varies in raw byte order between these observations,
while the other seven sampled buffers agree. The selected VS constants
confirm float stride **448** and disabled sorted indices. All **380** particle
tuples, each containing **45** float-component bit patterns, are identical
after sorting the tuples; their canonical SHA-256 is
`9902a61a1ee8ec6aade07c96eddda897136a47a87c48270b056200ee859fdc8d`.
This is evidence of permutation, not missing/different particle attributes,
and does not explain the unchanged Draw-only image. Resource History locates
the preceding write binding at execution **33725**, source record **608018**,
RenderDoc event **80963**, a **(7,1,1)** Dispatch. It also records the earlier
draw use at execution **35152**. Full producer correctness remains unverified.

The targeted direct-Draw substitution in `indirect-direct-probe1` also returns
the unchanged velocity image. `indirect-validation-probe1` reads the actual
D3D12 information queue after the selected Draw, since the public replay
message API only returns loading-stage messages. It reports one address-alias
warning (**926**) for a stale, unused vertex-buffer view, with no Draw error.
The logged PSO/root bindings, heaps, IB/VB views, viewport/scissor, depth bias,
stencil, predication and render-pass state agree across the prefix, OnlyDraw
and continuous-prefix paths. This does not prove identical actual GPU state;
state application/resource behavior remains unresolved. No address, state or
Draw substitution from these diagnostics ships in production.

`hellblade-indirect36222-history1` demonstrates an additional analysis defect:
both selected-texel observations are zero, yet the final Pixel History record
for that exact selected event reports the nonzero color above. Reporting only
“selected texel unchanged” misses this contradiction. The worker now compares
the final backend modification at the selected event with both observations
for regular color formats, using the stored channel count. A difference marks
the report partial and preserves both values under
`history_scope.replay_consistency.history_boundary`. Depth, packed formats,
missing observations and histories without a final modification at the
selected event explicitly remain not compared. Exact backend component-bit
equality is used; a conversion difference is not presented as verified
equality. This check adds no GPU replay and cannot validate intermediate
fragments or complete history.

The workspace displays the boundary conflict while retaining all five rows,
the exact **36222 / action 0** navigation and JSON export. Production report
`hellblade-indirect36222-history2` detects the mismatch. The positive control
at execution **12574** / event **11753** reports matching zero color before
and after, without the new partial flag. The new conflict UI check and the
existing legacy heap-restoration report check pass: **4 passed**, **0 failed**,
**0 skipped**, including setup/cleanup, **306 ms**. The screenshot was
inspected. Builds **364–368** passed; build 363 was an unchanged build after a
failed probe-preparation command. Temporary source changes were removed,
production code recompiled, and the original thirteen-patch DLL hash restored.
The selected-event replay fault and the full acceptance requirements remain
unfinished.

### Selected D32S8 depth roundtrip experiment

Further targeted probes isolate the execution **36222** discrepancy to the
depth attachment path. An explicit queue wait does not change the result.
GPU pipeline statistics around that exact ExecuteIndirect report the same
**2,280 IA vertices, 760 IA primitives, 1,520 VS invocations and 386 clipped
primitives** for split and continuous replay, but **4 versus 299 PS
invocations**. The Draw executes; this is not evidence of a skipped command.

Copying the D32S8 depth contents to an identical temporary resource and binding
that resource restores the entire velocity image. Copying out while retaining
the original attachment does not. Copying out **and back into the original
resource before the Draw** restores both velocity and the original depth
image, preserving the Draw's depth writes. The latter probe reports **971 PS
invocations**; exact final images do not imply identical early-depth behavior
or performance. No specific driver/compression fault has been established.

The fourteenth pinned patch, `dx12-depth-roundtrip.patch`, exposes this as an
**experimental, default-off** texture-analysis option:

```powershell
FloraGPA.Cli rdc-analyze capture.rdc texture --execution 36222 `
  --resource 2152463529088 --resource-creation 511789 --depth-roundtrip `
  --renderdoc <fourteen-patch-renderdoc.dll> --out <new-output-directory>
```

It applies only to the explicitly resolved selected event's isolated Draw,
including an indirect child. Eligibility requires a depth-writing D32S8 2D
attachment, stencil disabled, one mip/layer/sample/node and legacy depth
states. Mesh draws, enhanced layouts and other depth formats are outside this
experiment. Both planes are copied; the original DSV remains bound and the
tracked states are restored before the Draw. The temporary resource survives
GPU completion. Full/prefix replays and capture loading do not enable it.
This first milestone exposes the option only for texture analysis; the
following section records the subsequent PS debugger integration. It cannot
be combined with the older read-only D32 snapshot option.

The worker checks the extension version, rejects a request when no eligible
selected Draw was processed, and records `depth_roundtrip.copies` as a decimal
string with the experimental scope. A copy count is not a correctness claim.
The build/package scripts verify and include all fourteen patches and hashes.

Production CLI evidence in `hellblade-depth-roundtrip1`, using the unmodified
Hellblade2 RDC and no temporary instrumentation:

| Observation | Result |
| --- | --- |
| Enabled velocity readback | All 2,304,000 pixels equal the continuous prefix through execution 36223; 53 pixels differ from the incorrect split replay |
| Enabled original depth readback | Entire depth/stencil byte image equals that same prefix; 53 pixels differ from the incorrect split replay |
| Selected pixel (456,206) | Velocity UNORM16 `(34608,30148,46932,9359)`; depth `0.0021878723055124283` |
| Option disabled | Reproduces the previous incorrect velocity image exactly; no roundtrip report |
| Following API 36223 selected | Rejected as not applied; no texture file produced |

Each enabled readback reports one completed roundtrip. Velocity SHA-256 is
`df87584674035f2ae0ce0439a61f64b072b043423596462f1192661bf5e9112f`;
depth/stencil SHA-256 is
`3d43f75f10d0dd834dfd0ac55d54b6a5a49a0503e30c87f961b3bb921ff5c2ae`.
The runtime SHA-256 is
`02a0cca7fe646be42a716b4c1f1f65d7caf85080675e9eb951203bf34a2815ea`,
and this patch SHA-256 is
`35e3ee498bedb1122353489d8c9b6cc1e070b344009e7c1b7511e1e043545168`.
The enabled workers load neither GPA nor Python. Research sources, GPU query
hooks, hard-coded event probes and success-log preservation are removed.

The request-scope/default checks pass (**4 passed**, **0 failed**, **17 ms**,
including setup/cleanup). Build 370 compiled worker/CLI/UI but ended with a
misspelled test-target name; build 371 compiled the correct test target. The
first patch build rejected CRLF patch context before compilation; after LF
normalization the exact fourteen-patch source/build verification passes.
Package-script parsing passes; full portable-package acceptance remains
unverified. This restores one demonstrated texture boundary through an
explicit workaround, not general selected replay, analysis or frame fidelity.

### D32S8 refresh in pixel debugging

`--depth-roundtrip` now also accepts `debug-pixel`. The PS workspace exposes
**D32S8 depth refresh (experimental)** beside the older D32 snapshot in a
separate options row. Both default off and are mutually exclusive. The option
is disabled while work runs, clears on event/session/experiment changes and
is hidden for DX11. Changing it invalidates the request; incoming traces must
also report the matching refresh mode. Completed-copy counts and the limited
correctness scope remain visible and survive JSON export. Pixel History was
not integrated at this milestone; its subsequent integration is recorded below.

Real CLI comparison `hellblade-depth-roundtrip-debug1/{disabled,enabled}` uses
execution **36222**, automatic pixel invocation at **(456,206)**, sample 0.
Both return **131** steps. Without refresh, all three target observations are
zero. With refresh, the observations before DebugPixel, after trace creation
and after stepping all equal
`(0.5280842185020447, 0.4600290060043335, 0.7161363959312439, 0.1428091824054718)`.
These match the independently read UNORM16 texture values from the previous
section. The enabled report records **three** completed roundtrips. Both
workers load neither GPA nor Python; the fourteen-patch DLL is unchanged.

This fixes the GPU target reference, **not shader arithmetic equivalence**.
Both traces produce target-0 SSA values `_163`, `_164`, `_170`, `_175` of
`(0.5280897617340088, 0.46003520488739014, 0.7161379456520081, 0.14453497529029846)`.
Their UNORM16 conversion is `(34608,30148,46932,9472)`, versus GPU
`(34608,30148,46932,9359)`. The alpha difference remains **113 stored units**.
The earlier Pixel History shader output has alpha `0.14281071722507477`, also
different from the interpreter. No selected-instance/fragment equivalence is
inferred for this 380-instance Draw.

The disassembly encodes the low bits of `_142 = _55/_56 - _51/_52` into alpha.
Both traces have numerator bits `0x3f7fffff`, denominator bits `0x43e48853`
and `0x43e33824`, and interpreted difference `0xb7542500`. A separate numeric
calculation using rounded reciprocals and fusing the first product with the
subtraction gives `0xb754248f`, consistent with the observed encoded GPU
alpha. This narrows the next investigation to division lowering/contraction
and invocation inputs; it is **not evidence of the actual GPU instruction
sequence**, and no arithmetic substitution is shipped by this change.

The targeted workspace check loads the actual enabled report, stops at
instruction **137** / step **125**, and watches `_175`, keeping its unresolved
difference visible alongside the GPU target values. It verifies option
exclusion, request invalidation, stale/mismatched result rejection, busy state,
context reset, DX11 hiding and JSON export: **3 passed**, **0 failed**,
**1,362 ms**, including setup/cleanup. Screenshot
`depth-roundtrip-debug-ui1/depth-roundtrip-pixel-debug.png` was inspected.
Request-scope validation passes separately: **3 passed**, **22 ms**.
Builds **372–374** pass. Complete PS correctness and the remaining acceptance
items below remain unfinished.

### Division fusion counterexample and bound Shader export

The targeted `tests/fixtures/dx12-fast-div-sub.{cpp,hlsl}` GPU reproduction
uses the exact four operand words from the preceding section. Fast and precise
paths read separate root-constant copies, so precise propagation cannot change
the fast expression through shared SSA values. DXC output confirms separate
fast `fdiv/fsub`, precise `fdiv/fsub`, and explicit `FMad` operations.
On the current adapter (vendor **4318**, device **11352**), independent GPU
readback produces:

| Computation | Float32 bits |
| --- | --- |
| Fast `a/b - c/d` | `0xb7542570` |
| Precise separate divisions and subtraction | `0xb7542500` |
| Explicit `mad(a, rcp(b), -preciseRight)` | `0xb754248f` |

The fast minimal expression matches the numeric result of the **opposite
fusion candidate** from the one matching the original PS's encoded alpha;
this does not identify the actual machine instruction sequence. The original trace's
separate arithmetic agrees with the precise control. Thus fast flags and
operand values alone do not establish a unique driver lowering, and replacing
all eligible divisions/subtractions with one GPU helper would not be a proven
fix. The existing fast-multiply/add experiment is unchanged. No new arithmetic
substitution or sample-specific numerical correction is shipped.

The fixture is a manual, single-Dispatch reproduction of this concrete fault:

```powershell
dxc.exe -T cs_6_6 -E main -O3 tests/fixtures/dx12-fast-div-sub.hlsl -Fo <probe.dxil>
# From a Visual Studio developer shell:
cl /nologo /std:c++20 /EHsc /O2 /I third_party/renderdoc tests/fixtures/dx12-fast-div-sub.cpp /Fe:<probe.exe> /link d3d12.lib dxgi.lib
<probe.exe> <probe.dxil> <RenderDoc-1.45.dll> <new-capture-prefix>
```

The precise control initially shared loads with the fast expression; DXC
propagated precise requirements and removed the intended distinction. That
version was rejected by disassembly inspection before GPU execution. The
corrected fixture was run once; generated shader SHA-256 is
`09c34a5a1db80c0d0f202e884a4006484d9b8395048669fe517a6755251d8a8b`.
Observed GPU values are not portable expected constants for other drivers.

Shader debug workers now save available bound DXBC/DXIL bytes beside
`debug_shader.asm` as `debug_shader.dxbc` or `debug_shader.dxil` in the worker
output directory. `shader_bytecode` records status, relative filename, format,
decimal byte count, SHA-256 and scope. Missing or other-encoding bytes are
explicitly unavailable. Exported bytes include active replacements and are
not automatically labelled original-capture bytes or root-compatible code.
The output preserves compilation context that the readable RenderDoc listing
does not retain. Exporting a report JSON elsewhere does not copy this sibling
binary artifact.

`hellblade-depth-roundtrip-debug1/bytecode` exports **33,072** bytes with SHA-256
`eb1ff531c6d8ca0024880c8f3be0667c3b467bb9345246716d03c671706c3d66`,
equal to the previously observed bound PS digest. DXC successfully reads it
and identifies the original fast divisions `%99/%100` and subtraction `%103`.
All **131** debug steps and all GPU reference fields exactly equal the prior
enabled report; the worker loads no GPA/Python modules. Build **375** passes.
Evidence is retained in `dxil-div-sub-gpu1.json`, `dxil-div-sub-evidence1.json`,
the tiny RDC and the bound-shader export directory, outside Git. The actual
original GPU lowering and PS alpha discrepancy remain unresolved; further
work must retain the original program context rather than infer it from the
minimal expression.

### Original-context PS precision experiment

The manual `tests/fixtures/dx12-ps-precision.cpp` reproduction keeps the original
Hellblade2 PS program and its existing replay bindings, pipeline, Draw and
selected-event depth refresh. It uses the previously recorded independent RDC,
execution **36222** / replay event **88694**, resource **383**, and pixel
**(456,206)**. It is specific to those observed capture identities and is not a
general replay command or an automatic GPU regression test.

`tests/fixtures/dxil-assemble.cpp` assembles DXC textual IR directly with
`IDxcAssembler`, then validates the resulting DXIL container. It does not run an
HLSL optimizer. The control's complete `MainPixelShader` body exactly equals the
original DXC disassembly. The precision variant differs at exactly three
instructions, removing `fast` from `%99 = fdiv`, `%100 = fdiv` and
`%103 = fsub`; all other body instructions are identical. Reassembly strips
container/reflection metadata, so bytecode identity with the original is not
claimed. Both variants pass DXIL validation, and their SHA-256 values are:

- Control, **7,756** bytes: `e515389f5c1ffb23fe4e092afcc9ade96edd386d42ed0eac965154b6b0fc92fa`.
- Precision variant, **7,752** bytes: `59f90e3fc1922e5910b2db660a92bff0a0fce5d6150fa6353210cff38cf4e1fa`.

One successful GPU session observes original, control replacement, precision
replacement and restored original in sequence. The freshly exported bound
original matches the previously recorded **33,072**-byte PS digest. The entire
**1920 x 1200** UNORM16x4 texture is exactly equal for original, control and
restored original, with SHA-256
`df87584674035f2ae0ce0439a61f64b072b043423596462f1192661bf5e9112f`.
The precision variant changes **31** pixels, exclusively in alpha; RGB remains
exact across the whole image. Its texture SHA-256 is
`4d3b8b770369912ea9e5499d85a6e1b32002b56d565e37c06fcec2354d9d1a30`.

At the selected pixel, original/control/restored storage is
**(34608,30148,46932,9359)**. The precision variant is
**(34608,30148,46932,9472)**, exactly the earlier interpreter's output after
UNORM16 conversion. Thus tightening the original program's division/subtraction
flags reproduces the stored alpha discrepancy without replacing the program
with a minimal expression. This supports a fast-math lowering cause for this
pixel, but does not reveal the original machine instructions, certify all
intermediate values or establish a general fusion rule. Production replay and
interpreter arithmetic are unchanged; complete original PS correctness remains
unfinished. The experimental replacement affects all uses of this PS in the
RDC, not only the selected Draw, and this direct RenderDoc probe is not evidence
of workspace experiment persistence or portable-package acceptance.

From a Visual Studio x64 developer shell, with generated files outside Git:

```powershell
cl /nologo /std:c++20 /EHsc /O2 tests/fixtures/dxil-assemble.cpp /Fe:<assembler.exe>
cl /nologo /std:c++20 /EHsc /O2 /I third_party tests/fixtures/dx12-ps-precision.cpp /Fe:<probe.exe> /link artifacts/renderdoc-1.45-d16/x64/Release/renderdoc.lib
# Dump the bound original PS with the pinned v1.8.2505.1 DXC. Preserve a control
# listing, and remove fast only from the three instructions named above in a copy.
<assembler.exe> <dxcompiler.dll> <original.ll> <new-control.dxil>
<assembler.exe> <dxcompiler.dll> <precise.ll> <new-precise.dxil>
# Put the fourteen-patch RenderDoc DLL directory first in PATH for this process.
<probe.exe> <native-dx12_capture.rdc> <new-control.dxil> <new-precise.dxil> <new-output-directory>
```

Both committed fixtures compile. The assembler fixture reproduces the control
digest exactly. Before the successful run, the standalone probe lacked the
RenderDoc replay-marker export and crashed during capture opening without
producing observations; the corrected fixture includes the marker. That failed
run is not counted as validation. Evidence, disassembly, generated binaries and
texture readbacks remain in `original-ps-precision1` outside Git. The offline
verifier confirms complete function-body comparisons and whole-image equalities;
no further GPU replay was run to verify those files.

### Selected depth refresh in Pixel History

The same default-off `--depth-roundtrip` option now supports `history`, and
the existing Pixel History panel exposes **D32S8 depth refresh (experimental)**.
The selected-event eligibility checks remain unchanged; this does not refresh
every Draw in a history. Reports retain that limitation alongside the completed
roundtrip count. The UI rejects stale or differently configured results, disables
the option while busy, resets it for context/event changes and hides it for DX11.

The actual Hellblade2 query `hellblade-depth-roundtrip-history1` selects execution
**36222**, resource object **2152463529088** / creation **511789**, pixel
**(456,206)**, mip/layer/sample **0**. All **five** history records are exactly
equal to the earlier report that exposed the boundary contradiction, including
their values, test flags and execution mapping. The restored selected texel is
now nonzero both before history and after restoration; both observations exactly
equal the last record's event-after color and the independently read texture.
`history_boundary.status` changes from `differs_from_selected_texel` to
`matches_selected_texel`. The previous boundary-conflict partial flag is no
longer needed. This is selected-boundary evidence, not validation of every
intermediate fragment or complete history.

The report records **three** completed roundtrips and loads no GPA/Python
modules. The fourteen-patch DLL remains
`02a0cca7fe646be42a716b4c1f1f65d7caf85080675e9eb951203bf34a2815ea`.
The counter named `copies` counts completed copy-out/copy-back pairs, not
individual CopyResource commands or performance samples.

Selecting the final row locates **36222 / action 0**. **Debug PS** passes its
pixel/sample/primitive/indirect-action selection and refresh mode into the
existing debugger. Selecting the earlier Draw at **12574** does not carry the
selected-event refresh mode to that different execution. This transfer does
not infer a fragment index or instance identity. The PS numerical discrepancy
documented above remains unresolved.

Focused UI checks use the real history report for values, navigation, mode
transfer, stale/mismatched rejection, reset and JSON export. A separate
lightweight BF1 workspace check verifies the MainWindow signal connection
without launching replay; it is transport evidence, not BF1 D32S8 eligibility.
Together: **4 passed**, **0 failed**, **1,312 ms**, including setup/cleanup.
Request-scope checks: **3 passed**, **8 ms**. Builds **376–377** pass. Screenshot
`depth-roundtrip-history-ui1/depth-roundtrip-history.png` was inspected. No
additional GPU replay was run for the UI checks.

### Query completion ordering and Talos resource-data audit

Query coverage now retains the execution, submission and queue of each observed
`EndQuery`. A resolve on another queue requires a path through captured queue
order and single-writer monotonic Fence dependencies. CPU record order alone,
an already-satisfied initial Fence value, a Signal before the query submission,
and cyclic dependencies cannot establish that path. Missing, ambiguous or
unresolved graph nodes cannot establish it either. The graph includes
initialization operations and supports cancellation; repeated submission-pair
lookups use a bounded cache, built only when a cross-queue completion occurs.

Reports distinguish missing results from `unresolved_query_dependency`, retain
bounded examples of the latter with decimal-string identities, and expose
`completion_order_verified`, `cross_queue_completed_count` and
`unordered_completed_count`. Existing session capability and native preflight
paths reject the unresolved dependency before replay. The legacy
`completed_count` remains the number of observed matching EndQuery slots;
it is not by itself a synchronization guarantee. The report's broader
`cross_queue_dependencies_verified` remains false: query-slot reuse, concurrent
writers, CPU waits and all resource hazards are not certified by this check.
This is consistent with the same-queue ordering requirement described in
[Microsoft's query-resolution specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12CpuTimelineQueryResolution.html).

Seven CPU fixtures cover same-queue order, absent synchronization, initial-value
waits, premature Signals, direct and transitive Fence dependencies, and a cycle.
They also check the rejecting session capability, 64-bit provenance and
cancellation. With the existing query-boundary/identity and queue-dependency
checks: **11 passed**, **0 failed**, **99 ms**, including setup/cleanup;
`query-completion-order-tests1.txt` records the run. A separate CPU-only pass over
the actual directories takes **814 ms**: BF1 has **5** resolve entries,
Hellblade2 **240**, and Talos **2**. Their observed completed results are all
same-queue. Talos still lacks **140 / 146** results at executions **16001 / 19409**;
no new unordered completion is reported. Evidence is in
`query-completion-order-captures1`. No GPU replay was needed for this change.
Build **379** passes for the workspace, CLI, Worker and focused test target;
build 378 built the workspace/tests but then stopped at a misspelled CLI target.

The accompanying Talos audit maps `resources` read-only and scans every byte
offset wholly inside its **3,679** referenced kind-0 CopyOperation data views.
The views contain **1,590,966,351** bytes within a **1,591,201,807**-byte file;
scanning uses bounded 4 MiB chunks. No raw 64-bit value falls within either
query destination GPU-address range: **12696616960 + 65536** or
**12696485888 + 65536**. This complements the earlier packet-payload audit,
but excludes unreferenced bytes, encoded/derived addresses and values split
across views. It does not prove that subsequent GPU work cannot depend on
query results and does not recover the missing values. Evidence is retained
in `talos-query-blob-addresses1.json` outside Git. No missing resolve is skipped,
no substitute query values are generated, and the complete Talos replay
acceptance item remains unfinished.

### Captured DXIL fast-math flags in instruction debugging

The friendly DXIL disassembly previously hid the `fast` permissions on the
two divisions and subtraction involved in the original-context experiment.
The fifteenth RenderDoc patch now appends captured fast-math permissions to
the existing instruction line, for example
`float _142 = _138 - _139; // fast-math: fast`. It preserves the parsed `fast`,
`nnan`, `ninf`, `nsz` and `arcp` flags through RenderDoc's existing flag formatter.
It adds no lines and changes neither bytecode nor interpreter arithmetic.

The worker detects version **1** of the annotation patch and reports availability,
annotated-line count and scope under `debug_scope.dxil_instruction_flags`.
The debugger displays the comments while stepping, explains their meaning as
compiler permissions, and preserves them in exported reports. Older libraries
and reports remain readable without claiming annotation support. These comments
do not reveal the driver's instruction sequence or resolve the numerical
discrepancy described above.

One actual Hellblade2 PS trace at **36222**, pixel **(456,206)** with selected-depth
refresh produces **53** annotated lines, matching the count of fast arithmetic
and comparisons in the original DXC listing. Removing only those comments makes
the entire friendly disassembly exactly equal to the previous report. All
**131** steps, complete trace metadata, GPU reference fields and bound Shader
bytecode metadata are unchanged. No GPA/Python modules were loaded. Evidence is
in `dxil-instruction-flags1`, including the offline comparison report.

The focused UI check stops at instructions **99 / 100 / 103** and verifies
their original disassembly lines **635 / 636 / 639**, visible annotations,
exported text/metadata and legacy-report handling: **3 passed**, **0 failed**,
**1,394 ms**, including setup/cleanup. The inspected screenshot is
`dxil-instruction-flags-ui2/dxil-instruction-flags.png`; the current subtraction
and both preceding division breakpoints are visible. The UI checks consume the
saved trace and launch no further GPU replay. Builds **380 / 382 / 383** pass;
the intervening test compile needed an explicit `QTextBlock` include.

The exact fifteen-patch build passes the source-tree check. DLL SHA-256 is
`d5740e85ab0543944b480ce5b44a356b3ab7cd16ac5faf261cbeb50b0d381ac2`;
the new patch SHA-256 is
`8a2708ef82b12b70fac91b4eabb82a09813b7a71e9f6babcce130ce503033346`.
Build and packaging manifests now require and include this patch. Full portable
package acceptance and the remaining feature table below remain unfinished.

### Talos continuation with a deliberately altered query subset

A single isolated experiment checks whether the missing query results conceal
additional native-command blockers later in Talos. It deliberately changes the
capture and **does not establish original-frame fidelity**. The production
reader, replay checks and missing-query capability result are unchanged.

The manual `tests/fixtures/talos-query-subset.py` fixture pins the original thread
SHA-256 and takes the existing query-coverage report. In a separate directory it
replaces only original records **188923 / 192515** with the contiguous ranges
that have observed replayed EndQuery results: **52 / 49** resolve calls preserve
all **96 / 90** known result writes. The **140 / 146** missing result writes are
omitted; their values are not reconstructed or substituted. Every other packet
header and payload is byte-identical. New records retain the original timestamps
and same-thread order. The **99** extra records move the final execution identity
from **19410** to **19509**; these are distinct capture identities, not a claim
that original and altered execution numbers can be interchanged.

Unmodified files are hardlinked to avoid copying the 1.59 GB resource file and
are used only by the read-only replay tools. The fixture must not be used as an
editable capture. The modified thread SHA-256 is
`db76f40a5cb1c3b7899fe6c58ae905b0ce1d5fd164c1d14fdbb53590ed7a1703`;
the original remains
`db5bbf6e1ff0cd7483cce327a435c476ed96f18b1a595fab1f1519b180ce6db7`.

The unchanged native backend reaches altered execution **19509**, after the
last indexed submission, with **1,010 indexed Draws + 36 non-indexed Draws** and
**101** covered query-resolve calls. SDK **616** is used. The debug layer reports
**0 errors** and **781 messages**, including existing clear-value and GPU-address
validation warnings; this is not a warning-free run. The real readback of final
backbuffer creation **35118**, object **1651480816**, is **1920 x 1080 BGRA8**,
**8,294,400** bytes, SHA-256
`e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
The image was inspected. `completed=true` describes this selected boundary of
the altered capture; `frame_complete=false` and `frame_replayed=false` remain.
No comparison against a different original Present is treated as fidelity proof.

```powershell
# Save the query_replay_coverage object from the original command-lists report.
# Python is used only to construct this research fixture, never by replay.
python tests/fixtures/talos-query-subset.py <original-capture> <coverage.json> <new-directory>
FloraGPA.Cli.exe command-lists <new-directory>/capture.gpa_frame --out <new-index-output>
FloraGPA.Cli.exe replay <new-directory>/capture.gpa_frame --event 19509 --id 35118 --max-bytes 4294967296 --debug-device --out <new-replay-output>
```

Evidence is retained in `talos-query-subset1`: mutation provenance, the **101**
covered CPU query entries, native report/readback and offline byte comparisons.
The committed fixture reproduces the executed modified stream exactly without
another GPU run. A separate original-directory preflight still rejects execution
**19410** at **16001**, reports **140 of 236** unavailable results, and confirms
`device_created=false`. Thus the original query guard is verified intact.

This narrows the next implementation work to handling unavailable query outputs
and their consumers explicitly; it does not authorize silently omitting them.
Any production partial-output path must preserve unavailable-range provenance
and reject dependent reads instead of exposing leftover allocation bytes as
query results. Original Talos whole-frame replay, histories beyond the verified
prefix and corresponding analysis acceptance remain unfinished.

### Explicit isolation of unavailable Talos query outputs

Native replay now has an opt-in `--isolate-missing-queries` path for `replay`
and `buffer`. It reads the original capture, preserves execution IDs and splits
each affected ResolveQueryData into contiguous ranges supplied by completed
queries. Missing results are omitted and remain **unavailable**, never filled
with zero or reported as reconstructed historical values. Default replay and
workspace capabilities retain the original missing-data failure.

The initial scope is deliberately restrictive:

- Destinations must be private READBACK buffers with one captured resource
  lifetime and COPY_DEST state. Placed buffers require one heap lifetime and no
  overlapping captured resource allocation, including future placements and
  native allocation padding. Reserved, shared and protected cases are rejected.
- These allocations are reserved before initialization, including before their
  first missing resolve. Their complete contents start unknown. Completed query
  writes make exact byte ranges available; later missing writes revoke any old
  contents in those ranges. Ranges and identities use 64-bit integers/decimal
  strings, with overflow checks.
- Ordinary resource access, GPU address translation, descriptors, copies,
  barriers, tile mappings and experiment writes cannot consume these reserved
  buffers. Any such access fails explicitly. CPU inspection permits only fully
  available ranges after the selected GPU boundary completes. It rejects mixed,
  missing and untouched ranges before creating an output file.
- Query writes to an isolated destination must use one queue, with same-queue
  completed results. Cross-queue query outputs, RenderDoc capture, performance
  queries and modification experiments are not enabled in this mode. This does
  not certify arbitrary GPU-generated addresses, query-slot concurrency, original
  CPU control flow or complete capture fidelity.

`query_replay_coverage.destination_byte_ranges` retains execution/source/queue
provenance through its owning resolve entry. Native resource reports list the
current known/unknown byte spans. `query_readback_availability_valid` is true
only after the requested GPU boundary succeeds; a failed submission's state
model is not a readable result. Command counts retain original execution
instances: the two Talos resolves become **101** native contiguous resolve calls.

The targeted `talosIsolatedQueryReplay` check used the **unmodified** Talos
directory and reached original execution **19410**, SDK **616**, with **1,046**
Draws and **0 D3D12 validation errors** (781 existing messages, not warning-free).
It read final backbuffer creation **35118**, object **1651480816**, as 1,920 ×
1,080 BGRA8. The 8,294,400 storage bytes exactly match the prior altered-stream
experiment, SHA-256
`e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
The image was inspected. This comparison validates continuation of the native
drawing path; it is not an original-GPA full-frame comparison.

The same GPU run read an available 8-byte query at offset **64** from each
destination: creation **78132** returned `300e000000000000`, creation **12800**
returned `b912000000000000`. Their total available ranges were **768** and
**720** bytes respectively. Reads of the first unavailable byte, the mixed
1,888-byte resolve span and the untouched tail failed. Ordinary access, GPU
address access, tile mapping and an introduced overlapping placement were also
rejected. Evidence is in ignored `artifacts/dx12-research/talos-isolated-query1`;
the original thread SHA remains
`db5bbf6e1ff0cd7483cce327a435c476ed96f18b1a595fab1f1519b180ce6db7`.

Focused CPU checks covered interval overwrite/split/merge, byte-range overflow,
offsets above 2^53, repeated execution identity, malformed query data and queue
completion ordering. Default CLI replay still exits **2**, reports
`device_created=false` and stops at execution **16001** with **140 of 236**
results missing. Conflicting isolation/performance options fail before device
creation. No broad sample replay or regression suite was run for this change.

A second, CLI-only GPU check exercised the public failure path with `buffer
--event 19410 --id 78132 --offset 0 --length 1 --isolate-missing-queries`.
It reached the original boundary with zero validation errors, then exited **1**
at `buffer_readback` with an unavailable-byte error. No `buffer.bin` was created;
the report correctly distinguishes successful GPU completion from failed data
export. This was the only additional Talos GPU run for the change.

Example explicit incomplete replay:

```powershell
FloraGPA.Cli.exe replay D:\CDXrepo\captures\Talos_2026_10_18__00_55_08.gpa_frame `
  --event 19410 --id 35118 --max-bytes 4294967296 --debug-device `
  --isolate-missing-queries --out artifacts/talos-isolated
```

Both `frame_complete` and `frame_replayed` remain false. Historical values,
analysis beyond the strict prefix and full Talos acceptance remain unfinished.

### Workspace query-isolation mode and result identity

The existing DX12 output controls now include **Isolate missing queries**.
It is off by default and resets when a capture is opened. The shared selection
and capability interface carries the mode, so the workspace and CLI use the
same query preflight. A buffer selected as an image, an invalid execution or an
unsupported texture descriptor remains rejected; enabling isolation does not
bypass the rest of the replay capability checks.

Native replay and Buffer Read pass the explicit option through the isolated
Worker. Changing mode invalidates the image, buffer bytes, export availability
and analysis context. The output caption identifies incomplete query outputs;
the resource status and tooltip explain the missing historical values. Raw
texture exports preserve `missing_query_policy` and `frame_complete=false` in
their JSON sidecar. Pixel History picking is cleared on entry. Histories,
Shader Debug, geometry, pipeline inspection, performance and experiments remain
unavailable in this mode; their implementation is still required, not accepted
as complete because controls are disabled. The UI, native Worker and analysis
process reject unsupported combinations independently.

Request identity schema **2** adds the exact `isolate_readback_outputs` policy
for these DX12 requests. Strict DX12 and DX11 continue to write/read schema **1**.
Both retain decimal strings for 64-bit request/revision values. Identity
equality includes the mode even when all other fields match. Worker replay and
buffer commands reject envelope/argument mismatches before replay. Accepted
reports must agree with the expected top-level, native and storage policy and
must contain a valid completed GPU boundary for isolated query availability.
This prevents a strict result from being accepted as an isolated one or vice
versa. Legacy strict reports remain readable.

Targeted evidence:

- `workerIdentityEnvelope`, `isolatedQueryReportPolicy` and
  `queryWorkspaceBoundary`: **5 passed**, including setup/cleanup, in 1,766 ms.
  These cover schema compatibility, UINT64 identities, mode-only stale-result
  rejection, Worker command mismatch, analysis-process rejection, report-policy
  mismatch and actual Talos capability/control state without GPU replay.
- `isolatedQueryWorkspaceReplay`: **3 passed** in 5,080 ms. One actual Talos
  GPU run through the workspace reaches **19410**, displays creation **35118**,
  and exports all 8,294,400 BGRA8 bytes with SHA-256
  `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
  The exported sidecar identifies the incomplete policy. The test also checks
  that switching back clears the image/export and restores the strict replay
  rejection, and reopening the capture resets the option.
- Screenshot and exported storage are retained in ignored
  `artifacts/dx12-research/talos-isolated-query-ui1`. The screenshot was inspected;
  its long caption motivated moving the incomplete marker to the beginning.
  That caption-only adjustment did not trigger another GPU run. The final UI
  build and focused CPU mode-switch check pass (builds **387/388**).

This connects the previously verified partial native path to the shared
workspace. It does not recover query values, validate original-GPA full-frame
fidelity or enable analysis of the incomplete continuation.

### Query-isolated analysis capture and Talos color discrepancy

Native `replay --isolate-missing-queries --renderdoc <dll>` now embeds a
versioned queue marker in the RDC itself. Its bounded JSON records the selected
execution/boundary, reserved resource lifetimes and widths, and each missing
resolve's submission, queue and contiguous known/unknown byte ranges. All
64-bit identities and offsets remain decimal strings. No sidecar is needed to
identify an incomplete capture. Duplicate, malformed, oversized or inconsistent
policies are rejected.

`rdc-analyze --isolate-missing-queries` explicitly accepts that marker for
`inventory` and `texture` only. Request and capture policies must agree; request
identity includes the policy. Results preserve the policy and
`frame_complete=false`. Query destination buffers remain inaccessible in their
entirety: a range known after a later resolve cannot establish availability at
an earlier analysis event. Histories, debugging, geometry and counters remain
unsupported for isolated captures. The GUI does not yet enable these analysis
paths. This is a capture/provenance milestone, not analysis correctness acceptance.

Targeted evidence in ignored `artifacts/dx12-research/talos-isolated-analysis1`:

- The original Talos capture reaches execution **19410** with zero native
  validation errors and unchanged native output SHA-256
  `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
  Its RDC preserves both isolated resources/resolves, **1,046 Draws** and
  **661 resources**; execution **19410** maps to RenderDoc **20538**.
- RenderDoc final texture readback succeeds but differs from native in
  **204,352 pixels**, maximum BGRA deltas **58/60/72/112**. Channel order is
  unchanged. Successful readback is not a fidelity pass.
- The same RDC's first depth Draw **12675**, resource creation **167081**,
  layer **1**, exactly matches the native 8 MiB texture hash
  `c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`.
  First color **16000**, creation **42877**, differs in **564,384 pixels**,
  before the first missing-query resolve **16001**.
- A fresh **strict** native capture ending at **16000** reproduces the known
  native first-color hash
  `4fcac91107578f2a4a96ec8ffaaaa1d1cf343155205ecf28beb3861c843eb2e8`.
  Its RenderDoc readback still differs in **218,373 pixels** (maximum BGRA
  deltas **65/82/93/111**). Thus the discrepancy also occurs without query
  isolation; its cause remains unresolved. The strict and longer RDC reads
  are themselves different and are not interchangeable reference results.
- Indexed resource usage identifies four writes to this color target:
  **15981, 15988, 15996, 16000**. Comparing native and RDC immediately after
  the first write **15981** already gives **217,348 differing pixels**.
  The mapped PSO is creation **156649**; PS texture input is creation
  **156503**, object **1657132384**, and PS constants use buffer creation
  **150566** at offset **32457728**. These locate the next input comparison;
  they do not establish that the shader or any particular input is faulty.

Builds **389–391** pass. Focused marker/mapping tests report **5 passed** and
job-preparation tests **4 passed**, including setup/cleanup. They cover uint64
identity preservation, range/boundary validation, duplicate markers, policy
mismatch, prohibited buffer access/actions and option typing. GPU checks were
serial and limited to this capture path and the newly reproduced color fault.
The analysis processes report no loaded GPA modules. Full Talos fidelity,
isolated histories/debugging and portable-package acceptance remain unfinished.

### Bounded analysis-buffer reads and Talos alpha-path localization

`rdc-analyze <capture.rdc> buffer --execution <id> --resource <object>
--resource-creation <record> --offset <bytes> --length <bytes>` now exports
`buffer.bin` from the selected replay boundary. Offset/length and result sizes
are canonical decimal uint64 strings. Requests require an explicit resource,
1 byte to 64 MiB, no arithmetic overflow and a range wholly inside the replay
buffer. The backend checks the actual resource type/extent, fatal replay status
and exact returned size before committing output. Texture requests cannot accept
buffer ranges. Query-isolated analysis continues to reject buffer actions, so
this does not expose unknown query output. UI buffer inspection still uses the
existing native path.

The new path was used for the Talos color fault rather than a broad replay test.
At execution **14920**, buffer creation **150566**, object **109459904**, offset
**32281344**, all **752 bytes** declared by the PS constant block match native
replay, SHA-256
`a29e2d58b377cbed44a3b10d337625f461e970a21cf26090343a2f48462afaf5`.
The resource is 33,554,432 bytes long; offset 33,554,431 / length 2 fails with an
extent error and produces no `buffer.bin`. The analysis reports no GPA modules.
Focused preparation tests report **5 passed**, including setup/cleanup, covering
offsets above 2^53, overflow, noncanonical/numeric inputs, zero/excessive lengths,
missing parameters, action restrictions and query isolation. Builds **393/394**
pass after correcting the backend dispatch whitelist and shared-validator link.

All following evidence is under ignored `artifacts/dx12-research/talos-color-input1`.
GPU checks were serial, each following the reproduced color fault:

- Draw **15981** input creation **156503** already differs in **220,776 pixels**,
  RGB only. Its producer **15969** has an exactly matching 1x1 exposure input
  (creation **156411**), but differing HDR creation **193** and processed input
  **157183**. This excludes the final color-copy Draw as the first source of
  the observed discrepancy.
- HDR Draw **14835** has a separate isolated-draw issue: **12,914** pixels are
  unwritten in default RenderDoc analysis; all other pixels match native.
  The D32 depth input (creation **8468**) is byte-identical. Existing explicit
  `--readonly-depth-snapshot` performs one copy and makes the entire 16,588,800-byte
  HDR image identical, SHA-256
  `0204dbbde1a98625838132f7ab0c465445d36f518b4a21d831da602d486b18ba`.
  Reading after the following non-Draw execution **14836** also matches without
  that option. This verifies the selected workaround and separates isolated Draw
  behavior from continuous-prefix replay; it does not fix full-frame fidelity.
- Bisection of continuous-prefix HDR observations finds a matching boundary
  **14912** and **149 differing pixels** at **14921**, after Draw **14920**.
  This is a localized bracket, not an exhaustive proof of monotonicity.
  Its PS constants match as above. PS input creation **156599** has identical
  RGB but **219,446 differing Alpha pixels**. At (1917,286), native Alpha is
  1 and RenderDoc is 0.91650390625. The pipeline binds this texture at PS t8.
- That Alpha is written by full-screen Draw **14706**, which samples creation
  **156503** with an Alpha-only output mask. The preceding generation/filter
  chain is **14672 → 14684 → 14694**. The first generation Draw's output is
  **exact throughout its actual 1440x810 viewport/scissor**. Differences outside
  this region follow Discard and are not accepted as evidence of a rendering
  fault. Its R32 depth-pyramid input creation **150408**, Mips **0 and 5**,
  is also byte-identical; other mips have not been verified.
- After **14694**, **120,432 pixels inside that same valid viewport differ**.
  Draw **14684** reads depth creation **150408** and texture **156503**, writing
  texture **156615**; Draw **14694** reads **150408/156615** and writes **156503**.
  Their PS constants are at offsets **32262656 / 32263168** in creation
  **150566**. These are the next comparisons needed to isolate the filter fault.
  The observations do not yet identify a bad shader, descriptor, resource value
  or driver operation.

Talos color fidelity remains unfinished. The matching selected depth workaround,
constant-buffer bytes and valid initial Alpha region do not validate subsequent
filters, histories, debugging or the full portable package.

### Discard-layout correction and explicit replay policy

The Talos filter investigation reproduced two different effects. In the first
filter (**14684**), only **653 Alpha pixels at x=1438** differ inside the
1440x810 viewport. The combined 1 KiB constant range at offset **32262656** in
creation **150566**, covering both filters' PS constant locations, matches
native exactly (SHA-256
`62c0a0b2a963c3876e2a048a1b222bf1c4be5f2ba21f8e021eaf326d24e819be`).
Creations **150408/156503/156599/156615** are independent committed resources,
not overlapping placed allocations. The much larger second-filter interior
discrepancy therefore needed a separate explanation.

RenderDoc 1.45's balanced replay inserts a diagnostic pattern after
`DiscardResource`. On enhanced-barrier-capable devices its non-MSAA texture
copy changes the resource to COPY_DEST, then COMMON when the original call
provides no enhanced layout. The following captured legacy render pass expects
its attachment state to be preserved. Direct-list RT/depth discard requirements
are documented in [Microsoft's DiscardResource reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-discardresource).

The sixteenth pinned patch, `dx12-discard-layout.patch`, keeps this diagnostic
copy on legacy barriers for direct-list `DiscardResource` attachment calls,
restoring RENDER_TARGET or DEPTH_WRITE. It excludes simultaneous-access
resources. Enhanced-layout transitions, other queue types, buffer and MSAA
paths retain their previous behavior. It still issues the captured discard and
fills the diagnostic pattern; it does not replace discarded data with invented
historical values. The backend reports `backend_dx12_discard_layout_patch=1`.
The build verifies the complete 16-patch tree; packaging verifies the new patch
version/hash and includes its source.

The CLI also exposes RenderDoc's existing `--replay-optimisation` modes:
`none`, `conservative`, `balanced` (unchanged default), and `fastest`. Jobs
validate the string before loading the backend; results include `replay_policy`
with the mode, enum and DX12 discard-pattern status. Fastest omits those pattern
fills but also changes other optimizations, so it is an explicit diagnostic
comparison, not a general fidelity guarantee. No GUI default or experiment
overlay is changed by this option.

Targeted results under ignored `artifacts/dx12-research/talos-filter1`:

| Observation | Before patch, balanced | After patch, balanced | Explicit fastest |
| --- | --- | --- | --- |
| Second filter, valid 1440x810 region | 120,432 pixels differ | 1,144 differ, only right/bottom boundary; interior exact | Entire valid region exact |
| First color, execution 16000 / creation 42877 | 218,373 pixels differ | 5,854 differ; maximum BGRA deltas 31/36/41/32 | Entire 8,294,400 bytes exact |
| Final isolated query continuation, execution 19410 / creation 35118 | Earlier 204,352-pixel discrepancy | Not remeasured in this policy | Entire 8,294,400 bytes exact |

The final patch reproduces the second-filter 1,144-pixel result, with zero
differences in the interior region (x < 1437, y < 807). The remaining filter differences lie
at the border of the written region and disappear when pattern filling is
omitted; sampling discarded/previously unwritten texels is a supported inference,
not yet a traced per-sample proof. Default balanced replay is still not an exact
full-image match.

Fastest first-color SHA-256 is the established native
`4fcac91107578f2a4a96ec8ffaaaa1d1cf343155205ecf28beb3861c843eb2e8`.
The final isolated result is
`e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`,
preserves both missing-query resolve records and `frame_complete=false`, and
reports no loaded GPA modules. These are native-versus-analysis comparisons;
they do not recover historical queries or validate original-GPA full-frame
semantics. The earlier isolated Draw **14835** still has **12,914** differing
pixels with the new patch and balanced policy: its explicit depth-snapshot
workaround remains a separate, partial result.

Builds **395/396** and the pinned RenderDoc build pass. Focused replay-policy
preparation tests report **3 passed**, including setup/cleanup; both PowerShell
scripts parse successfully. The final runtime SHA-256 is
`9c5505cf60b8aeb6b705c36c9e30bc30acbf9153abab0d0e2fd5a0d414fc9105`;
patch SHA-256 is
`a471acc0ed7e486ed4bd29844b912286a307002808f9961130f6c11a36628a80`.
No broad regression, repeated full-sample sweep or portable acceptance was run.

### Native discard policy and a traced Talos filter sample

The seventeenth pinned patch, `dx12-discard-pattern-control.patch`, separates
RenderDoc's additional diagnostic writes from its replay optimization level.
Flora DX12 analysis now defaults to `balanced` with those writes disabled.
Captured `DiscardResource` calls and enhanced barriers remain executed; values
after discard remain undefined. This does not reconstruct historical contents.
The standalone patched DLL retains RenderDoc's original default until Flora
explicitly selects the policy. DX11 behavior is unchanged.

`rdc-analyze --discard-patterns` enables diagnostic fills explicitly; combining
it with `--replay-optimisation fastest` is rejected. Native discard replay with
an older library fails with an actionable missing-patch error unless fastest
already suppresses the writes. Reports preserve the policy, patch version and
decimal-string skipped-fill count. Counts cover repeated analysis replays, not
unique captured commands. The debug panel visibly identifies diagnostic reports.
Build/package manifests verify the additional patch version and source hash.

Evidence under ignored `artifacts/dx12-research/talos-filter-debug1` now traces
the earlier border inference to a particular sample: execution **14684**, pixel
**(1438, 0)**, instruction **21** samples source texture t1 at approximately
**(0.7499999404, 0.0004629629)**. Linear filtering reaches x=1439 and x=1440 of
the 1920-wide texture; x=1440 lies outside the preceding 1440-wide written
viewport. The native source alpha is 255 at both texels, whereas diagnostic
filling makes the latter zero. After this instruction, r0.y is **1** under
native policy and **0.5000076294** with diagnostic filling. The final shader
alpha is respectively **1.0000001192** and **0.8668378592**; UNORM8 target reads
are **255** and **221**, stable before and after the debugger replays.
Both traces contain 46 states. This verifies this invocation and its format
conversion, not general shader or undefined-data fidelity.

With the new default policy, these entire 8,294,400-byte outputs match native:

| Boundary | SHA-256 | Scope |
| --- | --- | --- |
| Execution 16000 / creation 42877 | `4fcac91107578f2a4a96ec8ffaaaa1d1cf343155205ecf28beb3861c843eb2e8` | Strict prefix, first color |
| Execution 19410 / creation 35118 | `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb` | Explicit query-output isolation; both missing resolves retained, `frame_complete=false` |

These results supersede the previous default-balanced discrepancies for those
two outputs. Historical queries, original-GPA full-frame semantics and the
separate Draw 14835 depth-snapshot issue remain unfinished. Explicit diagnostic
mode reproduces the selected 221-alpha result; a pre-control DLL is rejected
without producing a texture export.

Builds **397/398/399** and the complete 17-patch runtime build pass. Focused policy
and isolation preparation tests report **4 passed** (20 ms); saved-report UI
verification reports **3 passed** (598 ms), including setup/cleanup. The latter
checks the instruction-21 breakpoint, stepping, TEXCOORD.x and SV_Target.w
Watch values, exported policy and visible diagnostic notice without another
GPU replay. Screenshots are under `talos-filter-debug-ui1/replay-debug`.
Both packaging scripts parse successfully. Runtime SHA-256:
`079e490c33b87668f60e365ae831af3a2d0eafa925c96eaddd06b755e7a8c403`;
patch SHA-256:
`8ce9be37937a967393599f4d04f9a2e298cb6b29493ea71a186979d568a0d8fb`.
Full portable-package acceptance has not been run.

### Explicit register Watch during instruction debugging

Recorded DXBC typeless registers can now be watched with an explicit 32-bit
interpretation, for example `registers.r0.float.y`, `registers.r0.int.xy`,
`registers.r0.uint[1]` or `registers.cb0[2].float.x`. Double-clicking a register
adds a vector Watch in the selected register-table interpretation (hex selects
uint). Subsequent display-mode changes do not alter existing expressions.
These are readings of the trace's stored register representations, not numeric
casts or fabricated source declarations. Bare typeless `r0` remains unavailable.
Opaque handles and wide/packed types do not acquire guessed 32-bit views;
missing representations remain unavailable. A source declaration named
`registers` takes precedence over these aliases, including when unavailable.

The aliases follow forward/backward trace changes and register lifetime, include
captured constant-block members, and persist through existing debug configuration
and report export. Source variable tables and source mappings are unchanged.

The two saved Talos execution **14684** / pixel **(1438, 0)** traces verify this
workflow without another GPU replay. At step 21, r0.y is zero; after instruction
21 it becomes **1.0** under native discard policy or **0.5000076293945312** with
diagnostic patterns. The corresponding uint bit patterns are **1065353216** and
**1056964736**. UI checks cover register double-click, fixed interpretations,
constant-block Watch, reverse stepping and exported values. Screenshots under
ignored `artifacts/dx12-research/talos-register-watch-ui1/replay-debug` retain
the current instruction and watches. Targeted model tests additionally cover
lifetime expiry/restoration, signed/unsigned views, configuration round-trip,
source-name collisions and unsupported opaque/wide types. Full stage/debugger
acceptance remains unfinished.

Builds **400/401** pass. Focused model checks report **5 passed** (4 ms), and
the final saved-trace UI check reports **3 passed** (369 ms), including
setup/cleanup. No GPU replay or broad sample regression was added for this change.

### Read-only D32 refresh without replacing the depth attachment

Talos execution **14835** still loses **12,914** pixels under the native-discard
policy. It uses depth Equal with writes disabled, D32 creation **8468**, and HDR
creation **193**. Three research controls separate the existing snapshot's effects:

| Isolated Draw intervention | HDR pixels differing from native |
| --- | --- |
| None, including with API validation requested | 12,914 |
| Copy-source transition and restoration only | 12,914 |
| Copy to temporary storage, but rebind the original depth resource | 12,914 |
| Copy out and back, retaining the original depth resource | 0 |

This rules out the tested state transition and source-copy/rebinding alone as
sufficient workarounds. It does not identify a driver/compression root cause.
The validation report contains loading-stage warnings only and cannot validate
the subsequent isolated Draw. Probe source variants and output bytes remain in
ignored `artifacts/dx12-research/talos-depth-state1`; none of those instrumented
snapshot variants is part of the shipped patch set.

The eighteenth patch, `dx12-readonly-depth-roundtrip.patch`, extends the existing
explicit `--depth-roundtrip` path and reports patch version **2**. Alongside the
existing depth-writing D32S8 path, it accepts read-only D32 direct Draws with
stencil disabled, one mip/layer/sample/node and legacy depth states. It excludes
Always/Never tests and indirect/mesh Draws for this new D32 path. It copies all
planes out and back, restores resource states and retains the original resource,
DSV and PSO. It remains experimental and **off by default**. Reports distinguish
supported modes; the count is completed roundtrips, each involving two GPU copies.
Version 1 remains usable for D32S8; the selected D32 request on that version fails
without a texture export. Packaging verifies both the original and extension
patch hashes and the version-2 runtime export contract.

The final version-2 implementation reproduces exact native HDR equality across
**16,588,800 bytes**, SHA-256
`0204dbbde1a98625838132f7ab0c465445d36f518b4a21d831da602d486b18ba`.
All **8,294,400** original D32 bytes remain exact, SHA-256
`9978bcc8ffd3c43563a528115aec9d38f4a8d34dc9de1f7e39cfdb3dc4397224`.
At **(1776,380)**, history retains the same two records (Clear **14824**, Draw
**14835**, primitive **172**), but its final modification now agrees with both
selected-event readbacks. The default path reports this disagreement explicitly.
The verified stored color is
`(0.0036678314208984375, 0.004878997802734375, 0.00762176513671875, 1)`.
Other history events/intermediate fragments remain outside this acceptance.

The selected PS invocation provides **278** states and three stable GPU target
observations with refresh enabled. Its simulated return is approximately
`(0.0035753467, 0.0049502933, 0.0084888386, 1)`, which still differs from the
stored target and the history shader-output observation. This is an unresolved
shader-debug discrepancy, not a successful invocation-fidelity comparison.
Saved-report UI checks verify the history-to-debug event/primitive/refresh
handoff, export, and preservation of this difference; older D32S8 reports remain
accepted. No full portable-package or broad sample acceptance was performed.

The complete 18-patch runtime build passes; runtime SHA-256
`dc9f875ba2327a732673b331570cbba0a46b6e19d1e4623a1809102030d478f8`,
extension SHA-256
`053cb92ef9beed0cf0c43f77c2e94fffa06fbb979e1250ea2c3970f119bb8720`.
Final application/history UI build **405** passes. The three focused saved-report
UI checks report **5 passed** including setup/cleanup (**1,602 ms**), covering
the new Talos path and existing Hellblade2 D32S8 history/debug reports. Screenshots
are in `talos-depth-state-ui1`. Both PowerShell build/package scripts parse
successfully. GPU comparisons were serial and limited to this depth fault.

### Correct Cube directions in DX12 shader-debug sampling

The Talos PS discrepancy above exposed an error in RenderDoc 1.45's
`D3D12ShaderDebug::QueueSampleGather`: it cleared coordinate **z** for a non-array
Cube resource while adapting the resource to the debug shader's
`TextureCubeArray`. That shader (`data/hlsl/shaderdebug.hlsl`) uses **xyz** for
direction and **w** for the array index, including its SampleLevel path. The
nineteenth patch, `dx12-cube-coordinates.patch`, preserves xyz and clears w.
The DX12 helper is shared by DXBC/DXIL debugging; array textures and other
dimensions retain their existing coordinate handling. The similar D3D11 code
is unchanged and was not exercised in this DX12 fix.

For execution **14835**, pixel **(1776,380)**, primitive **172**, instruction
**239** samples t2 with direction approximately **(-0.28330356, 0.24964188, 1)**.
Before the fix it sampled with z=0. The new trace has identical inputs, constants,
instruction metadata and states **0–239**; the first changed state is **240**,
immediately after this sample. Its sampled float4 changes from approximately
`(0.52208745, 0.65465784, 0.90791178, 0.53624779)` to
`(0.59909970, 0.72188908, 0.91151291, 0.53121233)`.

The resulting shader return is
`(0.0036691678687930107, 0.004881807137280703, 0.00762186199426651, 1)`.
Compared with the independently observed Pixel History GPU shader output, the
float32 ULP distances are **(1,1,0,0)**. Truncating these positive normal values
to binary16 reproduces all four native stored components (**7043,7423,8142,15360**)
exactly. This is an observed conversion agreement for this invocation, not a
general rounding guarantee or bit-exact float32 interpreter claim. The selected
GPU target stays unchanged before/after debugging. This resolves the large Cube
sampling error in the preceding section while retaining its explicit depth-refresh
requirement and the broader incomplete debug/replay acceptance.

Evidence: ignored `talos-cube-debug1/patched`, `comparison.json`, and the prior
`talos-depth-state1` history/native references. Only one new GPU debug request
was needed for the final patch. The saved-report UI check verifies the original
instruction-239 breakpoint, sampled register Watch, final return, exported patch
version and the cross-source comparisons; **3 passed** including setup/cleanup
(**462 ms**). Screenshot: `talos-cube-debug-ui1/talos-cube-fixed.png`.
Builds **406/407**, both PowerShell syntax checks and the complete 19-patch
runtime verification/build pass. Reports expose
`backend_dx12_cube_coordinates_patch=1`; packaging checks the version/hash and
includes the patch source. No additional sample sweep or portable acceptance was run.
Runtime SHA-256:
`58b537fe40fc850dff2b0577e99366fe7a1f141fd316c959fbdfa1b3b861e034`;
patch SHA-256:
`9679916b73c7fd43641198b169dbdb78cab04f09e69b23ff0072b4e3283b9ce4`.

### Preserve typed DX12 input attributes and decode packed vertex formats

The VSIn reader previously converted every integer attribute through float32.
Values such as **16,777,217** and **4,294,967,295** therefore lost bits before
reaching the geometry table/export. The reader now expands integer inputs into
typed uint32/sint32 storage; the existing mesh model retains their exact numeric
values. Float/normalized inputs retain float32 storage, with original input and
index bytes exported separately. Integer OBJ exports identify their interpretation;
the legacy float OBJ text remains unchanged so saved reports remain readable.

The same path now decodes **R10G10B10A2_UNORM/UINT** and **R11G11B10_FLOAT**,
and aligns APPEND_ALIGNED_ELEMENT offsets to the element's required 1/2/4-byte
boundary. Unsupported format interpretations and truncated elements fail explicitly.
The format/alignment rules follow Microsoft's
[DX12 format support table](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/hardware-support-for-direct3d-12-0-formats),
[input element description](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_input_element_desc),
and [element alignment specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#4.4.6%20Element%20Alignment).

Focused evidence (`input-attributes1`, GPU fixture source
`tests/fixtures/dx12-input-attributes.cpp`): one native Draw of three vertices
and two instances writes its IA-fetched inputs directly from VS to a UAV.
Eight attribute readbacks compare **96 uint32 storage words exactly** against
that native GPU output, including large unsigned/signed integers, both packed
formats, R8/R16 input, and the second instance's uint4. The fixture explicitly
constructs missing components for its scalar and float3 comparison vectors;
typed default components also have a separate CPU check. The R16 attribute after
R8 resolves to offset **46**, skipping padding byte **0x99** at offset 45. No
GPA module is loaded. This fixture establishes the selected format conversions,
not full geometry acceptance for all three captures.

The saved-report workspace check verifies the eight typed tables, exact CSV/raw
exports, selected-instance VS-debug handoff, and stale-result rejection. It reports
**3 passed** including setup/cleanup (**248 ms**); screenshot
`input-attributes-ui1/replay-mesh/integer-input.png`. The focused decoder checks
report **4 passed** including setup/cleanup (**1 ms**), including sign extension,
normalized values, packed subnormal/infinity/NaN, truncation and alignment overflow.
The existing mesh-model executable passes. Builds **408/409** pass.

One Talos compatibility comparison at execution **14835**, VSIn attribute **0**,
retains exact original vertex bytes (**5,448**), index bytes (**1,272**), expanded
vertices (**7,264**), remapped indices (**2,544**) and CSV/OBJ exports. Expanded
vertex SHA-256: `79050ace6ec4c59f29f3b3ca70246182fc51100d85b39d372f6b35d849b78b0a`.
Evidence: `talos-input-layout1`, `talos-input-layout2/comparison.json`.
The indexed Talos frame has no Dispatch actions, so this work does not claim a
Talos CS invocation validation. Historical query results, complete geometry/stage
coverage and portable acceptance remain unfinished.

### Match packed/aligned VS debug inputs to native IA values

Following the typed VSIn work, the same fixture exposed **three wrong components**
in RenderDoc's DXBC VS input reconstruction at vertex ordinal **2**, instance **1**:
R10G10B10A2_UINT was `(1023,511,3,2)` instead of native `(3,511,1023,2)`,
and the appended R16_UINT was **64921** instead of **65533**. The debugger used
the low ten packed bits as B, swapped R/B, and read the R16 at byte **141**
instead of **142**, incorporating padding byte `0x99`.

The twentieth patch, `dx12-vertex-inputs.patch`, fixes both DXBC/DXIL UINT
unpacking and their shared input-layout expansion. Appended elements use the
required alignment, and packed formats advance by their complete element size.
The Flora input-snapshot reader now uses the same format-size/alignment helpers
as VSIn. Snapshots include format, layout offset and byte length; unsupported or
overflowing elements remain unavailable, and an explicit later offset can restore
known addressing. Before this change the three packed attributes were unavailable
and the R16 snapshot itself also read the padding byte.

Actual GPU validation uses `input-attributes1/native_capture.rdc` (DXBC) and
`input-attributes-dxil1/native_capture.rdc` (the same fixture shader compiled
with DXC 1.8.2505.1, `-T vs_6_0 -E main -O3`). Each fixed trace has **25 declared
attribute components**, all bit-exact against that fixture's independent native
VS-to-UAV output. DXBC has **24** states; DXIL has **63**. All eight attribute
byte ranges match the independently exported VSIn source bytes at each of the
three observation points. Both reports mark those observed bytes unchanged;
this does not prove stability during execution or validate every instruction/UAV
side effect. Source mappings are not fabricated when PDB data is absent.

Evidence: `input-attributes1/debug-before`, `debug-after`, `debug-comparison.json`,
and `input-attributes-dxil1/debug-after`. One existing DXBC capture was queried
before/after; one additional DXIL fixture Draw and debug query covered the second
decoder. No full-sample GPU sweep was run, and no GPA module was loaded.
The saved-report UI check covers both traces, native input comparisons, geometry
byte correspondence, invocation display and exported patch/snapshot metadata:
**3 passed** including setup/cleanup (**399 ms**). The initial UI check used the
wrong widget name and failed; after correcting the test it passed. Screenshots:
`vertex-input-debug-ui1/replay-mesh`. Focused snapshot CPU checks pass (**4**
including setup/cleanup, **1 ms**), including unknown append dependencies and
recovery at explicit offsets. Builds **410–412** and PowerShell syntax checks pass.

The complete 20-patch source verification/build passes. Reports expose
`backend_dx12_vertex_inputs_patch=1`; package manifests verify its version/hash
and include the patch. Runtime SHA-256:
`5e36f05cc59d27e9e57c300b50f10128b76c6489995795dea215581836ed90b4`;
patch SHA-256:
`1575891a7f386670d996919411a5fc25d1b6f058b3a46ea203b3eb16cd417c6e`.
Primitive restart, broader geometry/debug fidelity, historical query recovery
and full portable acceptance remain unfinished.

### Preserve Triangle Strip restart boundaries in geometry and VS navigation

DX12 VSIn now retains strip-cut entries as explicit sentinels while remapping
vertex indices. Cuts are identified **before** adding BaseVertex and only for
strip topologies; they do not expand the vertex range or acquire a VS invocation.
Original index ordinals remain intact. The geometry model resets Triangle Strip
winding at each cut and excludes cross-boundary/incomplete triangles. An all-cut
Draw has zero vertex bytes and never calls RenderDoc's zero-length buffer read
(which would request the whole buffer). Original index bytes remain exportable.
This follows the captured
[DX12 strip-cut setting](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_index_buffer_strip_cut_value).

VSOut required an additional correction: RenderDoc 1.45 preserves the original
cut values in its rebased indices, while its returned MeshFormat has a generic
`0xffffffff` restart default. On the selected 16-bit fixture that caused **7**
candidate faces and reversed winding for the three in-range faces. The adapter
now takes VSOut's restart enable/value from the selected Draw's input-assembly
state. The fixed output has **3** correctly wound triangles and no extra faces.

Native evidence (`strip-restart1`, source `tests/fixtures/dx12-strip-restart.cpp`):
one 16-bit and one 32-bit indexed Draw use StartIndex **1**, BaseVertex **-5**,
leading/consecutive/trailing cut markers, and two nonempty strips. A native GS
writes each assembled triangle's SV_VertexID values to a UAV. Both VSIn exports
and the fixed 16-bit VSOut match all three native triangles, comparing cyclic
vertex rotations while preserving winding. They contain **7** vertex rows,
**12** original index references and **5** cuts. A third Draw with three cuts
only produces no GPU triangles, no vertex rows and an empty vertex export,
while retaining its six original index bytes. No GPA module is loaded.

The first references are **(1,2,3,4,7,8,9)**. Selecting row **4** in both VSIn
and VSOut emits original ordinal **7**. One actual VS debug request confirms
raw index **9**, buffer row **4**, input byte offset **64** and SV_VertexID **9**;
the raw system value is distinct from the BaseVertex-adjusted buffer index.
The saved-report UI check validates native triangle membership/winding, counts,
OBJ export, invocation navigation and disabled debugging for the empty result:
**3 passed** including setup/cleanup (**272 ms**). Screenshot:
`strip-restart-ui1/replay-mesh/strip-restart.png`. The mesh-model executable also
passes focused 16/32-bit, disabled-restart, list-topology and all-cut checks.
Builds **413–416** pass. GPU work was serial and confined to these cases.

This closes the demonstrated Triangle Strip restart path; it does not establish
full geometry/GS analysis, adjacency topology rendering or complete sample
fidelity. Historical query recovery and portable acceptance remain unfinished.

### BF1 main-color synchronization experiment and workspace controls

The existing four-barrier particle experiment was extended to main-color producer
execution **36758**, resource creation **324859** (1920 x 1009, R11G11B10).
All requested barriers before **35713,35732,35751,35770** were recorded and
GPU-completed. Native readback and a separate texture query of that recapture
match all **7,749,120 bytes**, SHA-256
`821f6dd581a275fa3d882538df42e34dfd577c4630d155afcc71541aa18ab948`.
However, this still differs from original GPA `gpa-bf1-main-raw2` at **95,980**
packed pixels, within **(1329,394)–(1919,865)**. The selected pixel (1674,485)
also differs. This is a verified native/analysis boundary for an explicit
synchronization modification, **not** restored original-frame fidelity.

An unmodified recapture queried with the current 20-patch runtime differed from
the same original data at **41,781** pixels; enabling diagnostic discard fills
in a separate query yielded the above `821f6d…` result. The default query's
skipped-fill counter was **zero**, so this comparison does not establish that
discard policy caused the difference. The latter hash also occurred in an older
unmodified query. No improvement is attributed to a specific patch, and no
repeatability or full-frame acceptance is claimed. Evidence:
`bf1-producer-runtime20`, `bf1-producer-runtime20-diagnostic`,
`bf1-producer-barrier20`, `bf1-producer-barrier20-analysis`, with compact
`comparison.json` files. The four GPU requests were serial and targeted this
existing discrepancy; no sample sweep was performed.

The Edit menu now exposes **Insert DX12 UAV Barrier Before Event** and
**Remove Inserted DX12 UAV Barrier** for eligible submitted Draw/Dispatch
instances. The operation uses the existing capture-bound project and native
barrier implementation. It preserves other experiment edits, supports undo/redo
and save/load, invalidates prior analysis/output, and retains asynchronous
capture verification, cancellation and session checks. The tooltip explains
that ordering and timing change, including when the target draw is disabled.
CPU-record selections, query isolation and busy workers cannot insert a barrier.
This supersedes the earlier CLI-only workspace limitation; it does not silently
alter original replay or repair synchronization automatically.

Build **417** passes for the workspace, worker and targeted test executable.
`dx12UavBarrierWorkspace` opens BF1 and checks insertion/removal, preservation of
a disabled draw, undo/redo and redo truncation, saved string execution identities,
reloading per-instance state, CPU-record/busy gating and rejection of stale VS
debug/mesh results. Automatic replay stays off and no GPU worker runs during this
UI check: **3 passed** including setup/cleanup, **17,277 ms**
(`uav-barrier-workspace1.txt`).

### Preserve half-float storage between Pixel History fragments

The BF1 four-barrier experiment at execution **36560**, RGBA16F creation
**325012**, pixel **(836,241)** now has a matching selected-event boundary with
the existing opt-in D32 depth roundtrip. However, its 23-row history exposed a
separate color problem: primitive **1334** outputs zero with One/InvSrcAlpha
blending, yet the previous history showed its RGB falling by one half-float step.
The final value came from the real event readback; intermediate fragments were
blended into RenderDoc's RGBA32 scratch image and then converted on the CPU.
This does not preserve the original target's per-fragment storage conversion or
the stored destination consumed by subsequent blends.

The native reproduction is `tests/fixtures/dx12-half-blend-history.cpp`.
It draws one, two and three overlapping triangles in separate prefix draws,
clearing to the same exactly representable initial color each time. The middle
triangle outputs zero. Each prefix has an independent native RGBA16F readback;
execution **3** supplies the three-fragment history under test. Native half words:

| Fragment/prefix | R | G | B | A |
| --- | --- | --- | --- | --- |
| 0 / one triangle | 10363 | 10247 | 9940 | 14653 |
| 1 / two triangles | 10363 | 10247 | 9940 | 14653 |
| 2 / three triangles | 10497 | 10325 | 10008 | 14955 |

The old history returned **10364,10248** for R/G of both first fragments;
its last fragment already matched. The new twenty-first patch,
`dx12-half-color-history.patch`, uses a separate RGBA16F GPU scratch attachment
for per-fragment blending and reads its raw stored values. Shader output still
uses the full-precision scratch target. All **12** history components now match
the independent native prefixes, including exact preservation across the zero
fragment. This establishes the demonstrated blend/storage path on this device;
it does not assume a universal CPU rounding mode.

In BF1, **14** intermediate post-modification records change. All 23 Shader
outputs and primitive identities remain identical. Primitive **1322** now stores
`(0.035003662109375,0.031463623046875,0.02667236328125,0.65478515625)`;
primitive **1334** preserves those values exactly. The selected texel before and
after history and the final event modification agree. The earlier native
readback has the same selected value. This does not independently validate every
BF1 intermediate fragment, test-failure reason, or original-frame fidelity.

The patch currently covers RGBA16_FLOAT 2D targets with one mip, layer and sample,
including typeless backing requested through that float interpretation. Other
formats and layouts retain backend reconstruction and are not included in this
acceptance. Reports identify `backend_dx12_half_color_history_patch=1` and explain
the scope under `history_scope.intermediate_color_storage`; unpatched libraries
explicitly report the reconstruction limitation. Build/package scripts verify
the new patch version/hash and include its source. Runtime SHA-256:
`7ca41e0e903269444ade4126faf9a5e19672cfe36299b501ae84cb6392a92344`;
patch SHA-256:
`93042733bd75dd655ccd8ece2d0b47a59f7e81807346f2559f7a76f2cd66a4ec`.

Evidence: `half-blend-history1/{gpu.json,before,after,comparison.json}` and
`bf1-particle-{roundtrip-history1,half-history1}`. One small native fixture capture
and four analysis queries were run serially for this fault. The successful
analysis module inventories contain no GPA/Python modules. Exact 21-patch source
verification/runtime build and PowerShell parsing pass. Worker/CLI build **419**
and UI-test build **420** pass; build 418's incorrect Session member reference
was corrected. The saved-report UI check verifies native prefix values, BF1
before/after display, event/primitive/depth-option handoff and exported evidence:
**3 passed** including setup/cleanup, **322 ms** (`half-color-history-ui1.txt`).
Its screenshot was inspected. Full portable acceptance remains unfinished.

### Pixel History mip/layer selection and attachment-range filtering

The half-blend reproduction now has `mip` and `array` modes. Both render into
Mip **1** of a 32 x 32 RGBA16F texture; the array mode uses Layer **1**, native
subresource **3**. The effective render size remains 16 x 16. Native prefix
readbacks retain the three sets of half words listed above.

Two defects were reproduced with the 21-patch runtime. For a non-array Mip 1
query, only **one** record was returned for the three-fragment execution **3**.
The scratch RTV and DSV left `Texture2D.MipSlice` at zero, while fragment-count
readback used Mip 1. The array query returned all three records but retained the
four wrong intermediate components because the original-format blend scratch
path excluded arrays/mips.

The twenty-second patch, `dx12-history-subresources.patch`, explicitly binds
the selected non-array mip in both scratch views and extends RGBA16F blend
storage to the selected mip/layer of single-sample 2D resources. The post-blend
RTV is explicit rather than a default view. Both final positive queries now
return three correctly identified fragments with all **12** components exactly
matching their independent native prefix readbacks.

A negative query exposed a separate attribution error: asking for **Layer 0**,
which none of the fixture draws bind, still produced all **six** fragments from
the three Draw calls targeting Layer 1. Even its final history/texel comparison
said they matched. The occlusion pass now rejects a Draw when all known views
of that resource exclude the requested mip/layer. MRT selection also finds the
overlapping view instead of merely the first view of the same resource. Unknown
view layouts remain candidates; UAV writes and resource operations are not
discarded by this attachment check. Final negative queries for Mip 1/Layer 0
and Mip 0/Layer 1 contain **zero Draw fragments**. Three backend resource-level
Clear records remain unmapped in this synthetic capture; their presence is not
evidence of modification to the queried subresource.
Per-primitive array routing, MSAA, 3D targets and other format storage fidelity
are outside this validation.

Reports accept both half-color patch versions and identify version **2** for
the extended runtime, with `history_scope.subresource_filter` describing its
limits. Build/package manifests verify and include the extension patch. Complete
22-patch source verification/build and worker/CLI builds **421/422** pass.
Runtime SHA-256:
`497eec5449f6f3358127ff2ae230baf8428137ec56832197d2c1638108544ff9`;
extension SHA-256:
`093a131d33bd9d9fc2488de84d29862a037cdfece267f3bc23ff0ca331b8c046`.

Evidence: `half-blend-mip1` and `half-blend-array1`, each containing native
`gpu.json`, failing `before` and verified `final` reports. The array directory
also retains `other-layer-before`, `other-layer-final` and `other-mip-final`.
The initial positive `after` reports precede the negative-control filtering fix;
final acceptance uses `final`. Saved-report checks verify native component
values, primitive identities, requested subresources and negative Draw exclusion:
**3 passed** including setup/cleanup, **4 ms** (`half-color-subresources1.txt`).
GPU checks were serial and restricted to these two tiny reproductions and the
specific subresource faults. An inspected BF1 multi-mip candidate, creation
**238058**, has only PS read uses in the selected capture prefix
(`bf1-mip-history-source1`); it is not claimed as a real-sample fragment proof.
Full sample and portable acceptance remain unfinished.

### Pixel History Clear view/rectangle coverage and resource operations

The three resource-level Clear records remaining in the negative array query
above are now filtered using the captured API arguments. RenderDoc 1.45's
`Serialise_ClearRenderTargetView` and `Serialise_ClearDepthStencilView` preserve
the descriptor at the call, including its resource and explicit view range,
plus `NumRects`/`pRects`. The analysis worker checks these fields against the
queried resource, mip, layer and pixel. This does not infer coverage from a
later pipeline binding or assign an unmapped operation to a nearby Draw.
Excluded operations retain their event provenance and coverage evidence in
`history_scope.excluded_resource_operations`.

The array negative query now returns **zero records**, with all three Clears
excluded by their Mip 1/Layer 1 views. Its positive counterpart retains three
Clears and six Draw fragments. The fixture's new `rect` mode adds a red Clear
over `[0,0,4,4)` before each Draw. Querying `(5,5)` retains nine records and
excludes those three local Clears; querying `(2,2)` retains all twelve records.
The native prefix half words at `(5,5)` remain identical to the earlier oracle.
Evidence: `half-blend-array1/{clear-excluded1,clear-covered1}` and
`half-blend-rect1/{gpu.json,native_capture.rdc,outside,inside}`.

BF1's particle history at execution **36560**, resource creation **325012**,
pixel **(836,241)** still has 23 records. The captured Clear at execution
**36391** has confirmed view coverage. All 22 remaining fragment primitive IDs,
Shader outputs and post-modification values exactly match
`bf1-particle-half-history1`; the new report is `bf1-particle-clear-history1`.
The query retains the explicit experimental depth-refresh option and the
earlier capture's UAV ordering experiment; this is not full-frame acceptance.

Clear/Copy/Resolve rows are explicitly resource operations with null fragment
index, primitive ID and Shader output. The UI shows observed value change
separately from coverage, permits mapped event navigation and disables PS
debugging for these rows. An unchanged value does not mean that no write occurred.
Implicit views, unsupported Clear commands/dimensions and malformed or missing
arguments retain an explicit coverage gap and mark the history partial.
Copy/Resolve destination-region coverage is still unimplemented and reported
as a gap. Current support covers explicit 2D RTV/DSV views, including array and
multisample view layouts; this turn's GPU validation covers single-sample RTVs.

Worker/CLI and app/UI-test builds **423–425** pass. The focused coverage test
checks signed rectangle edges, malformed arrays, mip/layer ranges, DSV fields
and a resource identity above 2^53: **3 passed**, including setup/cleanup,
**1 ms** (`clear-coverage-unit1.txt`). Saved-report UI verification checks
negative coverage, resource-operation labeling, the exact BF1 Clear navigation,
PS-debug eligibility and export: **3 passed**, **103 ms**
(`clear-history-ui1.txt`). GPU checks were serial: one tiny fixture capture,
four focused fixture history queries and one BF1 history query. No runtime
patch or new GPA/Python dependency was added. Full acceptance remains unfinished.

### Pixel History Copy/Resolve coverage and regional Resolve values

The worker now reconstructs destination coverage for `CopyResource`,
`CopyTextureRegion`, `ResolveSubresource` and `ResolveSubresourceRegion` from
their captured structured arguments. Region transfers decode the destination
subresource, source box/rectangle, destination offsets and source dimensions;
buffer sources use the placed footprint rather than a texture index. Resource
identities remain decimal strings. Whole-resource copies cover all subresources.
Region support is currently limited to regular single-plane 2D color textures;
unsupported formats, invalid ranges and missing source metadata remain explicit
coverage gaps. This does not claim support for compressed-coordinate conversion,
depth/stencil planes, 3D transfers or every special Resolve mode.

Two small GPU reproductions extend the existing half-blend fixture. `copy`
copies source box `[2,1,6,5)` into destination `[4,4,8,8)` of Mip 1/Layer 1.
`resolve` resolves a 4x MSAA source rectangle into that same destination region.
Both positive queries at `(5,5)` retain three transfers, three Clears and six
Draw fragments. Negative queries at `(2,2)` exclude the three transfers and
retain nine records. The copy query for Layer 0 excludes six resource operations
and returns no records. Excluded operations preserve their exact execution
provenance rather than becoming fragments of an adjacent Draw.

The Resolve reproduction also exposed an upstream 1.45 fault independent of
coverage: `Serialise_ResolveSubresourceRegion` did not invoke the active replay
analysis callback. Pixel History therefore returned its initialization sentinels
`0xDEADBEEF`/`0xDEADF00D` as before/after color, even though the following Draw's
final value matched the selected texel. The twenty-third patch,
`dx12-resolve-history.patch`, dispatches the same Pre/PostMisc and optional
repeat callbacks used by full-subresource Resolve. It exports
`RENDERDOC_GetFloraDx12ResolveHistoryPatchVersion=1`.

With the patch, all three regional Resolves report the initial Clear color
before the operation and `(1,0,0,1)` afterward. A second native capture reads
the destination immediately after each Resolve, before the Draw; all **12**
components agree exactly with the native half words `[15360,0,0,15360]`.
The selected last Resolve's history boundary also matches direct texel readback.
For both Copy and Resolve, the three fragments of execution 3 match all **12**
native prefix components. On an unpatched runtime, regional Resolve values are
now null, with an explicit value gap, partial history, no numeric UI color and
no boundary comparison. Uninitialized values are never interpreted as an
unchanged pixel. These reproductions validate the analysis paths, not complete
BF1/Talos/Hellblade2 fidelity.

Evidence: `half-blend-copy1/{gpu.json,inside,outside,other-layer}`;
`half-blend-resolve1/{gpu.json,inside,outside,patched,unpatched-guard}`;
`half-blend-resolve2/{gpu.json,transfer}`. Resolve1 `inside` is the failing
22-patch result; `patched` is the same capture with callback recovery. Resolve2
adds the independent immediate readbacks. GPU work was serial and limited to
three tiny captures and eight targeted history queries for these faults.

The complete 23-patch source verification/build passes. Runtime SHA-256:
`15a7506f7b2c0e77d6ef22481263f7feec8f763ec8fb5a272b1dbe6e4e2d1f2f`;
patch SHA-256:
`b441aea780102c0a45a2f1eb660e9a6c7238aecb8beeecc85e44b6b2176a4b3e`.
Build/package scripts verify the manifest and ship the new patch source;
PowerShell syntax checks pass. App/worker/CLI/test builds **426–429** pass.
Focused transfer-field tests cover offsets, edge exclusion, layers, overflow,
null source boxes, footprints, source metadata, full Resolve extents and 64-bit
identities: **3 passed**, including setup/cleanup, **1 ms**
(`transfer-coverage-unit1.txt`). The saved-report UI check verifies native values,
exact transfer navigation, PS-debug exclusion, unpatched-runtime presentation
and export: **3 passed**, **33 ms** (`transfer-history-ui1.txt`).
Full sample and portable acceptance remain unfinished.

### Resource History operation ranges

Resource History now reads captured operation arguments instead of leaving every
Copy/Clear range unknown. Rows expose `operation_ranges`, distinguishing source
and destination roles, whole-resource copies, buffer byte intervals, texture
subresource indices and buffer footprints. CopyTextureRegion preserves its source
box and destination origin; Resolve preserves native subresources and optional
source rectangles/mode. Texture metadata decodes mip/layer/plane indices where
available. Explicit 2D Clear views reuse the captured descriptor-range validation.
Resource identities, buffer offsets, lengths and interval ends remain decimal
strings, including values above 2^53. Overflow or malformed fields produce an
unavailable range rather than a partial valid-looking list.

These are native API ranges, not measured content modifications. In particular,
compressed copy coordinate units and planar formats are not converted into
guessed texel coverage. A footprint's pitch and offset do not imply a contiguous
written byte interval. Draw/Dispatch bindings still describe potential access,
and existing captured barriers/discards retain their separate annotations.
The UI shows the role/range, preserves exact execution navigation and exports
the original evidence. Unknown or implicit Clear views retain a visible gap.

The existing copy fixture now names its target, source and readback resources
with distinct original object/creation identities. In
`half-blend-operation-ranges1`, three independent resource queries through Draw
execution 3 produce:

| Resource | Indexed uses | Recovered operation ranges | Range gaps |
| --- | ---: | ---: | ---: |
| Target 1001 / creation 1 | 30 | 11 | 0 |
| Source 1002 / creation 2 | 5 | 3 | 1 |
| Readback 1003 / creation 3 | 5 | 5 | 0 |

Copy execution **11** names target native subresource **3** (Mip 1/Layer 1,
plane 0) and source subresource **0** (Mip 0/Layer 0). The source box remains
`[2,6) x [1,5) x [0,1)`, with destination origin `(4,4,0)`. Readback uses carry
256-byte row pitches and their distinct destination offsets. Their five
unmapped native copy actions remain unmapped; none is assigned to a nearby
Draw. The source's default-view Clear is the one explicit range gap.

BF1 creation **325012**, object **10432990048**, through execution **36560** has
16 indexed uses. Its Clear execution **36391** now reports Mip 0, first Layer 0,
one layer and the complete view. All uses continue to report
`content_change: not_measured`. Evidence is
`bf1-particle-operation-ranges1/result.json`. All four queries use explicit
resource identities and the existing usage index, without replaying the selected
event; capture loading can still execute GPU work. Their module inventories
contain no GPA/Python modules.

App/worker/CLI/test builds **430–433** pass. Focused field tests cover source and
destination roles, same-resource buffer copies, >2^53 offsets, overflow,
texture indices and Clear view ranges: **5 passed**, including setup/cleanup,
**1 ms** (`operation-ranges-unit1.txt`). The saved-report UI check verifies
source/destination presentation, readback footprints, the default-view gap,
exact execution navigation, BF1 Clear display and exported evidence:
**3 passed**, **79 ms** (`resource-ranges-ui1.txt`). This turn used one tiny
capture and four serial resource-index queries. Full resource lifetime,
synchronization history and observed-content coverage remain unfinished.

### Default RTV/DSV ranges and wrong-mip Draw attribution

The default-view Clear gap found in the preceding source-resource query is now
resolved for typed 2D textures. RenderDoc 1.45 `D3D12Descriptor::Init` stores a
NULL RTV/DSV creation description as a zeroed descriptor, and its serializer
explicitly identifies the UNKNOWN view dimension as this representation.
This is distinct from missing structured fields. Microsoft's
[CreateRenderTargetView](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createrendertargetview)
and [CreateDepthStencilView](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createdepthstencilview)
contracts define default views as the first mip and all array slices of a
compatible typed resource. The worker checks the zero format/dimension, DSV flags
and resource type before applying that rule. Malformed descriptions and typeless
resources remain unavailable. Reports distinguish `default_resource_view` from
`explicit_descriptor`; Resource History labels the default view.

The small fixture's `default` mode uses a 32x32 RGBA16F texture with two mips and
two layers, bound through a NULL RTV description. Its native three-prefix oracle
is unchanged. Querying Mip 1 with the 23-patch runtime reproduced **six unrelated
Draw fragments**, despite the new worker correctly excluding the three default
Clears. The twenty-fourth patch, `dx12-default-view-history.patch`, applies the
same default-view range to the runtime's RTV/DSV attachment overlap check. The
Mip 1 query now returns **zero records**. The positive Mip 0/Layer 0 query retains
three Clears and six fragments, and all **12** components of execution 3 agree
exactly with native prefix readbacks. A query immediately after the first Clear
at Layer 1 confirms the two-layer default view and agrees with the selected
replay texel; this is not a claim about per-primitive Draw routing between layers.

Evidence: `half-blend-default1/{gpu.json,other-mip-before,other-mip-after,positive,
clear-layer1}`. Re-querying the previous source resource in
`half-blend-operation-ranges1/default-source` preserves five indexed uses, raises
recovered operation ranges from three to four, and removes its one default-view
gap. No new original-sample fidelity claim is made by these reproductions.

Runtime reports expose `backend_dx12_default_view_history_patch=1`; unpatched
runtimes explicitly describe the default-attachment filtering limitation.
Complete 24-patch source verification/build passes. Runtime SHA-256:
`07c7bfdff028c00b96ac4a95a23600d055f051458f96705c14530a0b94e951fd`;
patch SHA-256:
`d96568cfd45ccf1ea20d0a74344ae7a7509717f002ef97e1a63c90011b167353`.
Build/package manifests verify and include the new source patch; PowerShell
syntax checks pass. App/worker/CLI/test builds **434–436** pass. The focused RTV/DSV
coverage test checks both default ranges, malformed descriptions and typeless
rejection: **3 passed**, including setup/cleanup, **1 ms**
(`default-clear-unit1.txt`). Saved-report UI verification checks wrong-mip
exclusion, native values, Layer 1 Clear evidence, default-range display and
export: **3 passed**, **35 ms** (`default-view-ui1.txt`). Work used one tiny
capture and five serial targeted queries. Interruptions were resumed from
completed output files without repeating GPU queries. Full sample/portable
acceptance and general per-primitive array routing remain unfinished.

### Fixed-layer Draw attribution and remaining GS routing fault

The twenty-fifth runtime patch, `dx12-fixed-layer-history.patch`, inspects the
last active VS/DS/GS output signature. Without `SV_RenderTargetArrayIndex`, a
Draw targets the attachment view's first layer. The attachment-overlap filter
now rejects other layers for default, explicit 2D-array and MS-array views.
Missing reflection, mesh shaders and view instancing remain candidates. Runtime
reports expose `backend_dx12_default_view_history_patch=2` and state this limited
routing guarantee. The build/package manifest pins the additional patch.

Evidence under `half-blend-default1/{other-layer-before,other-layer-after}`:
querying Mip 0/Layer 1 previously returned three Clears and six unrelated Draw
fragments; it now returns only the three Clears. The former selected-texel
boundary check passed despite incorrect fragment attribution, demonstrating why
that check alone cannot validate a history. The explicit view starting at Layer 1
in `half-blend-array1/fixed-layer-positive` retains all nine records and all 12
selected-Draw color components match native prefix readbacks. The saved-report
UI test checks Clear-only display, disabled PS debugging and the positive native
comparison: **3 passed**, including setup/cleanup, **241 ms**
(`fixed-layer-ui1.txt`). App/CLI/test builds **437–438** pass.

The new `routed` fixture mode uses a GS that writes array index 1. Its separate
native-only run (`FLORA_FIXTURE_NATIVE`) and captured run have identical three
prefix readbacks. The fixture passes its primitive color selector through a
custom semantic; it does not emit a GS primitive-ID system output. Optional
`FLORA_FIXTURE_DEBUG` enables D3D12 diagnostics. A prior fixture linkage error was
resolved from native debug messages, not classified as a hardware limitation.
`half-blend-routed1/selected-layer` returns the expected nine records and matches
all 12 native selected-Draw components, but primitive IDs remain unavailable.
`half-blend-routed1/other-layer` still falsely attributes six Draw fragments to
Layer 0. This reproduces an **unfinished dynamic-routing fault** in analysis
scratch attachments/counters. Neither successful values at Layer 1 nor an
unchanged Layer 0 boundary validates those other-layer fragments. General array
routing and the original-sample/full portable acceptance remain unfinished.

Complete 25-patch source verification/build passes. Runtime SHA-256:
`bee99c3d2ce3c5ed8995f26d193ca2bad5507e4e52bd8d5afa0e31ce9217b6e8`;
patch SHA-256:
`23166f91105c30841e35849a506602b3f10f50f806543838cc6a70d4b41223b2`.
The local runtime checkout required a process-scoped Git safe-directory setting
after an ownership change; its pinned upstream commit was rechecked, and no
global Git trust setting was changed.

### GS array routing: selected-layer coverage and fragment reconstruction

The dynamic-routing fault reproduced above is repaired for the tested 2D GS
array paths by the twenty-sixth patch, `dx12-routed-layer-history.patch`.
Previously, scratch RTV/DSV views exposed only the requested slice. This changed
`SV_RenderTargetArrayIndex` routing, and a whole-Draw occlusion query could count
primitives from another layer. Each Draw now reconstructs its original first
slice/count in the scratch views. Coverage uses a scratch stencil mask: clear
the bound range to zero, clear only the selected physical layer to one, then
query with EQUAL/KEEP and reference one. Original capture resources are not used
for this mask. Per-fragment stencil counts and color readbacks retain the selected
physical slice while geometry keeps its original view-relative index.

`half-blend-routed1/other-layer-masked` removes all six unrelated Draw fragments
and retains the three actual Clears. `selected-layer-masked` preserves nine
records, with all 12 selected-Draw color components matching native prefixes.
The explicit single-slice view starting at Layer 1 remains correct in
`half-blend-array1/routed-runtime-positive` (nine records, 12 exact components).

The new `split0`/`split1` modes make the same GS route primitives 0/1 to Layer 1
and primitive 2 to Layer 0. Primitive 1 outputs zero RGBA; it is a real fragment
whose blend does not change the target, not a discarded invocation. A single
capture plus an independent native Layer 0 prefix run provides the oracles:
`half-blend-split1/{gpu.json,native-layer0.json,native_capture.rdc}`. The Layer 0
query returns three Clears and one execution-3 fragment; Layer 1 returns three
Clears and five Draw fragments, including exactly two at execution 3. All four
Layer 0 and eight Layer 1 selected-Draw components match their corresponding
native prefixes. This checks separation within a Draw, not only whole-Draw
exclusion. Evidence is in `half-blend-split1/{layer0,layer1}`.

One targeted BF1 single-layer regression, `bf1-particle-routed-history1`, retains
all 23 records from `bf1-particle-clear-history1`. Event/fragment/primitive IDs,
pre/post/shader colors and recorded culling/depth/stencil/discard fields are
unchanged. Its selected texel is stable and the final boundary matches. This
checks the mask's effect on the existing query; it does not resolve BF1's original
frame fidelity or concurrency gaps. Saved-report UI checks for layer-specific
row counts, native values, execution navigation, unavailable GS primitive IDs
and JSON export pass: **3 passed**, **45 ms** (`routed-layer-ui1.txt`). After
adding the partial-result contract, the focused UI check passes again: **3
passed**, **48 ms** (`routed-layer-ui2.txt`).

Runtime reports expose `backend_dx12_routed_layer_history_patch=1`, and the build
and package manifests pin/include the new patch. The complete 26-patch source
verification/build and application/CLI/UI builds **439–441** pass. Final runtime
SHA-256: `08a5e40359277ff7cdef52e400c30a16edc844d56272afe81012e3ba94d844a8`;
patch SHA-256: `fc48eb9b9893ac659f9c61e584045688df45ccae681f2f2855e5d8a29e508e01`.
The first five fixture queries used the same implementation immediately before
its version export was added; BF1 checked the final versioned runtime. One later
`half-blend-split1/layer1-scoped` query verifies the new partial/unverified report
contract and preserves all eight prior history rows. No fixture queries were
repeated merely for the export addition.

**Still unfinished:** per-layer test-failure queries can aggregate other layers;
coverage/color agreement does not validate those reasons. Array texture reports
are explicitly partial and label this reason status unverified. GS primitive-ID
recovery, mesh/view-instancing, general MSAA and depth-array routing remain
unverified. No original-sample/full portable acceptance item is promoted by
these fixtures. GPU work for this repair was one new tiny capture, one native
prefix run, six focused fixture queries and one existing BF1 query, serialized.

### Selected-layer failure queries: culling, discard and depth controls

The twenty-seventh runtime patch, `dx12-layer-test-history.patch`, addresses a
concrete cross-layer false pass in the remaining test-reason path. The fixture's
`cull` mode routes primitives 0/1 to Layer 1 with reversed winding and primitive 2
to Layer 0 with front-facing winding. Layer 1's three native prefix readbacks all
retain the Clear value. Before this patch, executions 1/2 reported backface
culling, but execution 3 reported no culling because the occlusion query counted
the passing primitive in Layer 0. Evidence:
`half-blend-layer-cull1/{gpu.json,before}`.

For single-sample 2D array targets, fixed-shader test passes now write a nonzero
marker to a scratch FLOAT4 RTV with the original array range, with blending
disabled. Each test copies only the selected physical layer's pixel to a separate
readback buffer; zero/nonzero replaces the whole-Draw occlusion result. Original
DSV tests remain active when applicable, with depth/stencil writes suppressed.
Other captured color attachments are write-masked. Original-PS discard queries
use the preceding patch's selected-layer stencil mask, so zero-color output is
not mistaken for discard. The query pool accounts for all eight possible tests,
and PSO cache keys include the tested output slot. Probe storage is separate from
the pre/post-event buffer and scales to at most eight 512-byte slots per Draw.

`half-blend-layer-cull1/{after,final}` now reports culling for all three relevant
Draws and preserves native Clear colors. `passing-layer` returns the three Clears
and the actual execution-3 fragment with culling false. The `discard` fixture
makes only Layer 1 discard; its five backend fragment records remain discarded
and match unchanged native colors (`half-blend-layer-discard1/{gpu.json,before,
after}`). This is a discard-path control, not a claim that the pre-patch final
records were wrong: the existing fragment reconstruction already identified
these discards. The new depth mode binds a two-layer D32 DSV, clears depth to
0.25, and outputs depth 0.75 on Layer 1 versus 0.125 on Layer 0.
`half-blend-layer-depth1/rejected` reports depth failure for all five Layer 1
fragments with native unchanged colors and depth 0.25. `passing-layer` preserves
the actual Layer 0 Draw, passing depth 0.125 and the exact native single-fragment
color from the matching split-layer oracle. This verifies that scratch markers
do not replace the original depth test.

Reports expose `backend_dx12_layer_test_history_patch=1` and actual probe counts.
The final culling query performs nine fixed-shader and three original-PS discard
queries with zero probe failures; the passing-depth query performs three and one
respectively. Successful eligible queries report `backend_selected_layer_probes`;
allocation/readback failures are counted and reported as `probe_failed`, and
unsupported/unexecuted paths remain `unverified`. Array histories remain partial.
The UI shows the test source and remaining limits next to the result summary.
The saved-report UI test verifies the formerly false culling result, all three
failure controls against native colors, passing-layer behavior, execution
navigation and export: **3 passed**, **51 ms** (`layer-test-ui1.txt`).

Complete 27-patch source verification/build, application/worker/CLI/UI-test build
**442**, and build/package PowerShell syntax checks pass. Runtime SHA-256:
`147bb78f64656ec9be4c9f5932c4ae830d9e77f8e4a36c136695b5a0d97c65a6`;
patch SHA-256:
`e644526694c22ab9ed26bf54f4afd1281966368b443e2662b3a297045e210ad7`.
The fixtures before the final stats export used the same probe implementation;
the final culling and passing-depth queries verify the versioned/reporting path.
Testing was limited to three tiny captures and eight serial queries serving the
reproduced fault, depth/discard controls and report contract. No full-sample or
portable acceptance was promoted. Early depth/stencil semantics, mixed
per-fragment failures, MSAA, full depth reconstruction, mesh/view instancing and
GS primitive-ID recovery remain unfinished.

### Original BF1 particle-storage boundary and first input extraction attempts

A new original-player raw readback narrows the main-color mismatch to an earlier
particle attachment. The selected submitted Draw is execution **36560**, source
record **255931**, list **0x2af247e70**, zero-based Draw/Dispatch ordinal **13**.
The source ordering was checked against the saved submitted-command index; the
submission's list ordinal **75** is not a Draw ordinal and was not used as one.
The target is object **0x26ddacb60**, creation **325012**, RTV 0, mip/layer 0.

Using the previously identified dump-layer conversion wrapper (RVA **0x15ad0**),
a bounded debugger launched its own GPA child and copied the selected
`DirectX::Image` before PNG conversion. It restored the original instruction and
continued; GPA exited **0**. No installed binary or capture was edited. The same
run produced `f_0_cl_2af247e70_dc_13_rtv_0_r_26ddacb60_i_0.png`. The checked source
was **960 x 504**, DXGI **10** (RGBA16_FLOAT), row pitch **7680**, and **3,870,720**
bytes. This is the RTV interpretation of the typeless native resource, not a
preview image used as a GPU oracle. Raw SHA-256:
`efd68b29828a47ced97da6d5bef5d9986af7e663f9da68071029891a08d1a16d`.

Comparison against the saved native particle boundary and the explicit
four-barrier experiment (both `a1cec82b979735c74db4dd23e7b41bcd189128ae376bbcd6fae0abfc029b7138`)
finds **26,473** differing packed pixels, bounded by **(668,197)–(959,432)**.
Per-component differing-pixel counts are **26,242 / 26,185 / 26,048 / 0** for
RGBA. All Alpha storage words and nonzero-Alpha coverage are identical; only RGB
differs. Maximum absolute RGB differences are **0.00433349609375 /
0.00311279296875 / 0.001983642578125**. At (836,241):

| Source | R | G | B | A |
| --- | --- | --- | --- | --- |
| Original GPA | 0.037872314453125 | 0.03363037109375 | 0.0281219482421875 | 0.65478515625 |
| Saved native/barrier output | 0.035003662109375 | 0.031463623046875 | 0.02667236328125 | 0.65478515625 |

These values are exactly the two previously observed debug references. The new
original raw output therefore confirms a real intermediate RGB discrepancy; it
is not an Alpha display conversion or solely a later tone-map mismatch. Its
right-side location is consistent with the earlier main-color difference, but
this is not proof that it accounts for every downstream differing pixel. Equal
Alpha alone does not prove all geometry/depth behavior is identical. Separate
original replay runs can vary, as already documented; neither output is declared
the unique deterministic result of the capture.

At this milestone, an attempted earlier input boundary remained unavailable. The original dump layer
was asked for shadow UAV object **0x20664cf30**, creation **300425**, after source
**265610** / execution **35770**, list **0x27b022210**, work ordinal **17**. It
exited 0 without a buffer file. Removing the resource/view filter at that exact
Draw, then checking all buffer bindings at the known particle Draw, also produced
no buffer files. These are failed extraction attempts, not zero contents or
successful comparisons. Logs contain invalid-binding messages. A read-only
Ghidra cross-reference resolves that message to dump-layer RVA **0x8200**, which
logs it for a zero tracked binding type; buffer dispatch goes through **0xb640**.
The log is not tied to one resource, so it does not prove which target binding is
missing or explain the native/original shader-input difference. No barrier or
capture bytes were changed to force agreement.

Evidence: `gpa-bf1-particle-raw1/{probe.json,original-storage.bin,comparison.json,
player.log}`, `gpa-bf1-shadow-raw1`, `gpa-bf1-shadow-bindings1`,
`gpa-bf1-particle-buffers1`, `dump-invalid-binding2.txt`, and the bounded
`gpa_bf1_particle_probe.cpp` helper under ignored research artifacts. GPA player
and dump layer report **2025.1.3.1269** (product 2025.1.3). Dump-layer SHA-256
remains `853913de932f235341655d7f55f1d38d68e56d60df45402acff87825eaf78b72`;
player SHA-256 is
`aa605ff70e677f7d71cb841ee8b3573475c228d4b7d871d35c3d4e1cea11c7db`.
The research used four serial, specifically filtered original-player runs and one
read-only Ghidra query. No new native/RDC replay or unrelated regression was run.
The next section recovers that original shadow input and tests its effect. Its
ordering, BF1 fidelity and full feature/portable acceptance remain unfinished.

### Original BF1 shadow-buffer recovery and exact particle-color localization

The dump failure is separate from the capture's contents. Read-only Ghidra
inspection of the same hash-checked GPA dump DLL identifies the resource filter
at RVA **0xac20**: it accepts only DS/RT/UAV types **2/3/6**, so requesting every
buffer view cannot export vertex or constant buffers. The Draw counter at
**0xab30** is tested by **0xa690/0xa740** before incrementing; graphics binding
enumeration starts at **0xbca0**, then **0x14860**. A bounded child-process probe
confirms list **0x27b022210**, ordinal **17**, graphics stage **1**. Its first
64 bindings, then a complete **154-binding** probe, all have type zero. This is
an observed exporter-state failure, not evidence that the captured UAV is empty.

The descriptor enumerator also has inconsistent address units: **0x14bf9–0x14bfb**
adds a descriptor-range offset directly to the GPU table handle and **0x14c44**
increments that handle by one. Lookup **0x154f0** subtracts the heap start, while
**0x14770** divides by the heap's descriptor increment. A diagnostic child sees
an increment of **32**. Correcting only that lookup argument at **0x15546** still
finds all 154 entries empty, including corrected slot **4109** for the expected
UAV. Thus the stride issue alone does not explain this extraction failure. The
missing tracked descriptor contents at recording time remain unresolved; later
keyframe descriptor population is a hypothesis, not a verified cause. Another
post-layer attempt exits **0xc0000005** during restoration without output, as
earlier original-player attempts did; it is not used as a reference.

A different extraction follows the actual captured **CopyResource**, source
record **265612**, execution **35772**, immediately after the captured transition
at **265611 / 35771**. The initial CopyBufferRegion probe encountered GPA's own
4-byte auxiliary copy and was rejected before requesting any dump. The corrected
probe uses the selected command list's CopyResource entry and calls the
hash-identified original-object helper **0x4ea0** on its source. The returned
identity must equal **0x20664cf30** (creation **300425**). Only then does it call
buffer-dump helper **0xb1a0** on the same paused recording thread, with offset
zero and length **256,000**, before resuming the original copy. The helper adds
a readback after the recorded COPY_SOURCE transition; no ordering barriers are
inserted between the four shadow draws. Breakpoints/context changes are confined
to the probe's own child; installed binaries and captures remain unchanged.

GPA exits **0** and writes **256,000 bytes** to
`f_0_cl_27b022210_dc_0_18_uav_o_0_r_20664cf30.bin`. The counter has advanced to 18
at this copy boundary; that filename is not interpreted as Draw 18. SHA-256:
`55657dd672615456e5d222155ec377b378ae975b663a83eff1c68005c2b42f33`.
All 64,000 floats are finite. Compared with the earlier unmodified RDC VB1
snapshot, 16 float words differ (72–75, 144–147, 188–191, 1440–1443); the selected
primitive's vertices 1428/1431/1430 all match at **0.45074862241744995**.

One bounded analysis-buffer query reads the existing four-barrier experiment's
VB1 (creation **163437**, object **0x206645960**) at execution **36560**. Its SHA-256
is `749ae336379772522d09b2e4f51bc4a3c747c715a29eb5ed82424faa07cc1286`.
Only **12 float words**, indices **1428–1439**, differ from the recovered original:

| Vertex indices | Original GPA shadow factor | Four-barrier experiment |
| --- | --- | --- |
| 1428–1431 | 0.45074862241744995 | 0.35990530252456665 |
| 1432–1435 | 0.4450402855873108 | 0.3541969358921051 |
| 1436–1439 | 0.4094027578830719 | 0.3185594379901886 |

The existing capture-bound shader experiment tests this input difference without
changing default replay. PSO creation **221693**, VS original SHA-256
`d976d8abfb98f703466acdfea4a9e5a304f2fafe3f6ddd9ff99c4e295ae24879`, uses `v8.x`
for the per-vertex shadow factor. A first global constant replacement changed
other particles and did not match the original; it is not accepted as a fix.
The targeted replacement selects the 12 saved `v0.xyz` tuples and supplies their
original shadow values, retaining `v8.x` everywhere else. The diagnostic helper
checks the original DXBC container/checksum roundtrip, replaces exactly three
source operands, adds one temporary and explicit coordinate tests, and retains
the original signatures/reflection. The CLI's system disassembler confirms the
emitted instructions. Replacement SHA-256:
`b22e861c5b8762d0ba31c668c16d3bbd80ca50b27b6bdcd01daaa17b17460bda`.

With the four existing barrier operations retained, native PSO creation and
replay to execution **36560** succeed. The entire **3,870,720-byte** particle
attachment now matches the saved original GPA output exactly: SHA-256
`efd68b29828a47ced97da6d5bef5d9986af7e663f9da68071029891a08d1a16d`, **zero**
different RGBA words or pixels. All **26,473** previously differing pixels are
accounted for at this boundary. This establishes a direct input explanation for
this saved intermediate-image discrepancy, not a general replay correction.
The diagnostic shader/vertex data are kept outside Git and are never installed
as a default workaround. The reason the original and ordered native executions
produce these different factors, original replay variation, downstream full-frame
fidelity and other samples' unfinished features remain open.

Loading the same saved project with cursor **4** undoes only the shader edit and
retains the four synchronization experiments. Replay succeeds with no shader
replacements and restores the complete earlier native/barrier attachment exactly,
SHA-256 `a1cec82b979735c74db4dd23e7b41bcd189128ae376bbcd6fae0abfc029b7138`.
This verifies project loading, the actual GPU modification and exact undo for
this case. It does not validate an unmodified full frame or every resource edit.

Evidence is under ignored `artifacts/dx12-research`: `dump-resource-filter2.txt`,
`dump-draw-counter2.txt`, `dump-descriptor-{lookup,slots}1.txt`,
`dump-binding-loop-code1.txt`, `gpa-bf1-shadow-{all-bindings,stride,copy}*`,
`gpa_bf1_copy_buffer_probe.cpp`, `bf1-shadow-barrier-vb1-compare1`, and
`bf1-particle-shadow-{constant,selected}*`. Research helpers compile against the
existing checked DXBC utilities; the native runtime gains no GPA/Python dependency.
Validation comprised six specifically targeted GPA children (including rejected
or failed probes), one bounded RDC buffer query and three native modification/
undo replays. The analysis query reports no loaded GPA modules. No all-sample
regression or portable acceptance is claimed.

### BF1 shadow-prefix readback state and original-player variation

The apparent third-shadow-Draw mismatch was an original-exporter artifact.
Initial per-Draw probes of dump helper **0xb1a0** returned the same 256,000-byte
buffer after work ordinals **13** and **15**, with SHA-256
`b76404e012aa2caec2fda62c5b0d385c19fdedb24e60643c74353f1bff7ad01b`.
Those bytes must not be used as valid intermediate reference contents. An earlier
one-barrier probe using the same unchecked readback state is likewise excluded.
A native DrawInstanced breakpoint independently confirms the actual Draw precedes
each selected dump callback and uses the captured **3/1/0/0** arguments; this
was not an off-by-one Draw selection.

Ghidra identifies the dump helper's state lookup at **0xb444**, calling **0x9dc0**,
and its returned state at **0xb449**. That state becomes the readback transition's
StateBefore at **0xb492**; **0xb4b1** records the transition, and **0xb4f6** records
the copy. At ordinal **15**, a bounded probe verifies the native source pointer
matches the ClearUAV resource whose original identity is **0x20664cf30**, then
observes returned state **0x400 (COPY_DEST)**. The first diagnostic child rejects
that unexpected state before completing a dump. Captured source record
**364523**, execution **35614**, in preceding list **0x27b2bcc30**, transitions
this resource from **0xc0** to **0x8 (UNORDERED_ACCESS)**. The shadow list contains
no intervening resource barrier until **265611 / 35771**, which transitions it
from UAV to COPY_SOURCE. The dump layer's recording-time lookup is therefore
stale relative to the actual submitted sequence. Its local-list/global fallback
in **0x9dc0** cannot be assumed to provide a valid execution-boundary state.

A corrected diagnostic child accepts only the verified source and observed
COPY_DEST state, substitutes **UAV** as the export helper's StateBefore, and
restores the one-shot breakpoint immediately. The helper now records UAV to
COPY_SOURCE, the copy, and the reverse transition. No extra ordering operation is
added between the preceding shadow Draws. The child exits **0**. Its ordinal-15
readback matches both the saved direct native prefix and the existing four-barrier
RDC prefix at execution **35751**, across all **256,000 bytes**: SHA-256
`bb4e98be94f0da8cf8817fb2b586b4144e47669c478207212fbf5806cd3f405d`.
The previous invalid reference differs in **144 float words**; indices
1428/1432/1436 are **0** in the valid readback, rather than the invalid **1**.
This resolves this specific apparent pipeline/input discrepancy. The diagnostic
readback itself adds synchronization after the selected Draw, so it does not
establish uninstrumented whole-frame determinism.

The final CopyResource boundary from the previous milestone is separately
checked, without changing the export state. At **265612 / 35772**, **0xb449**
returns **0x800 (COPY_SOURCE)** for the identity-checked source, agreeing with the
captured immediately preceding transition. This child exits **0** and its full
buffer has SHA-256
`749ae336379772522d09b2e4f51bc4a3c747c715a29eb5ed82424faa07cc1286`, exactly the
saved native four-barrier result. It differs from the previous original-player
readback only at the same **12 words, 1428–1439**. Thus those two observed values
are both obtainable from original-player runs with no inter-Draw barrier
experiment. The earlier shader experiment still proves the cause of its saved
particle-image discrepancy and exact undo; its supplied factors are not a unique
correct result to hardcode into replay.

A final focused control records one explicit global UAV barrier after ordinal
**15**, before the fourth shadow Draw, and uses the checked UAV state for the
ordinal-17 readback. It exits **0** and also matches the entire native four-barrier
buffer, SHA-256 `749ae336...1286`. The third/fourth shaders read and write the same
UAV, and the captured interval has no UAV barrier. These observations support an
ordering sensitivity, but one controlled run does not establish that this one
barrier eliminates every variation. Default replay remains unchanged. No
additional repeated runs or all-sample regression were used to claim stability.

Evidence remains under ignored `artifacts/dx12-research`:
`dump-buffer-barriers-code1.txt`, `dump-list-state-callers1.txt`,
`gpa-bf1-shadow-trace15-1`, `gpa-bf1-shadow-state15-{1,2}`,
`gpa-bf1-shadow-copy-state1`, `gpa-bf1-shadow-ordered-state17-1`,
`bf1-shadow-native-prefix15-1`, and the corresponding bounded
`gpa_bf1_{shadow_state,copy_state,shadow_order_state}_probe.cpp` helpers.
The successful state-correct prefix and ordering-control folders include exact
byte/float comparisons. The GPA DLL/player hashes remain those recorded above.
All breakpoint changes are confined to each probe's own child; installed files,
captures and shipping code remain unchanged. This evidence milestone does not
complete BF1 full-frame fidelity or the remaining cross-sample feature acceptance.

### UAV Clear ranges in Resource History

Resource History now reconstructs `ClearUnorderedAccessViewFloat` and
`ClearUnorderedAccessViewUint` ranges from RenderDoc's captured CPU descriptor,
resource argument and rectangles. Previously these events were listed as recorded
operations with an unavailable range. Buffer views retain **element** indices,
count, format, stride and flags; 64-bit indices use decimal strings. Explicit
texture views retain Mip, array range/plane or 3D depth-slice range. The workspace
shows the view range and Clear rectangles, preserves the execution jump, and
exports the complete report. These are captured API arguments, not evidence that
every covered element changed or that the GPU and CPU descriptors agree.

The implementation follows pinned RenderDoc's `Serialise_ClearUnorderedAccessView*`
serialization of `ViewCPUHandle`, `pResource`, `NumRects` and `pRects`, with view
semantics from Microsoft's [buffer UAV description](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_buffer_uav).
It rejects resource/descriptor disagreement, overflowing element ranges,
truncated/inverted rectangles and texture ranges outside resource metadata.
Default/unsupported UAV descriptors remain explicitly unavailable; no guessed
whole-resource range is substituted. Clear ordering is not inferred from the
range, consistent with Microsoft's [UAV Clear ordering requirement](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewuint).

One real BF1 Resource History query uses the existing four-barrier analysis
capture through execution **35772**, resource **0x20664cf30**, creation **300425**.
Source **265543** / execution **35703** maps to RDC event **42593** and reports
R32_FLOAT format **41**, elements **[0, 64000)**, stride/flags **0**, and rectangle
**[0, 4700) × [0, 1)**. The subsequent CopyResource **35772** remains a whole-resource
source use. The query reports **2 captured operation ranges**, **0 range gaps**
and **0 unmapped uses**. The four Draw uses retain binding evidence rather than
being relabeled as measured writes.

Builds **443/444** succeed. Focused parser checks cover both Clear variants,
indices above 2^53, invalid/truncated inputs, array/3D bounds and adjacent existing
Copy/RTV/DSV range behavior (**6 passes including setup/cleanup**). The report-backed
workspace test verifies the real range text, exact execution jump and exported
JSON equality (**3 passes including setup/cleanup**); it reuses the single GPU
query and starts no additional GPU replay. Texture variants have structural test
coverage only at this milestone. Evidence: ignored `bf1-shadow-uav-history1`,
`bf1-shadow-uav-history-ui1`, `uav-clear-ranges-unit1.txt` and
`uav-clear-history-ui2.txt`. Full Resource/Pixel History acceptance remains open.

### UAV Clear pixel coverage and actual before/after values

Pixel History now applies the shared captured-UAV range decoder to float and
integer Clears of supported plane-zero 2D texture views. It filters by the chosen
Mip, array layer and half-open rectangles. Previously a UAV Clear remained an
unresolved coverage candidate, including when the queried pixel or layer was
outside its view. Unsupported dimensions, planes and descriptor layouts retain
an explicit coverage gap. This change does not infer shader fragments, counter
clears, ordering barriers or measured writes from descriptor coverage.

The `uav` mode of `tests/fixtures/dx12-half-blend-history.cpp` creates a two-Mip,
two-layer RGBA16_FLOAT target, matching CPU/GPU UAV descriptors for Mip **1**,
Layer **1**, and rectangle **[4,8) × [4,8)**. Three marked float Clear operations
(**11/12/13**) write red before the existing independent Draw prefixes. Explicit
RT-to-UAV and reverse transitions bracket each Clear. The fixture copies the
selected texel to readback immediately afterward; all three GPU results are
binary16 words **[15360,0,0,15360]**.

Three serial queries on that one capture verify distinct coverage conditions:

- **(5,5), Mip 1 / Layer 1:** 12 records, including all three mapped UAV Clears.
  Each before value is the captured initial color and each after value is exactly
  **[1,0,0,1]**, matching the fixture's GPU readbacks. The final history value also
  matches the selected texel before and after the analysis replays.
- **(8,5), Mip 1 / Layer 1:** 9 records; the three UAV Clears are explicitly
  excluded by the right-open rectangle boundary, with no coverage gaps.
- **(5,5), Mip 1 / Layer 0:** no modification records; all six RTV/UAV Clears
  are excluded by their captured view ranges. Uninitialized contents of that
  unused layer are not asserted as a reference image.

Builds **445/446**, the relevant parser/RTV coverage tests (**4 passes including
setup/cleanup**) and the report-backed Pixel History workspace test (**3 passes**)
succeed. The workspace test verifies execution navigation to **11**, disables
shader debugging for the Clear operation and checks exported JSON equality.
The inside query's API-validation report contains two loading-stage buffer-state
warnings; it does not expose the analysis replay validation queue, so this is not
a claim of validation-clean replay. Evidence: ignored `half-blend-uav1`,
`uav-pixel-coverage-unit1.txt` and `uav-pixel-history-ui1.txt`. No game sample was
replayed for this isolated coverage correction, and no RenderDoc runtime patch
was required. Integer Clear coverage has structural tests; the GPU value evidence
here is for float Clears only. Full sample histories remain unfinished.

### Talos resource-file partition and isolated resource history

A bounded CPU audit closes the previously unexamined physical-file gap in the
Talos snapshot audit. The **3,679** referenced CopyOperation descriptors cover
**235,456** header bytes and **1,590,966,351** payload bytes. Sorting their exact
intervals covers the entire **1,591,201,807-byte** `resources` file with **zero
gaps and zero overlaps**. There is no remaining unreferenced resource-file region
in which to search for another query snapshot. This does not rule out every
possible encoded metadata mechanism, reconstruct missing results, or prove
whole-frame fidelity. Evidence: ignored `talos-resource-partition1.json` and
`talos_resource_partition.py`; no GPU run was needed for this audit.

Explicit query isolation now permits **Resource History** with an execution,
resource object and creation identity. The analysis uses RenderDoc's loading-time
usage index without `SetFrameEvent` or resource-byte readback. Query destinations
may be inspected this way, but their bytes remain forbidden to value analysis.
The capture's validated query-isolation marker, `frame_complete=false`, omitted
result ranges and the scope of partial native resolves remain in the report.
The workspace displays the incomplete-replay notice and exports that policy.
Other history/debugging, metrics and experiment restrictions remain in force.

Workspace recapture and analysis carry the isolation flag in request identities;
strict and isolated captures have different cache keys. Switching mode invalidates
old rows and rejects stale results. Acceptance also rejects a missing/incorrect
capture policy or a report falsely marked complete. The native recapture report
is checked against the requested query policy before it enters the analysis cache.

The real Talos workspace test selects final execution **19410**, backbuffer
creation **35118** / object **1651480816**. It completes independent native
recapture and indexed analysis, returning **six** uses: barriers **19378/19410**
and color-target bindings **19389/19396/19404/19408**. All are mapped, both missing
query resolves remain in the policy, and `selected_event_replayed=false` explicitly
separates usage indexing from value analysis. Exported JSON is equal to the
accepted result. Returning to strict mode clears the rows and disables the
unavailable late-frame query. No GPA/Python modules are loaded by analysis.

A focused query of isolated destination **78132 / 1643440736** returns **52**
partial ResolveQueryData uses, with parent execution **16001** and source record
**188923**. It writes no buffer/texture data. Their per-call byte ranges remain
unavailable and their expanded child events are nonselectable; these gaps are
reported, not attributed to neighboring Draws or claimed as completed history.
The policy still distinguishes omitted query slots from available native resolves.

Builds **447–449** pass. Policy, job-preparation and Talos capability/navigation
checks each pass **3 tests including setup/cleanup**. The workspace end-to-end
check passes **3** in **9,597 ms**; the existing strict BF1 Resource History view
check also passes **3**, without another GPU replay. Two earlier workspace
attempts failed because the analysis executable's identity-specific entry guard
still had its old action whitelist; this guard is now updated and the same path
passes. A CLI query against the existing isolated capture localized that failure.
Evidence: `talos-query-history-ui3`, `talos-isolated-resource-history1`,
`talos-isolated-query-usage1`, `query-history-{policy,options,capability}1.txt` and
`query-history-strict-view1.txt`, all under ignored research artifacts. Historical
query-value recovery and full Talos replay/debug acceptance remain unfinished.

### Verified partial query-resolve ranges and source navigation

Resource History now decodes each captured `ResolveQueryData` destination range,
query type/heap, first slot and count. Query-result strides share the same layout
helper as native replay and preflight: scalar queries, stream-output statistics,
and both pipeline-statistics layouts retain their actual sizes. Unknown layouts,
unaligned offsets and overflowing slot/byte ranges remain explicit failures.
Buffer offsets, lengths, ends and resource identities retain decimal strings.
No query-result value is read by this operation-range decoder.

For query-isolated Resource History, expanded Resolve calls become navigable
only after **every** emitted part agrees with the capture policy. Verification
requires a unique Resolve provenance marker, matching execution/source/submission/
queue, the exact target resource lifetime, ordered event IDs, all and only the
available query intervals, and consistent query heap/type/stride. Mismatches
remain in `query_resolve_mapping_gaps`; they do not receive selectable provenance.
A verified row shows its part ordinal and navigates to the original Resolve
execution. Selecting that original event as the history endpoint uses the last
verified emitted part. This endpoint selection is limited to indexed Resource
History; ordinary state/value analysis still rejects a non-unique action.

Two serial queries reuse the existing isolated Talos RDC: through final execution
**19410**, and through original Resolve **16001** itself. Destination creation
**78132**, object **1643440736**, has **52** actual Resolve uses, all with source
**188923** and matching captured availability ranges. Their total destination
coverage is **768 bytes / 96 occlusion results**; the first range is **[64,72)**,
slot **8**, count **1**. Missing slots remain omitted in the policy. Both queries
return identical use rows, **52** captured ranges, **0** range gaps and **0**
nonselectable uses. The Resolve endpoint maps to RDC event **10174** and keeps
`selected_event_replayed=false` and `frame_complete=false`.

Builds **450/451** pass. Focused provenance tests reject wrong origins and missing
or inconsistent parts; range tests cover offsets above 2^53, all
implemented result layouts, unknown types, alignment and overflow (**4 + 3 passes
including setup/cleanup**). The report-backed workspace test checks part labels,
byte ranges, navigation to **16001** without an indirect-action ordinal, selection
of the Resolve endpoint, malformed-part/stale rejection and exact JSON export
(**3 passes**, **78 ms**). It launches no GPU work. Evidence: ignored
`talos-query-resolve-history1`, `query-resolve-mapping1.txt`,
`query-resolve-ranges1.txt` and `query-resolve-history-ui1.txt`. No native sample
recapture or query-value recovery is claimed. Historical query contents and full
Talos replay/debug acceptance remain unfinished.

### Portable native replay and analysis dependency evidence

The packaged DX12 Worker now records `loaded_modules` while its replay, device
and optional RenderDoc capture objects are still alive. The native and analysis
processes share the same bounded module enumerator. Enumeration failure or a
truncated path cannot silently produce an empty/partial inventory; a native
replay that cannot record provenance returns a failed report while retaining its
diagnostic artifacts. The field is explicitly a **snapshot**, not a lifetime
DLL-load or child-process trace.

`tools/verify_portable_modules.ps1` checks successful DX12 reports against the
chosen package, Windows and the capture's Agility SDK directory. It rejects
missing inventories, missing files, external development dependencies and
GPA/Python modules, and records hashes of the reports and non-Windows modules.
It accepts an explicit report list and runs no GPU work:

```powershell
./tools/verify_portable_modules.ps1 `
  -PackageDirectory <package> -CaptureDirectory <single-frame-directory> `
  -Reports <native-report.json>,<analysis-result.json> -OutputFile <new-evidence.json>
```

Build **452** passes. `tools/package.ps1` assembles
`out/dx12-portable-build452` with the pinned DXC and the existing 27-patch
RenderDoc runtime (SHA-256
`147bb78f64656ec9be4c9f5932c4ae830d9e77f8e4a36c136695b5a0d97c65a6`).
Workers and analysis are launched from that package with PATH restricted to
Windows/System32 and Qt/Python override variables removed.

- **Talos:** packaged Worker replays through **19410**, reads texture creation
  **35118** and creates a fresh RDC with the packaged RenderDoc. Native storage
  remains exactly
  `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
  Packaged CLI discovers its bundled analyzer/library without an explicit
  analysis DLL override. Resource History through **16001**, destination creation
  **78132**, returns **52** verified ranges, zero mapping gaps and endpoint
  **10174**. All history rows equal the verified development result, and the
  exported native storage file's hash is checked directly. The native/analyzer
  snapshots contain **84/88** modules, all within
  the package or Windows. Query isolation remains explicit and
  `frame_complete=false`.
- **BF1 SDK compatibility:** one packaged native buffer query at UAV Clear
  **35703**, creation **300425**, reads **18,800 bytes** with SHA-256
  `f9ef289a12f1f8e88c143059b09b92b7c2be1ef4026b0b7420bdf5146675ed07`,
  matching the previously verified all-ones float range. Its requested SDK is
  **602**; `CreateDeviceFactory` succeeds and the device reports actual SDK
  **616**, with `D3D12Core.dll` loaded from Windows. Talos requests/reports
  **616**. No claim is made that this machine executes the old 602 runtime.

An initial Talos invocation used the CLI's default **256 MiB** allocation limit
and correctly failed initialization. The subsequent chain uses the established
**4 GiB** sample limit; BF1 uses its established **8 GiB** limit. The successful
chain was repeated only after adding native module provenance. Verification
also rejects the earlier native report without an inventory and a real
development-build analysis report. No all-sample regression, Hellblade2 GPU run
or clean-machine validation is claimed.

Evidence: ignored `portable-package1.log`, `portable-package2.log`,
`portable-chain1/2/3`, `portable-native1/2/3.log`, `portable-history2/3.log`,
`portable-bf1-602-1` and its log. `portable-chain3/module-verification.json`
and `portable-bf1-602-1/module-verification.json` record loaded module hashes;
`portable-chain3/rejected-provenance.json` records the two negative checks and
`portable-chain3/baseline-comparison.txt` records history/file comparisons.
These establish the selected native/analysis dependency paths. Full portable
three-sample replay, debugger, geometry, performance and experiment acceptance
remain unfinished, as do the underlying fidelity gaps.

### Pipeline inspection after an isolated query boundary

The query-isolated workspace now permits **Pipeline** inspection at a supported
Draw/Dispatch execution. Native recapture still rejects ordinary consumers of
isolated query READBACK resources; pipeline inspection reports bound state and
resource metadata without reading query-result bytes. The analyzer requires an
explicit nonzero execution identity and matching embedded query policy. The
result retains `frame_complete=false`, marks
`pipeline_scope.query_outputs_incomplete=true` and describes the missing query
values in its scope. Shader debugging, geometry, performance and modification
experiments remain disabled in this mode.

Pipeline requests, worker envelopes and capture-cache keys carry the isolation
mode. Mode changes invalidate pending/results state. The view rejects absent or
mismatched policies and reports that claim a complete isolated frame. Resource
and PSO creation navigation remain available; **Replace shader** remains
disabled. These are incomplete-replay bindings, not restored historical query
results or an assertion of full original-frame fidelity.

One end-to-end workspace test opens Talos, selects execution **19408**, enables
isolation and invokes Pipeline **Read**. It performs native recapture followed by
the real isolated analyzer. The result has **51** binding/state rows and maps the
selected Draw to source **192508**, RDC event **20484**. VS and PS both belong to
captured PSO **156511**, object **1657076288**. RTV 0 maps to texture creation
**35118**, object **1651480816**, and resource navigation selects that lifetime.
The embedded policy contains the earlier incomplete Resolve **16001**; the
later Resolve **19409** is outside this selected prefix. No GPA module is loaded.

Build **453** passes. Focused policy and job-preparation checks pass
(**3 + 3** including setup/cleanup), covering missing/zero/non-string execution
identities, mode mismatch, query-output resource rejection and still-disallowed
analysis actions. The workspace test passes (**3**, **11,672 ms**) with JSON
export, missing/tampered policy rejection, replacement-action gating and stale
result rejection after switching back to strict mode. The existing CPU-only
query-boundary test also passes (**3**, **331 ms**), including the newly supported
Draw and still-unsupported strict/query-dependent selection. Build **454**
updates the explanatory tooltip/error text afterward; GPU work is not repeated
for that wording-only change.

Evidence: ignored `talos-query-pipeline-ui1/{result,recapture}.json`,
`query-pipeline-policy1.txt`, `query-pipeline-job1.txt`,
`query-pipeline-ui1.txt` and `query-pipeline-capability1.txt`. This is one targeted
Talos GPU chain, with no new BF1/Hellblade2 replay or RenderDoc patch. The
Hellblade2 MainPS arithmetic discrepancy was rechecked against its existing
counterexample: fast flags do not uniquely determine driver contraction.
Its experimental model remains opt-in and no unproven arithmetic change is
introduced by this milestone.

### Shader debugging after an isolated query boundary

Query-isolated analysis now accepts explicit DX12 **VS/PS/CS** invocation
requests. The native replay still rejects ordinary consumers of isolated
READBACK resources, and the analysis policy still rejects explicit reads of
those query outputs. Each request requires a nonzero execution identity and a
matching capture marker/request envelope. Shader traces retain
`frame_complete=false`, `partial=true` and
`debug_scope.query_outputs_incomplete=true`; a successful invocation or GPU
output comparison does not restore historical query values.

The workspace carries the mode through the existing capture cache and debug
request. The debugger keeps an incomplete-replay label while stepping and
shows the policy scope alongside existing arithmetic/input warnings. Mode
changes invalidate traces and invocation selections. Missing/tampered policy,
false completeness/partial flags and stale results are rejected before replacing
the current model. Strict DX12 reports and the DX11 context defaults are retained.
This change does not enable isolated Pixel History, geometry, performance or
modification experiments.

One actual Talos workspace run selects execution **19408**, VS vertex ordinal
**0**, instance **0**, and performs native recapture followed by debug analysis.
The index buffer resolves the invocation to actual vertex **1**. Its DXBC
bytecode SHA-256 is
`0c0b1135c50eab06a996d50bea69b0335dfd8b3805c7f929651f5504d07205dc`.
The trace has **20** steps and no source files. Selected input bytes are unchanged
at all three recorded observation points. All **15** components of the four
outputs (`SV_Position`, `TEXCOORD0`, `TEXCOORD1`, `NV_X_RIGHT`) exactly match the
separate instrumented GPU VS replay. Position words are
**[3211705538, 1059942408, 1065353216, 1065353216]**. This is output agreement at
this invocation; constants/textures, intermediate machine instructions and
full-frame fidelity are not thereby verified.

The workspace test steps, stops at instruction breakpoint **10**, and evaluates
`SV_Position.x` at final step **19** as **-0.9325677156448364**, equal to the
GPU reference. Export retains the breakpoint, Watch and incomplete policy.
The test also rejects missing policy and incorrect frame/partial/scope flags,
then clears the model and refuses the old result after disabling isolation.
The native and analysis reports retain incomplete frame status; the analysis
module snapshot contains no GPA/Python modules.

Build **455** passes. Targeted tests pass: policy **3**, job preparation **4**,
capability/worker identity **4**, real shader workspace **3** (**9,249 ms**), and
the existing strict-mode instruction-only VS report **3** (**52 ms**), all counts
including setup/cleanup. Job/policy checks cover all three stage actions, wide
decimal identities, missing/invalid execution selections and forbidden query
resource references. The worker identity test's obsolete diagnostic-string
expectation is updated to the current unsupported-action error. Only the
selected VS path receives a new GPU validation here; PS/CS isolation is connected
but not marked sample-verified. No new RenderDoc patch or other-sample replay.

Evidence: ignored `talos-query-shader-ui1/{result,recapture,export,output-evidence}.json`,
`query-shader-policy1.txt`, `query-shader-job1.txt`,
`query-shader-capability1.txt`, `query-shader-ui1.txt` and
`query-shader-strict-ui1.txt`. Historical query recovery, remaining stage evidence
and the complete acceptance matrix remain unfinished.

### BGRA intermediate history correction and isolated Talos PS evidence

The selected Talos PS at execution **19408**, pixel **(65,172)**, primitive
**0**, sample/mip/layer **0**, produces a **21**-step DXBC PS trace. Its shader
SHA-256 is
`9ac2a66820c99072d1ee0bd792e6164206669e67a55fa30461d0a32cb3b42c2f`.
The four output words **[0,0,0,1049859732]** exactly match the separate GPU
fragment output, including alpha **0.2882581949234009**. The existing isolated
RDC is reused; this is not another native full-frame capture. The report remains
`partial=true`, `frame_complete=false`, with the incomplete-query warning.

Inspecting that reference exposed an intermediate Pixel History defect in
RenderDoc 1.45: `EncodeFormattedComponents` wrote regular BGRA values in RGBA
order, whereas `DecodePixelData` correctly swapped red/blue on decode.
`ConvertAndFillInColor` uses this encode/decode pair to quantize intermediate
floating-point history results. Event endpoints read from the original texture
were unaffected; checking only the final Draw color therefore missed the bug.
Patch **28**, `dx12-bgra-history.patch`, swaps red/blue before regular BGRA
encoding, preserving the existing component conversion and rounding.
The analyzer reports `backend_dx12_bgra_history_patch=1`; the build manifest and
packager pin the patch source hash and version.

The existing small GPU fixture now has a `bgra` mode with an asymmetric clear,
source-alpha blending, and three overlapping fragments (the second is
transparent). Separate one-, two- and three-triangle GPU draws provide raw BGRA
readbacks independently of Pixel History. At **(5,5)**:

| Fragment | Independent GPU RGBA8 | History before patch | History after patch |
| --- | --- | --- | --- |
| 0 | 44,52,147,115 | 147,52,44,115 | 44,52,147,115 |
| 1 | 44,52,147,115 | 147,52,44,115 | 44,52,147,115 |
| 2 | 52,44,139,108 | 52,44,139,108 | 52,44,139,108 |

All three storage values and the intervening pre/post chain now agree. Talos's
first fragment changes from the erroneous RGBA8 **[1,4,4,176]** to
**[4,4,1,176]**. The PS output comparison remains exact; the final selected target
words **[1052556477,1060945085,1052424891,1060681913]** remain unchanged before
and after this fix, with exact history-endpoint agreement. This validates the
channel correction and selected PS output, not every intermediate instruction,
all quantization formats, or complete original-frame fidelity.

Analyzer build **456** and the exact 28-patch RenderDoc build pass. DLL SHA-256:
`8e5d94c5f3e2f5404f92b8d31d8dfa0154914a8881590f6778f06a6e4c993a2b`.
`out/dx12-portable-build456` passes packaging with the same patched DLL and
source patch included. No new complete portable runtime acceptance is claimed.
No BF1/Hellblade2 replay or broad regression suite was run for this correction.
Evidence: ignored `bgra-history1/{gpu,verification}.json`,
`bgra-history1/{baseline,fixed}/result.json`, `talos-query-ps{1,2}/result.json`,
`verify_bgra_history.ps1`, `bgra-rdc-build1.log`, `build456.log` and
`portable-package456.log`. Isolated Pixel History workspace access and the
remaining acceptance requirements are still unfinished.

### Pixel History in the incomplete query-isolated workspace

Query isolation now admits explicit DX12 `history` requests through the native
worker, CLI preparation, capture policy and workspace. Execution, resource
object and creation identities must be nonzero decimal strings. Unlike
index-only Resource History, Pixel History still rejects query-output resources.
The request/cache identity retains the isolation mode. Results must retain
the embedded query policy, `frame_complete=false`, `partial=true` and
`history_scope.query_outputs_incomplete=true`; missing or contradictory fields
are rejected before replacing the displayed history. Disabling isolation clears
the history and rejects old request results.

The existing output pixel picker, history table, export, execution navigation
and PS invocation handoff now work in this mode. The history summary and exported
scope explicitly describe the incomplete replay. Existing strict-mode/DX11
contexts remain supported. This does not recover missing historical query values
or establish original-frame fidelity.

The actual Talos workspace test opens the directory, selects execution **19408**
and resource creation **35118**, enables isolation, replays the selected texture
and selects output pixel **(65,172)** at mip/layer/sample **0**. The subsequent
native analysis capture and history query return:

| Execution | Source record | RenderDoc event | Fragment / primitive | Post RGBA8 |
| --- | --- | --- | --- | --- |
| 19389 | 192489 | 20427 | 0 / 0 | 6,6,2,217 |
| 19408 | 192508 | 20484 | 0 / 0 | 4,4,1,176 |
| 19408 | 192508 | 20484 | 1 / 2 | 94,188,93,184 |

All three records are mapped/selectable. The selected texel before/after history
and the final history boundary match component bits exactly:
**[1052556477,1060945085,1052424891,1060681913]**. The intermediate first
fragment retains the BGRA correction verified in the preceding milestone.
Filtering the execution out, then selecting **Debug PS** on its history row,
restores visibility and fills execution **19408**, primitive **0**, pixel
**(65,172)** and the isolation flag in the debugger. This test checks the handoff;
the actual PS trace/output comparison is recorded in the preceding milestone.

The first workspace attempt exposed a null analysis-request access during
ordinary native Replay, before an analysis request existed. The new history
allowlist now guards that access with an object check. The repeated targeted
workspace run passes in **18,308 ms**, including real output replay and recapture.
The policy, preparation and capability checks each pass **3** tests (including
setup/cleanup). The strict DX12 indirect-navigation and existing DX11 hardware
history workspace checks pass **4** tests in **16,073 ms**. No BF1/Hellblade2 or
full-suite replay was added. Final build **460** retains the **28** RenderDoc
patches from the preceding milestone; no new DLL patch.

Evidence: ignored `talos-query-pixel-ui3/{result,recapture}.json`,
`query-pixel-{policy1,job1,capability1,ui3,compat1}.txt`, and `build457.log` through
`build460.log`. Earlier `query-pixel-ui{1,2}.txt` record the null-request failure.
Full histories, remaining subresources/test reasons, isolated geometry and
performance/experiments, and complete portable acceptance remain unfinished.

### Geometry in the incomplete query-isolated workspace

Explicit DX12 `postmesh` requests now carry query isolation through preparation,
worker dispatch, capture-policy validation and Replay Mesh. The execution
identity must be a nonzero decimal string; query-output resource selectors remain
rejected. The workspace retains the mode in its capture cache and rejects
results missing the embedded policy, `frame_complete=false`, `partial=true`
or `mesh_scope.query_outputs_incomplete=true`. Changing modes clears the mesh and
invalidates pending results.

Both input attributes and post-shader data use the existing geometry decoder,
table, preview and vertex-invocation mapping. The view labels incomplete replay.
OBJ/CSV and raw input vertex/index exports now include an adjacent `.json`
provenance file in isolation mode, with the report, query policy, exported byte
length and SHA-256. A failed provenance write prevents writing the data file.
Strict-mode/DX11 exports retain their existing behavior.

The actual Talos workspace check selects execution **19408**, instance **0** and
reads **VSOut**, then **VSIn attribute 0**, reusing the identical native capture.
Both return **552** vertex rows, **828** indices and **276** triangles. The
post-VS stride is **60** bytes; the original input slot-0 stride is **12** bytes
and its float3 TEXCOORD0 data is displayed without assuming a position semantic.
Displayed vertex **1** maps to Draw vertex ordinal **0** in both stages. Its
post-VS position words are exactly
**[3211705538,1059942408,1065353216,1065353216]**, matching the earlier selected
GPU/VS trace comparison. Its three input words are
**[0x42817852,0x432e27ae,0]**, and the exported 12-byte attribute matches the
earlier debugger input snapshot. The first original 16-bit index is **1**.
These are selected-vertex/input checks, not validation of every vertex or the
original full frame.

The raw input exports contain **6,624** vertex bytes and **1,656** index bytes.
Their recorded SHA-256 values are respectively
`ec57a35bb880052b8365f260a379a0c76cb1ba43c6e9abb638b2c86698006138` and
`144005f18006ce1325e3056d47b1b978b4bcc5d63c645aba65373efc631f7f14`.
OBJ/CSV exports for both stages and both raw input exports retain matching
sidecar hashes and incomplete policy. Selecting row 1 in either view prepares VS
execution **19408**, ordinal **0**, instance **0** in query-isolated mode.
Missing policy, incorrect completeness flags and old results after switching
back to strict mode are rejected.

Build **461** passes. The real workspace/selection checks pass **4** tests in
**16,965 ms**; policy **3**, job preparation **4** and capability **3** pass.
The existing strict DX12 geometry/indirect-selection report check passes **3**
tests in **115 ms**, without another GPU replay. Counts include setup/cleanup.
No RenderDoc patch, other-sample GPU replay or broad regression suite was added.
Evidence: ignored `talos-query-mesh-ui1/{input,output}.json`, exported OBJ/CSV/raw
data and their sidecars, `recapture.json`, `query-mesh-{ui1,policy1,job1,capability1,strict1}.txt`,
and `build461.log`. Isolated GS/DS/MeshOut requests are connected but not newly
sample-verified here; remaining stages, full geometry fidelity, performance,
experiments and full portable acceptance remain unfinished.

### Native performance measurements in query-isolated replay

The native selected-event timestamp/statistics and prefix-timeline path now
accepts query isolation. `NativePerformance` and `NativeTimeline` create their
own Query Heaps and readback buffers; they do not consume or restore the captured
query-output resources. Existing unknown-byte consumer checks, the restriction
against combining native measurement with RenderDoc capture, and the isolation
restriction against modification experiments remain in force.

The CLI, capability adapter and existing performance workspace carry this mode.
The workspace enables native instrumentation; RenderDoc/vendor counter sampling
in isolated replay remains unavailable. Native reports and the exported view
retain `missing_query_policy=isolate_readback_outputs`, `frame_complete=false`,
the measurement's `query_outputs_incomplete=true`, and the captured query
coverage description. The converted view is `partial=true`. Filtering metrics
keeps the incomplete label. Mode changes invalidate results; inconsistent
top-level/measurement policies, missing coverage and incorrect completeness
flags are rejected. The native worker boundary also validates query policy.

One actual Talos workspace measurement selects execution **19408**, source
record **192508**, queue **175347920**, submission **192532**, command list
**1650859184**. Its independent counters report:

| Measurement | Observed value |
| --- | --- |
| IA vertices / primitives | 828 / 276 |
| VS invocations | 552 |
| Clipper invocations / primitives | 276 / 276 |
| PS invocations | 8,481 |
| CS, HS, DS, GS invocations; GS primitives | 0 |
| Selected-event GPU duration | 3.424 microseconds |
| Timestamp difference / queue frequency | 3,424 ticks / 1,000,000,000 Hz |
| Prefix list measurements | 16, all with CPU-QPC clock calibration |

Begin/end timestamps **1792471599545846272 / 1792471599545849696** remain
decimal strings. Duration is computed from their integer difference. The
selected Draw is bracketed by the timestamp range of its matching prefix list.
These are one sample of the current replay with no warmup or stable-power-state
request; they are not original-capture timings, stable benchmark values or
vendor hardware counters. The Draw's vertex/primitive counts agree with the
geometry evidence in the preceding milestone.

Builds **462–464** pass. The first workspace attempt stopped at the native
backend's old isolation/performance rejection, before measuring the frame; that
guard is now removed. The real workspace check passes **3** tests in **4,341 ms**,
including setup/cleanup, and covers metric navigation through an API filter,
export and invalid-result rejection. Capability and existing native report-policy
checks also pass. The recorded strict native-report check passes without GPU
replay. A first fallback-timeline check used the earlier report without a timeline;
with the existing timeline report it passes **3** checks in **28 ms**, preserving
wide timestamp identities and queue-local display when calibration is absent.
No additional BF1/Hellblade2 GPU replay or repeated timing series was run.

Evidence: ignored `talos-query-performance-ui2/result.json`,
`query-performance-{ui1,ui2,strict1,fallback1}.txt`, and `build462.log` through
`build464.log`. Isolated vendor counters and experiments, original-frame
fidelity, complete synchronization/performance coverage and full portable
acceptance remain unfinished.

### Modification overlays in query-isolated replay

Query isolation now accepts the existing native experiment overlay through the
workspace and CLI. This supersedes the experiment restrictions in the preceding
milestones. Disabling a direct Draw/Dispatch preserves surrounding query commands;
shader replacements, resource imports and inserted UAV barriers use the same
validated paths as strict replay. Ordinary access to isolated query READBACK
allocations still fails, including resource patches and descriptor bindings.
An experiment neither supplies missing historical query values nor makes the
frame complete. Vendor counter analysis remains unavailable in this mode.

DX12 project UI state saves `missing_query_policy`. Loading restores that mode
before applying selection/output context; an omitted field in older projects
means strict replay, and unknown values reject the load without replacing the
current experiment. Switching mode marks an open project modified. Native raw
texture exports now include the experiment capture identity, document hash and
cursor as well as the existing query policy and incomplete-frame status.

The actual Talos workspace check disables execution **19408**, source record
**192508**, and reads creation **35118**, subresource **0**, after that execution:

| State | Raw BGRA8 pixel (65,172) | Complete 8,294,400-byte storage SHA-256 |
| --- | --- | --- |
| Baseline | `5d bc 5e b8` | `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb` |
| Draw disabled | `02 06 06 d9` | `5e3aae068d6743e52729350ec8e958861a21e5c800bad41b33946dddc4450f82` |
| Undo | `5d bc 5e b8` | `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb` |

The disabled pixel retains the preceding mapped fragment's RGBA8
**[6,6,2,217]**, consistent with the earlier Pixel History evidence. Undo compares
every raw byte with the baseline, not just that pixel. Both edited exports
retain `frame_complete=false`; cursors **1 / 0** and different document hashes
distinguish disabled/restored states. Existing debug, mesh, history and metrics
request tokens are invalidated. Metrics for the disabled selected Draw are
unavailable. Redo, save/reopen, invalid-policy rejection and legacy strict-policy
loading are checked with automatic replay disabled.

Build **465** passes. The targeted workspace/capability run passes **4** checks
including setup/cleanup in **51,868 ms**; its identity-fixture check initially
skipped because the fixture path was unset. With the existing strict Talos
fixture configured, that identity check and the DX11 experiment workspace
compatibility check pass **4** checks in **2,582 ms**, without additional GPU
replay. Evidence is under ignored `talos-query-experiment-ui1/`,
`query-experiment-ui1.txt`, `query-experiment-compat1.txt` and `build465.log`.
Shader/resource/barrier overlays are connected but not newly sample-verified
under query isolation here. Full experiment coverage, original-frame fidelity
and complete portable acceptance remain unfinished.

### Query-isolated shader/buffer overlays and inputless PS debugging

Two Talos workspace experiments now exercise execution **19408**, output
creation **35118**, and the incomplete query policy through native replay,
fresh analysis capture, Shader Debug, pipeline inspection and undo:

- Replacing the PS in pipeline **156511** with an opaque magenta shader yields
  raw BGRA8 **`ff 00 ff ff`** at **(65,172)**. The modified full-storage hash is
  `42ad87cc9b2bd4312676289ad49db4e475fdc4818b4f14e9bf4bd8ec8f746fc6`.
  The replacement bytecode SHA-256 is
  `2d38ffa150b7d9909a3893c13ecddf796c62edf1924fddd77e3b9cbd567715c8`;
  native PSO creation, the bound-stage report and the debug trace agree on that
  identity. The replacement applies to the pipeline lifetime, not only this Draw.
- An **8-byte** zero patch at offset **32866312** of resource creation
  **150566**, applied before submission **192532**, changes the original PS's
  `cb0[0].zw` alpha scale/bias. The fresh debug trace reads
  **[-1,0,0,0]** for `cb0[0]`, retains the original PS hash, and shows zero
  `SV_Target.w` at step **20**. The native report records a `buffer_copy` at
  the requested boundary. The entire output hash is
  `5e3aae068d6743e52729350ec8e958861a21e5c800bad41b33946dddc4450f82`,
  identical to the previously verified disabled-Draw image.

Each undo invalidates both debug and pipeline results and restores the full
**8,294,400-byte** output hash
`e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
Debug and pipeline queries reuse the same experiment recapture. Exports retain
the experiment identity and incomplete query/frame status.

The constant replacement exposed a RenderDoc 1.45 input-fetcher defect: a PS
with no input signature acquired a synthetic `SV_Position` at register **v0**,
while the captured VS produces position at **o2**, after two user outputs.
The auxiliary debug PSO failed with `E_INVALIDARG`, returning no trace.
The fallback also counted its synthetic `float4` as **4 bytes**, not **16**.
The pinned `dx12-empty-pixel-inputs.patch` version **1** preserves the producer's
position register using the existing signature gap filler and corrects the
fallback byte stride. Fetch-only entries remain register **-1**, so they never
become invented inputs to the original shader interpreter.

The saved replacement capture reproduces failure with the prior DLL. With the
patch, the same capture returns **3** debug states and an empty real input list.
The four output words are exactly **[1065353216,0,1065353216,1065353216]**
(`float4(1,0,1,1)`), agreeing with the native opaque pixel. The workspace Watch
shows `SV_Target.w=1` at step **2**. The UI now also enables the bound-stage
replacement button in query isolation and permits changing query mode while an
experiment exists; the obsolete tooltip restriction is removed.

Builds **466–468** pass. The first targeted run passed the buffer workflow and
strict pipeline-navigation check but failed constant-PS debugging. After the
focused same-capture reproduction/fix, only the failed shader workflow was
rerun: **3** checks including setup/cleanup pass in **66,755 ms**. No further
BF1/Hellblade2 replay or repeated buffer experiment was run.

The canonical RenderDoc build now has **29** pinned patches; its DLL SHA-256 is
`efcd5800c001d4f8a41e4251454f6df2ebc07428aa120f712159f4db6c469fcb`.
Reports expose `backend_dx12_empty_pixel_inputs_patch=1`, and package validation
checks the new patch's version and hash. Packaging to ignored
`out/dx12-portable-build468` passes; this is not full portable runtime acceptance.

Evidence: ignored `talos-query-overlay-ui1/buffer/`,
`talos-query-overlay-ui2/shader/` (including `output-evidence.json`),
`talos-constant-ps-repro1/` (same RDC, baseline diagnostic log and fixed trace),
`query-overlay-ui{1,2}.txt`, `empty-pixel-inputs-build1.log`,
`build466.log`–`build468.log` and `package468.log`.
The inputless-PS reproduction verifies DXBC with this producer layout; DXIL and
other producer stages are not newly verified. Texture overlays under query
isolation were still unverified at this milestone; the next section records
the subsequent focused check. Remaining experiment coverage, original-frame
fidelity and full portable acceptance remain unfinished.

### Query-isolated texture import, pixel navigation and modified history

The Talos texture overlay now has an end-to-end workspace check at execution
**19408** / record **192508**. Creation **51141**, object **4146950352**, is a
1024×1024, single-mip, single-layer `B8G8R8A8_TYPELESS` texture (format **90**).
Its description comes from metadata packet **51141**, header offset **4240815**,
132-byte payload starting at **4240847**; the existing native decoder and
RenderDoc PS SRV binding agree on its identity. The initial test assumption that
the allocation itself was UNORM was rejected before running the GPU.

The native readback now previews this typeless BGRA8 storage explicitly as UNORM,
without a color-space transform. Both the report and UI tooltip identify that
interpretation; it does not claim to recover an intrinsic typed resource format.
Raw exports retain format **90** and unchanged storage bytes.

The experiment imports **4,194,304** bytes, repeating BGRA `21 43 65 00`, into
subresource **0** before submission **192532**. The resource is read back after
execution **19408** and every byte matches the import, SHA-256
`130bcad8ba9a879553f911a1c6d73663a443c4d2095847826a6754370918fe89`.
The preview reports RGBA `(101,67,33,0)` at `(0,0)`, checking channel order as
well as transparent alpha. This is native GPU readback, not a capture preview.

The original PS remains bound, SHA-256
`9ac2a66820c99072d1ee0bd792e6164206669e67a55fa30461d0a32cb3b42c2f`.
A fresh analysis capture supplies **21** debugger states; `SV_Target.w` is
**0** at step **20**. Pipeline inspection and Pixel History reuse that same
modified capture. Output creation **35118** at `(65,172)` retains BGRA
`02 06 06 d9`: the three history rows map to execution **19389** / record
**192489**, then both fragments of **19408** / **192508**. Both final-Draw
fragments have zero shader alpha and preserve RGBA8 `(6,6,2,217)`.

The modified full output SHA-256 is
`bbdd19451f826f4ceba48d90f6ba466d5749557ec0478d4a61267384bf2f66df`.
It differs from the previously verified disabled-Draw image at **900** pixels,
within inclusive bounds `(1776,327)`–`(1860,340)`. A texture patch applies before
a submission and can affect multiple uses; equivalence to disabling only the
last Draw was an invalid test assumption. The changed pixels are recorded,
without attributing them to an unverified individual Draw.

Pixel navigation previously asked for the strict-only analysis key and silently
rejected valid query-isolated selections. It now validates the current explicit
query policy, session, execution and experiment context, rejects empty keys,
and retains the chosen pixel after readback. The test uses the real analysis
context to navigate to the imported texture and back to the output; it also
rejects a strict-mode key and an obsolete experiment key. This exercises the
shared navigation slot, not an isolated CS texture-store debug invocation.

Undo invalidates debug, pipeline and history results and restores all
**8,294,400** output bytes to the established baseline SHA-256
`e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
Builds **469–471** pass. After correcting the format and scope assumptions,
only the texture workflow was rerun: **3** checks including setup/cleanup pass
in **92,087 ms**. The subresource-navigation CPU check also passed in the first
run. Shader/buffer workflows, BF1 and Hellblade2 were not repeated.

Evidence: ignored `talos-query-overlay-ui5/texture/` (description, experiment,
native exports, recapture, debug, pipeline and history),
`talos-query-overlay-ui4/texture/output-comparison.json`,
`query-overlay-ui{3,4,5}.txt` and `build469.log`–`build471.log`.
All results retain incomplete query-isolation provenance. Historical query
values, other texture formats, complete experiments and original-frame
fidelity remain unfinished; no portable runtime acceptance is added here.

### Replay through the captured stream tail

`NativeReplay::prepareFrame` / `replayFrame` and CLI `replay` without `--event`
now include all frame submissions and CPU records through the directory's final
packet. Explicit `--event` retains selected execution semantics; buffer reads
and event performance queries still require an explicit execution. This is a
new runtime path, not a rename of the last submitted command's boundary.

The plan extends queue dependency scheduling, CPU waits, Fence observations,
host mutations and resource experiments to the final record. Every submitted
list instance is included, including empty lists following the last nonempty
execution. Unsubmitted recordings remain unsubmitted. Stream-end replay drains
submitted work at each captured Present and at the stream tail. Present payloads
are checked and retained with original record/swapchain/flags/HRESULT; offscreen
replay does not create a presentation window or claim `displayed=true`.
Selected execution replay keeps its previous drain policy.

The read-only packet audit found substantial tails after the last
ExecuteCommandLists: **3** records for Talos (two Signals and Present),
**25,953** for BF1 (predominantly unsubmitted recordings and CPU updates), and
**136** for Hellblade2 (including two Signals, 59 Fence observations, 14 waits
and Present). All three directories contain two Presents in their frame stream.
Their terminal records must not be inferred from the last GPU execution ID.

One serial GPU run per sample validates this new path with the debug layer:

| Capture | Last execution / stream record | Issued queue operations | Completed submissions | CPU wait drains | Readback creation |
| --- | --- | ---: | ---: | ---: | ---: |
| Talos | 19410 / 192535 | 34 | 16 | 0 | 35118 |
| BF1 | 54186 / 418061 | 4 | 2 | 0 | 11399 |
| Hellblade2 | 37104 / 613510 | 532 | 125 | 46 | 506294 |

All report zero pending queue operations, zero D3D12 validation errors and
`frame_stream_end_reached=true`. Actual SDK version is **616** in these runs.
Talos's strict request fails before device creation because its historical query
values remain missing; only explicit query-output isolation reaches the end.
Its two tail Signals at **192533 / 192534** are present in the issued schedule,
and all **8,294,400** readback bytes match the established final-prefix baseline,
SHA-256 `e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.

BF1 reads the explicitly requested **first** serialized backbuffer at stream
end, **7,749,120** bytes, SHA-256
`c9b2625d366914e6dce7ae9e67e35c28cec46467b8b8a200323801c3ac5a3434`.
This is evidence of complete stream traversal and that resource's readback,
not a new comparison of BF1's final presented backbuffer (**11395**).

Hellblade2 reads the final RGB10 backbuffer, **9,216,000** bytes, SHA-256
`3860808241d51e58d82336ecf1e2819624a7816f64addf4625a1bfb3c91c9f2c`.
Compared offline against saved `hellblade-queue-schedule2` storage, **6,543**
pixels differ, maximum RGB10 deltas **3 / 4 / 5**, with equal alpha. Against
the saved original GPA raw second-Present image, **5,743** pixels differ,
maximum deltas **2 / 4 / 5**, again with equal alpha. These are observed
differences, not a claim of improved fidelity or an attribution to tail handling.
Original concurrency and full Win32 event semantics remain unverified.

Reports use `replay_scope=frame_stream` and `value_time=frame_end`;
raw sidecars carry that scope and texture boundary `frame_end`. Both retain
`frame_complete=false`.
`completed=true` means this requested native operation succeeded, not that the
capture's original output, historical values or full analysis suite passed
acceptance. The native report records the planned/final record and offscreen
Presents. Workspace end-of-stream selection and automatic final-backbuffer
selection were still outstanding at this milestone; the next section records
the workspace connection. CLI callers explicitly choose `--id`.

Builds **472–475** pass. The Talos test initially assumed one Present; its GPU
run succeeded and saved both. After correcting that assumption, the retained
reports/bytes pass **3** checks including setup/cleanup in **63 ms**, with
`FLORA_DX12_FRAME_STREAM_REUSE=1` avoiding another GPU run. BF1 and Hellblade2
each ran once, and their reports/storage were checked offline.

Evidence: ignored `frame-tail-audit1.json`, `frame-stream-evidence1.json`,
`talos-frame-stream1/`, `bf1-frame-stream1/`, `hellblade-frame-stream1/`,
`frame-stream-tests1.txt`, `frame-stream-evidence-tests1.txt`, and
`build472.log`–`build475.log`. No GPA replay, RenderDoc recapture, package run,
or broad regression suite was added for this change.

### Frame-end output in the shared workspace

The DX12 output boundary selector now offers **Frame end**, with no submitted
event selection required. `ReplaySelection::frameEnd` describes this output
request independently of the event retained by analysis panels. Capability
queries check the complete submitted range and the explicit missing-query
policy; Talos remains unavailable in strict mode. The Worker receives a replay
request without `--event`, using the same stream-tail path verified above.
Texture creation/subresource selection remains explicit. Selecting an API
execution switches output back to **After event**, invalidating the old image
and exports. Bound debugger texture navigation also returns to the event
boundary before reading its pixel.

Result acceptance checks both reported and native execution identities, replay
scope, before/after/end boundary and stream-end status. A frame-end result must
name the directory's exact final record as both planned and reached, with no
pending queue operations. Raw storage must carry the same scope, execution and
boundary. Decimal-string identities remain mandatory. Existing request/session/
experiment checks and cancellation still precede publication.

Frame-end pixel inspection displays resource coordinates and display RGBA
without assigning them to the previously selected Draw. In this initial
workspace milestone **Pixel History** was unavailable for this output boundary;
the subsequent milestone below connects it to a verified full-stream capture.
The other event analysis panels retain their own selection.

The targeted Talos workspace check opens the directory with no execution
selected, chooses creation **35118**, enables query isolation and replays
Frame end. It then locates execution **19408**, verifies immediate image/export
invalidation, and replays After event. Both native raw exports contain
**8,294,400** bytes with SHA-256
`e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb`.
Their sidecars correctly differ: `frame_stream` / execution **19410** /
`frame_end` versus `selected_execution` / **19408** / `after`. Neither claims
`frame_complete=true`. Returning to Frame end clears the event image again.

CPU checks reuse the previous real full-stream report to reject incorrect
scope/execution, false stream-end flags, numeric record IDs, a pending queue
operation and an event-boundary storage sidecar. One existing DX11 GF2 workspace
replay still matches its **2560×1440** baseline hash
`2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`;
its boundary label remains **Final** and experiment controls retain their
existing behavior. The focused run passes **5** checks including setup/cleanup
in **12,652 ms**, with two Talos GPU replays and one DX11 replay run serially.
No BF1/Hellblade2 replay was repeated for this UI integration.

Builds **476–478** pass. Evidence: ignored `talos-frame-stream-ui1/`,
`frame-stream-ui-tests1.txt`, the previous `talos-frame-stream1/isolated/`
report, and `build476.log`–`build478.log`. Automatic final-backbuffer selection,
remaining frame-end analysis, original-frame fidelity and full portable acceptance remain
unfinished.

### Pixel History at the frame-stream boundary

Frame-end output now supports Pixel History through a fresh full-stream
RenderDoc capture. After replay drains the stream through its final record and
passes D3D12 validation, the native backend emits a versioned terminal marker
containing the final record, final execution and missing-query policy. Record
and execution identities remain canonical decimal strings. The analysis
backend requires that marker, checks its identities against the request and
rejects mapped execution commands at or after it. History queries select the
terminal marker itself, including operations after the last submitted
execution, rather than selecting the last Draw.

Frame-end and selected-execution analysis captures have separate cache keys.
The workspace requests full-stream recapture without `--event`, verifies its
native boundary report, and rejects history returned for a different boundary
or final record. Frame-end history disables the per-Draw depth-roundtrip
experiment. Switching to an event invalidates old frame-end requests and
output. The result records `output_boundary=frame_end`, `through_record` and
`through_event`; it still declares `frame_complete=false`. Explicit Talos
query isolation retains its incomplete-query provenance.

The focused Talos workspace check queries output creation **35118**, pixel
**(65,172)**, at frame end under query isolation. The fresh capture reaches
record **192535**, execution **19410**; its terminal marker is RenderDoc event
**20544**, after the last mapped execution event **20538**. Three history rows
map to execution **19389** / source record **192489** and the two fragments of
execution **19408** / source record **192508**. PickPixel before history and
after restoration both yield RGBA8 **[94,188,93,184]**, matching native output.
These observations validate that selected texel, not every intermediate
fragment or complete history. The existing final-modification-versus-selected-
event comparison correctly reports `not_compared`: the selected event is the
terminal marker, which does not modify the texture.

The test rejects forged boundary mode/final-record results and rejects the old
request after navigation to execution **19408**. Frame-end and selected-event
raw exports both match the prior **8,294,400**-byte Talos baseline, while their
boundary identities differ. This first GPU run passes **3** checks including
setup/cleanup in **22,029 ms**. A CPU-only marker check passes **3** checks
including setup/cleanup, covering identities above 2^53/at UINT64_MAX, numeric
identity rejection, absent/duplicate markers and mismatched scope/policy.
No BF1/Hellblade2 replay or additional DX11 replay was needed for this change.

Build **479** passes. Evidence remains outside Git:
`talos-frame-history-ui1/{history.json,recapture.json,boundary-evidence.json,frame-end.rdc}`,
the raw exports and their sidecars, `frame-history-ui-tests1.txt`,
`frame-end-marker-tests1.txt`, and `build479.log`. Frame-end history on the other
samples, automatic final-backbuffer selection, original-image fidelity and full
portable acceptance remain unverified or unfinished.

### Suggested final backbuffer from submitted presentation evidence

Opening each sample now selects a suggested frame-end output resource in the
existing workspace, with its inference evidence in the output tooltip and
resource details. It does not start a GPU replay automatically. CLI full-stream
`replay` without `--id` uses the same suggestion; explicit `--id` continues to
override it. Analysis recapture with `--renderdoc` keeps its existing behavior
and does not add an unsolicited texture readback. Reports and raw sidecars
record the selection mode and evidence; `requested_resource` remains null
when no explicit ID was supplied.

The suggestion uses the unique backbuffer whose last submitted transition in
the final successful non-test Present interval changes RT/COPY_DEST to PRESENT
on the swapchain's own queue. It records the resource creation/object, source
record, execution instance, recording generation, submission, queue and Present.
Unsubmitted recordings do not participate; repeated submissions retain their
own execution identities. This is a bounded, cancellable index scan over mapped
record payloads, without reading/copying the resource-storage file.

The result is explicitly **inferred**. It does not equate a live
`GetCurrentBackBufferIndex` observation to a serialized GetBuffer index, certify
displayed pixels or establish original-frame fidelity. Multiple candidates or
presenting swapchains, unresolved recordings, cross-queue/split transitions,
ambiguous buffer lifetimes, frame resizes and submissions after the final Present
leave selection unavailable. Initialization resizes with one unambiguous final
buffer generation are accepted: BF1's GetBuffer records follow one resize and
Hellblade2's follow three. A null aliasing-barrier resource is scoped to
placed/reserved resources, as specified by
[Microsoft's aliasing-barrier documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_aliasing_barrier);
an explicit aliasing reference to a backbuffer still prevents the suggestion.
Manual resource selection remains available.

| Sample | Suggested creation | Transition source / execution | Submission | Final Present |
| --- | --- | --- | --- | --- |
| BF1 | 11395 | 391654 / 54186 | 392108 | 418061 |
| Talos | 35118 | 192530 / 19410 | 192532 | 192535 |
| Hellblade2 | 506294 | 613354 / 37104 | 613374 | 613510 |

All three selections agree with earlier output-resource research. The targeted
CPU/UI check opens the three directories, verifies the selected resource and
frame-end boundary, and confirms that opening has not initiated GPU work.
Minimal fixtures check a differing live index, unsubmitted trailing recordings,
multiple candidates, foreign queues, repeated submissions, work after Present,
high uint64 object identities and cancellation. The focused run passes **4**
checks including setup/cleanup in **1,407 ms**.

One additional BF1 GPU run exercises the newly added CLI default:

```powershell
FloraGPA.Cli.exe replay <BF1-directory> --max-bytes 8589934592 --debug-device --out <new-output>
```

Without `--id`, it reaches record **418061** and reads creation **11395** /
object **2119326144**, yielding **1920×1009 RGBA8**, **7,749,120** bytes, SHA-256
`0527714ad4ea57124f2b05dae3b93994be30f023e763511622183a5158a21480`.
The native report has zero validation errors, `boundary=frame_end`,
`output_selection.mode=suggested` and `frame_complete=false`. This verifies the
new selection/readback path, not equality with the original GPA frame or prior
variable BF1 replay outputs. Talos and Hellblade2 were not replayed for this
selection change.

Build **480** exposed a misplaced anonymous-namespace boundary and was fixed;
builds **481–483** pass. The first two CPU checks respectively exposed an overly
strict generation-one restriction and an overly broad null-alias rejection;
the real initialization records and documented alias scope resolve both.
Evidence: ignored `suggested-output-tests1.txt`–`suggested-output-tests3.txt`,
`bf1-suggested-output1/{report.json,selection-evidence.json,output_storage.bin,output_storage.json}`,
and the corresponding build logs. General presentation selection outside this
conservative inference, complete frame fidelity and full portable acceptance
remain unfinished.

### Full-stream native GPU metrics in the workspace

With the output boundary set to **Frame end**, the existing performance panel
now measures the complete frame stream without requiring a selected Draw or
Dispatch. The CLI exposes the same path as `replay --measure-frame`; it rejects
an explicit event, `--measure-event` or RenderDoc capture in combination with
this mode. Timing-only requests do not automatically read back the suggested
output texture. Explicit resource readback remains available with `--id`.

Each submitted command-list instance has a timestamp bracket, including
repeated invocations. Direct lists additionally receive native pipeline-statistics
queries. Compute/copy lists retain their separately reported timestamp support
and do not acquire invented statistics. Results include the full-stream final
record and execution, list ranges, queue identities, calibrated timing where
available, and trailing queue Signal/Wait observations. The backend must finish
the stream and drain all pending queue operations before the panel accepts the
result. Query-isolated Talos results retain `frame_complete=false` and explicit
missing-query provenance.

The Values tab groups measurements as **Submit / list**, with units and source,
and navigation targets the first execution in that list. A list's aggregate
statistics are not attributed to that first Draw; the sampling scope and
execution count remain in each row and export. The Timeline tab retains
queue-local fallback when calibration is unavailable. No cross-queue duration
sum is presented as a frame time, and Fence rows remain CPU observations with
unmeasured GPU wait duration. Source switching to RenderDoc is disabled for this
native frame-stream mode. Returning to an event invalidates the frame sample
and restores selected-execution measurement.

One Talos workspace GPU run under explicit query isolation reaches record
**192535**, execution **19410**, and reports:

| Evidence | Result |
| --- | --- |
| Submitted-list timestamp brackets | 16, all calibrated in this run |
| Lists with pipeline statistics | 4 Direct lists |
| Other measured list types | Copy lists; no pipeline-statistics claim |
| Exported metric values | 60 |
| Trailing queue observations | Signals at records 192533 and 192534 |
| Final list identity | Submission 192532, ordinal 0, list 1650859184, generation 2 |
| Final list execution range | 16021–19410, 3390 executions |
| Final list IA vertices | 1,425,066, exactly equal to the sum of its captured direct Draw/DrawIndexed vertex/index counts × instance counts |
| Final list IA primitives | 475,022 |
| Final list timestamp interval | 798,528 ticks at 1,000,000,000 Hz = 798.528 µs |

The timestamp interval includes replay instrumentation and is a single sample
with unstabilized clocks. Initialization and experiment-helper submissions are
excluded. It is not original-application frame time, a vendor hardware counter
measurement or proof of restored historical query values. The Draw-argument
oracle validates the selected list's IA vertex count; it does not independently
validate every exported statistic.

The focused GPU/UI check passes **3** checks including setup/cleanup in
**4,526 ms**. It verifies the terminal boundary, list coverage, tail Signals,
raw timestamp arithmetic, IA count oracle, list grouping, filtered navigation
and rejection of a stale sample/wrong scope/final record. Existing selected-event
conversion and queue-clock fallback checks reuse `bf1-native-timeline2.json` and
pass **4** checks including setup/cleanup in **28 ms**, without GPU work.
BF1 and Hellblade2 full-stream metrics have not been measured by this milestone.

Build **484** builds the implementation but exposes a test variable colliding
with Qt's `signals` macro; the variable is renamed and build **485** passes.
Evidence stays outside Git: `talos-frame-metrics-ui1/{result.json,oracle.json,measurement-evidence.json}`,
`frame-metrics-ui-tests1.txt`, `frame-metrics-compat1.txt`, and the build logs.
Original concurrency/timing, hardware-counter coverage, full image fidelity and
portable acceptance remain unfinished.

### Full feature acceptance

| Requirement | BF1 | Talos | Hellblade2 |
| --- | --- | --- | --- |
| Open/index in shared workspace | Passed | Passed | Passed |
| Frame-end output in workspace | Available; not separately GPU-checked in this UI milestone | Frame-end → event → frame-end invalidation, exact raw exports and distinct boundary provenance verified under query isolation | Available; not separately GPU-checked in this UI milestone |
| Frame-end Pixel History | Not verified | Fresh full-stream capture, terminal marker, three mapped fragments, native selected-texel agreement and stale-boundary rejection verified under query isolation; full history unfinished | Not verified |
| Suggested final output resource | Workspace selects 11395; CLI default GPU readback verified | Workspace selects 35118; prior native readback evidence reused | Workspace selects 506294; prior native readback evidence reused |
| Full-stream native performance | Not verified | 16 timed/calibrated submitted lists, 4 Direct-list statistics, trailing Signals, exact final-list IA vertex oracle and scope/navigation rejection verified under query isolation | Not verified |
| CLI replay through stream end | Reached record 418061; unsubmitted tail recordings stay unsubmitted; selected first-backbuffer readback verified | Reached record 192535 with two trailing Signals under explicit incomplete-query isolation; full readback matches final-prefix baseline | Reached record 613510, 532 queue operations and 46 CPU wait drains; final RGB10 readback obtained, original-image differences remain |
| Submitted-list identity/navigation | Structural checks passed | Structural checks passed | Structural checks passed |
| Replayed pipeline/binding inspection | Tone-map shader inputs, descriptor ranges and output creation mapped; full state unfinished | PSO/root creation navigation and original/replaced VS ownership/digests verified; direct stage import and stale-result rejection verified; shader-constant navigation and query-isolated final Draw bindings/PSO/resource navigation verified; full state unfinished | LensSoftnessPS output, three input resources, viewport/scissor and execution provenance mapped; existing RDC lacks new PSO/root markers; full state unfinished |
| Native initialization buffer copy | One isolated copy verified | One isolated copy verified | One isolated copy verified |
| Captured initialization submissions | 52 executed; selected buffer verified | 25 executed; selected buffer/texture verified | 26 executed; selected buffer/texture verified; tile transfers executed |
| Complete native frame and selected-event replay | Last indexed submission reached; backbuffer replay available in workspace; first Draw/UAV clear verified; full frame semantics/fidelity unfinished | First Draw before/after available in workspace; first color boundary verified; explicit workspace query-output isolation reaches original last execution 19410, exports marked incomplete storage and rejects unknown readbacks; historical values and full frame fidelity unfinished | Last indexed submission reached through execution 37104; copied RGB10 backbuffer read/display verified; first Draw float boundary verified; output variation, full frame fidelity and general synchronization unfinished |
| Resource/Pixel History with execution mapping | Color history connected to PS; partial buffer/texture resource usage, creation provenance, captured barrier details and real shadow-buffer UAV Clear range/navigation/export verified; full histories unfinished | Patched single-fragment D16 depths verified; two-layer modification verifies mapped fragments and separated-depth rejection/pass; selected D32 Equal Draw refresh preserves original depth bytes and aligns its mapped color-history boundary with native readback; query-isolated final-prefix resource usage, partial Resolve byte ranges/source navigation and policy rejection verified; isolated output pixel selection, three mapped color-history fragments, exact final texel boundary, filtered execution navigation and PS invocation handoff verified; exact D16 intermediate storage, other failure reasons and full histories unfinished | One mapped float pixel modification matches native before/after; LensSoftnessPS input has 11 mapped usage entries; final-color fragment query rejects changing replay as unverified; full histories unfinished |
| VS/PS/CS debugging and geometry | DXBC VS, PS and selected CS traces/workspace paths checked; nonzero DXBC shared-memory group IDs fixed with an exact GPU reproduction; selected temporal CS stores match the correct GPU replay phase after format truncation; GPU DXBC dot products correct the selected VS position W mismatch and all four position components match post-VS readback; GPU MAD corrects TEXCOORD4.xy while TEXCOORD2.xyz still varies across GPU reads; transient replay variation, geometry and full stage coverage unfinished | Original and modified DXBC VS stepping/Watch, post-VS mesh and exact geometry restoration after undo verified in workspace; input float3 and original index bytes match captured initialization copies; selected filter PS instruction breakpoint/stepping and mapped Watch verified, with traced discard-border sample and native-policy GPU alpha agreement; corrected Cube direction gives selected PS GPU shader-output agreement within one float32 ULP and exact stored values after observed binary16 truncation; query-isolated final Draw VS stepping, breakpoint and Watch verified, with all four output semantics matching the selected GPU reference; selected isolated PS trace matches all four GPU output words, and a reproduced BGRA intermediate-history channel swap is corrected; isolated VSIn/VSOut tables, selected vertex/input bytes, 276-triangle exports with provenance and VS invocation handoff verified; inputless replacement PS auxiliary-signature failure fixed with a same-capture reproduction and exact four-component output; remaining stages/formats unfinished | One DXIL SM6.6 PS verified against GPU shader output; MainPS contraction-sensitive dither mismatch reproduced and explicitly reported, numerical fix unfinished; selected indirect CS RGB stores match their corresponding GPU phases; retained DXIL inputs fix the selected downsample comparison, while earlier replay output and full input fidelity remain unverified; CS trace/breakpoint/SSA Watch verified; first MeshOut positions/indices, table and OBJ verified; full UAV validation, other stages and complete geometry unfinished |
| GPU performance analysis and modification experiments | One selected Dispatch's native timestamp/statistics and 13-list calibrated replay timeline verified in workspace; replacement CS writes exactly checked output through the existing experiment overlay; targeted VS shadow-factor experiment restores the full particle attachment to original GPA bytes and undo restores the native/barrier output; original concurrency, full performance and experiments unfinished | Workspace Draw disable, VS replacement and constant-buffer patch change GPU output; undo restores exact bytes; D16 texture import/Clear boundaries verified; project persistence and stale-result rejection verified; selected query-isolated Draw timestamp/statistics and 16-list calibrated prefix timeline verified with incomplete-frame provenance; isolated final Draw disable preserves the preceding pixel and undo restores every output byte, with project policy persistence, export experiment identity and stale-analysis rejection verified; isolated PS replacement and alpha-buffer patch have native output, fresh debug/pipeline identity and full-storage undo evidence; isolated typeless BGRA8 texture import has exact full-input readback, explicit preview interpretation, modified pixel history, context-safe navigation and exact full-output undo evidence; full experiments and performance unfinished | Selected compute Dispatch timestamp and 72-list/two-queue calibrated native timeline measured; four Fence dependency brackets verified, no overlap observed; changed DXIL PS produces an exact full-viewport constant-color image; indirect CS replacement produces 295,552 exact marker texels and undo restores the selected original texel; whole-image undo has unresolved raw variation; full stage/resource experiments and performance unfinished |
| Comparison with original GPA frame/intermediate resources | Output and tone-map boundary compared; raw R11G11B10 main input differs at its producer before tone mapping; original and independent variation remain unexplained; screenshot alpha forcing identified; original RGBA16F particle boundary differs at 26,473 RGB pixels with all Alpha words equal; recovered original shadow buffer and a 12-vertex diagnostic shader correction account for all bytes at that boundary, while state-correct third-Draw and final-copy readbacks match the selected native results; original final-copy runs also vary at those 12 factors, and default full-frame fidelity remains unfinished | First color RGB exactly matches original playback; screenshot alpha forcing identified; raw-alpha/full/intermediate comparisons unfinished | Second original Present compared as raw RGB10: 6,402 pixels differ, maximum channel delta 4; screenshot high-eight-bit conversion and serialized backbuffer rotation identified; full/intermediate fidelity unfinished |
| Full portable replay/analysis without GPA/Python | Packaged SDK-602-requested UAV Clear readback matches baseline; module snapshot verified; full acceptance unfinished | Packaged isolated native replay, fresh recapture and mapped Resource History verified with module snapshots; missing query values and full acceptance unfinished | Not verified |

No unfinished item above has been attributed to a hardware limitation. Known
implementation gaps include historical query state, complete Talos execution
semantics beyond the strict prefix, full resource lifetime and address-alias handling,
complete command arguments, dependency scheduling and cross-queue synchronization.
The isolated analysis backend now accepts native DX12 captures and execution
identities. Selected replay/readback, VS debugging and Replay Mesh are connected
to the workspace. Pixel History has a verified partial D16 path; full history,
performance and experiment integration remain unfinished.
