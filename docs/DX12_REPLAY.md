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
resource retains varying alpha. The source of that difference is not established,
so RGBA equality and complete-frame correctness are not claimed. The screenshot
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
for the first / last output. As for Talos, the reason for the alpha difference
has not been established. GPA also logs a missing capture-root D3D12Core.dll and
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
`inspectObservationWaits` proves this bounded property before initialization and
reports the two waits explicitly. It does not synthesize an event signal or
weaken GPU queue/Fence checks. Any later state-changing operation on the same
thread, including in another stream file, fails preflight. Extending the prefix
to submission 579109 currently fails at event registration 578875; CPU event
signalling/scheduling remains unfinished. This is verified by CPU inspection,
without another heavy GPU run.

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
real next-submission dependency failure are covered by focused tests. This does
not yet verify Hellblade2's complete workspace request flow, VS/CS/MS debugging,
geometry, full histories, performance or modification experiments.
Release GUI, CLI, Worker and the focused test target build successfully
(`build106.log`). All four focused cases pass (`hellblade-final-checks1.txt`);
these final checks reuse the trace and inspect CPU records without another GPU
replay. No unrelated DX11/Talos/BF1 GPU regression run was added for this change.

### Full feature acceptance

| Requirement | BF1 | Talos | Hellblade2 |
| --- | --- | --- | --- |
| Open/index in shared workspace | Passed | Passed | Passed |
| Submitted-list identity/navigation | Structural checks passed | Structural checks passed | Structural checks passed |
| Native initialization buffer copy | One isolated copy verified | One isolated copy verified | One isolated copy verified |
| Captured initialization submissions | 52 executed; selected buffer verified | 25 executed; selected buffer/texture verified | 26 executed; selected buffer/texture verified; tile transfers executed |
| Complete native frame and selected-event replay | Last indexed submission reached; backbuffer replay available in workspace; first Draw/UAV clear verified; full frame semantics/fidelity unfinished | First Draw before/after available in workspace; first color boundary verified; full frame blocked by historical query state | First submitted Draw before/after raw float readback verified; full frame and CPU event synchronization unfinished |
| Resource/Pixel History with execution mapping | Color history connected to PS; partial buffer/texture resource usage, creation provenance and captured barrier details verified; full histories unfinished | Clear/Draw history and D16 event-boundary values verified from workspace pixel pick; fragment depths unavailable; full history unfinished | One mapped float pixel modification matches native before/after; full histories unfinished |
| VS/PS/CS debugging and geometry | DXBC VS, PS, direct/indirect CS traces verified; selected PS and CS workspace paths checked; geometry and full stage coverage unfinished | DXBC VS debugging/Watch and post-VS mesh/OBJ verified in workspace; remaining stages unfinished | DXIL SM6.6 PS trace, instruction breakpoint and Watch verified against GPU shader output; other stages and geometry unfinished |
| GPU performance analysis and modification experiments | One selected Dispatch's native timestamp and pipeline statistics verified in workspace; full performance and experiments unfinished | Unfinished | Unfinished |
| Comparison with original GPA frame/intermediate resources | Both output frames compared; small RGB and widespread alpha differences remain unexplained; intermediate comparison unfinished | First color RGB exactly matches original playback; alpha differs; full/intermediate comparisons unfinished | Not performed |
| Full portable replay/analysis without GPA/Python | Not verified | Not verified | Not verified |

No unfinished item above has been attributed to a hardware limitation. Known
implementation gaps include historical query state, frame execution coverage
beyond the verified Talos prefix, full resource lifetime and address-alias handling,
complete command arguments, dependency scheduling and cross-queue synchronization.
The isolated analysis backend now accepts native DX12 captures and execution
identities. Selected replay/readback, VS debugging and Replay Mesh are connected
to the workspace. Pixel History has a verified partial D16 path; full history,
performance and experiment integration remain unfinished.
