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

`inventory`, `commands`, `command-lists`, `replay` and `validate-frame` accept DX12 directories. Validation
reports `blocked` while complete frame replay and feature acceptance remain unfinished;
the selected native execution and isolated analysis paths below are available for investigation.
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

The workspace reports the first blocking resolve as `MissingData`, including
its execution/source and exact missing count. Before execution 16001 remains
selectable; after 16001 and later prefixes report the gap. Execution 16000 is
still available. Analysis captures and performance require the selected event
to be included even when the output view selects a before-event boundary.
Malformed query records report a separate failure. Existing native deferred
query checks remain as an execution-time guard.

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

The last recorded `GetCurrentBackBufferIndex` returns 3, but that captured buffer
has no frame-thread writes and its native readback is zero (`hellblade-last-color1`).
Following actual GPU commands instead identifies CopyResource **613353**, copying
to object **2149283441408**, creation **506294**, swapchain buffer index 1. Readback
after execution 37104 produces the visible scene, with the character, rocks and
letterbox bars, from that resource. The relation between the observed swapchain
index and copied buffer remains to be reconciled with original GPA behavior.

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

### Full feature acceptance

| Requirement | BF1 | Talos | Hellblade2 |
| --- | --- | --- | --- |
| Open/index in shared workspace | Passed | Passed | Passed |
| Submitted-list identity/navigation | Structural checks passed | Structural checks passed | Structural checks passed |
| Replayed pipeline/binding inspection | Tone-map shader inputs, descriptor ranges and output creation mapped; full state unfinished | Workspace shader-constant resource navigation and event/backend invalidation verified; full state unfinished | Not verified |
| Native initialization buffer copy | One isolated copy verified | One isolated copy verified | One isolated copy verified |
| Captured initialization submissions | 52 executed; selected buffer verified | 25 executed; selected buffer/texture verified | 26 executed; selected buffer/texture verified; tile transfers executed |
| Complete native frame and selected-event replay | Last indexed submission reached; backbuffer replay available in workspace; first Draw/UAV clear verified; full frame semantics/fidelity unfinished | First Draw before/after available in workspace; first color boundary verified; full frame blocked by historical query state | Last indexed submission reached through execution 37104; copied RGB10 backbuffer read/display verified; first Draw float boundary verified; output variation, full frame fidelity and general synchronization unfinished |
| Resource/Pixel History with execution mapping | Color history connected to PS; partial buffer/texture resource usage, creation provenance and captured barrier details verified; full histories unfinished | Patched single-fragment D16 depths verified; two-layer modification verifies mapped fragments and separated-depth rejection/pass; exact D16 intermediate storage, other failure reasons and full histories unfinished | One mapped float pixel modification matches native before/after; full histories unfinished |
| VS/PS/CS debugging and geometry | DXBC VS, PS, direct/indirect CS traces verified; selected PS and CS workspace paths checked; geometry and full stage coverage unfinished | Original and modified DXBC VS stepping/Watch, post-VS mesh and exact geometry restoration after undo verified in workspace; input float3 and original index bytes match captured initialization copies; remaining stages/formats unfinished | DXIL SM6.6 PS verified against GPU shader output; CS trace/breakpoint/SSA Watch verified; first MeshOut positions/indices, table and OBJ verified; UAV write validation, other stages and complete geometry unfinished |
| GPU performance analysis and modification experiments | One selected Dispatch's native timestamp/statistics and 13-list calibrated replay timeline verified in workspace; byte-preserving CS import creates a PSO; original concurrency, full performance and experiments unfinished | Workspace Draw disable, VS replacement and constant-buffer patch change GPU output; undo restores exact bytes; D16 texture import/Clear boundaries verified; project persistence and stale-result rejection verified; full experiments and performance unfinished | Byte-preserving MS/PS import creates a PSO; changed shader execution, performance and full experiments unfinished |
| Comparison with original GPA frame/intermediate resources | Output and tone-map boundary compared; raw R11G11B10 main input differs at its producer before tone mapping; original and independent variation remain unexplained; screenshot alpha forcing identified; full fidelity unfinished | First color RGB exactly matches original playback; screenshot alpha forcing identified; raw-alpha/full/intermediate comparisons unfinished | Not performed |
| Full portable replay/analysis without GPA/Python | Not verified | Not verified | Not verified |

No unfinished item above has been attributed to a hardware limitation. Known
implementation gaps include historical query state, frame execution coverage
beyond the verified Talos prefix, full resource lifetime and address-alias handling,
complete command arguments, dependency scheduling and cross-queue synchronization.
The isolated analysis backend now accepts native DX12 captures and execution
identities. Selected replay/readback, VS debugging and Replay Mesh are connected
to the workspace. Pixel History has a verified partial D16 path; full history,
performance and experiment integration remain unfinished.
