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
calls; native frame copy execution remains to be verified. Hellblade2's new
descriptor path has CPU decoding evidence only. Targeted checks also verified
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
Workspace analysis controls, PS/CS/DXIL validation, complete Resource History,
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
UI integration. Shader/debug/history/geometry controls in the workspace still
need the isolated DX12 analysis connection; successful selected replay is not
acceptance of those features.

The equivalent CLI run in `talos-workspace-cli1` completed with zero D3D12 debug
errors. `selectedReplayStorageExport` read that actual Worker-compatible report,
validated its display PNG/RGBA pair, exported native storage through the existing
bounded exporter, and verified all 8,388,608 bytes against SHA256
`c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217`.
The exported pixel remains exactly `17980`; its sidecar retains creation
`167081` and subresource `1`. See `dx12-storage-export1.txt`. Release CLI, Worker,
GUI and targeted test executables build successfully.

### Full feature acceptance

| Requirement | BF1 | Talos | Hellblade2 |
| --- | --- | --- | --- |
| Open/index in shared workspace | Passed | Passed | Passed |
| Submitted-list identity/navigation | Structural checks passed | Structural checks passed | Structural checks passed |
| Native initialization buffer copy | One isolated copy verified | One isolated copy verified | One isolated copy verified |
| Captured initialization submissions | 52 executed; selected buffer verified | 25 executed; selected buffer/texture verified | 26 executed; selected buffer/texture verified; tile transfers executed |
| Complete native frame and selected-event replay | Unfinished | First Draw before/after available in workspace; first color boundary verified; full frame blocked by historical query state | Unfinished |
| Resource/Pixel History with execution mapping | Unfinished | First Clear/Draw mapped; D16 event-boundary values verified, fragment depths unavailable; full history unfinished | Unfinished |
| VS/PS/CS debugging and geometry | Unfinished | One DXBC VS trace and post-VS mesh verified; remaining stages/UI unfinished | Unfinished |
| GPU performance analysis and modification experiments | Unfinished | Unfinished | Unfinished |
| Comparison with original GPA frame/intermediate resources | Not performed | First color RGB exactly matches original playback; alpha differs; full/intermediate comparisons unfinished | Not performed |
| Full portable replay/analysis without GPA/Python | Not verified | Not verified | Not verified |

No unfinished item above has been attributed to a hardware limitation. Known
implementation gaps include historical query state, frame execution coverage
beyond the verified Talos prefix, full resource lifetime and address-alias handling,
complete command arguments, dependency scheduling and cross-queue synchronization.
The isolated analysis backend now accepts native DX12 captures and execution
identities. Selected replay/readback is connected to the workspace; its analysis
workflow still requires integration.
