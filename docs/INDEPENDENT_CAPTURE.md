# Independent DX11 capture

Implementation is **partial**. The native controller now launches, injects and
captures a limited blit/flip DX11 profile through private IPC. Completed
`.gpa_frame` files execute in FloraGPA and the installed original GPA kernel.
Unsupported profiles fail explicitly. Qt exposes the same workflow and passes
only completed file paths into replay. The full planned release, original GUI
inspection and GF2 six-frame gate remain incomplete. GF2 passes its two-buffer
FLIP_SEQUENTIAL / ALLOW_TEARING boundary and mapped writes. Limited pre-frame
EVENT observations now serialize; in-frame successful completion still rejects.
The elevated continuation resolved game-cache access. Colored GF2 start-screen
preflights replay exactly; the main scene still refuses MSAA sample preservation,
and formal scene acceptance remains 0/6.

Capture HUD work is tracked separately in [CAPTURE_HUD.md](CAPTURE_HUD.md).
Protocol v3 adds configurable HUD/hotkeys and Qt settings. Drawing is currently
limited to guarded single-buffer blt devices; flip/multi-buffer chains (including
GF2) skip HUD because pixel isolation is not yet established. This limitation
does not disable the in-game capture shortcut or expand capture format support.

## Architectural contract

`FloraGpaFormat` is a C++20 wire contract. `FloraGpaReader` owns checked, immutable
mapped container storage. `FloraGpaWriter` writes container transactions to a
same-directory temporary file and publishes without replacing existing output.
Neither library links Qt, D3D, capture, replay or application analysis.

`FloraCore::Frame` retains resource interpretation, context recovery, hashing
and edited-view overlays; it shares the immutable reader between overlays.
Replay links the reader, not the writer. Existing RenderDoc recapture remains
an optional replay-analysis feature.

The native capture controller/runtime and encoder must not include or
link FloraCore, FloraReplay, FloraExperiment or Qt. They use their own recording
model; only pure wire layouts may be shared. A configure-time transitive target
check and the `capture_boundaries` CTest enforce these constraints. Runtime
module and standalone-package audits remain separate evidence.

The Qt capture client belongs to the application composition layer. It uses
asynchronous CLI calls and shares only a completed file path with replay. It
never handles target COM pointers or writes GPA records.

The full profile exposes **File > Capture Application…**. The dialog supplies
application/arguments/working directory/output, swap-chain observations, launch
capture/cancel/disconnect controls, and a collapsed task log. Capture is enabled
for available profiles; Open Capture requires a completed, existing frame file. The session persists when
the modeless dialog is hidden; disconnect is explicit and does not kill the game.
The controller path is the app-local `FloraCapture.exe`.

The Qt client suite passes malformed/oversized/conflicting CLI responses,
concurrent request rejection, target exit and a real native-controller lifecycle.
The full-profile run has no skips; standalone UI-only tests intentionally omit
the native-controller case when capture binaries are not configured. The full
GUI and Worker compile, and the existing view-overlay tests pass. None of this
constitutes acceptance of the unimplemented frame pipeline.

## Historical foundations and remaining gates

The C++20 native bootstrap uses pinned Detours 4.0.1 and the existing repository
nlohmann/json 3.12.0. The upstream Detours target alone needs MSVC permissive mode;
Flora capture sources use strict conformance. There are no runtime Python or GPA
dependencies. Controller sessions validate PID and process creation time, and
their local named pipe admits only the current user. Disconnect leaves transparent
hooks resident until normal target exit.

CreateDXGIFactory/1/2 now wrap Factory through Factory7 and interpose legacy,
HWND and composition swap-chain creation. Swap-chain GetParent preserves the
factory wrapper identity. CoreWindow creation forwards through the same wrapper
but has not been exercised by a real CoreWindow oracle. Device, Context, resource,
view, shader, state, CommandList and related DXGI forwarding now use a shared
COM registry. The checked API description has 37 families, 78 public interfaces
and 330 declared methods; this is forwarding coverage, not recording coverage.
Unknown interface escape never enables frame capture and reports an API/IID gap.

Earlier bootstrap validation: 19 swap-chain comparisons, 24 factory
comparisons and the `capture_bootstrap` CTest pass. These include native/injected
pixels, COM identities, reference observations, concurrent QI, TEST Present,
failed/successful resize and explicit frame-request refusal. Local results are
`artifacts/capture-bootstrap/{swap-chains-final,factories-final}/`.

The object-forwarding iteration passes seven capture-only CTests, 24 factory
comparisons and 20 updated swap-chain checks. The independent WARP application
compares native/injected device and context versions, D3D11/DXGI identity,
concurrent QueryInterface, GetDevice/GetResource, private interface data, input
and getter arrays, dynamic Map data, Dispatch values, deferred Draw reuse and
ExecuteCommandList state restoration. Its 36 application wrapper lifetimes all
close. The registry unit test also exercises successful unknown interfaces,
failed queries, LastError preservation and native references outliving wrappers.
Full-profile capture tests, including Qt controls, pass as well.

Native interface caches hold real COM references; diagnostic AddRef/Release
numbers are not expected to match an unwrapped device. Application-visible
IUnknown identity is stable across implemented interfaces while references are
live. Persistent capture identities now use native lifetime metadata, separately
from application wrapper generations (see below). Returned unsupported interfaces remain native and their
subsequent calls/identity queries are outside tracking. This keeps stage C partial.

The generator is native C++20 and emits only into the build directory. See
`src/capture/api/README.md` for ABI description and array-handling rules. Neither
the generator nor the test programs are included in the capture package.

GF2 object-forwarding diagnostic evidence is in `artifacts/capture-objects-gf2-1`.
Two observations show 130,216 then 213,006 successful Presents, 138 created
wrappers and no reported interface gaps or swap tracking failures. Capture was
rejected, disconnect succeeded and the game exited through CloseMainWindow.
This tests that launch path only; no scene or image comparison was performed,
and no generated frame is accepted. The diagnostic preceded subsequent forwarding
hardening edits and did not retain a hash of its exact runtime binary.

## Native identity and creation metadata

The capture-owned `ObjectRecord` is independent of replay and format records.
A private IUnknown token on each supported native object preserves its ID while
driver bindings retain the object after all application wrappers have gone.
The token owns metadata only: it has no native object/device/wrapper reference.
Native destruction retires the record; retaining its CPU metadata does not keep
the GPU object alive. Reusing a native address creates a new ID. This follows
the public [SetPrivateDataInterface lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-setprivatedatainterface).

Metadata holds actual resource/view/state descriptors, device IDs, view resource
IDs, device creation flags, context kind/flags, buffer creation bytes, shader
DXBC and class-linkage IDs, owned input-layout semantics/signatures, and owned
stream-output declarations/strides. Null SO gap semantics and absent buffer
initial data remain distinct. Captured buffer creation bytes are not a frame's
initial GPU snapshot: later Map/Update/Copy/Draw operations can change them.
Class-instance details, command-list contents and several other creation paths
remain incomplete. No frame can be published yet.

Initialization is serialized per native identity; descriptor queries and token
installation do not hold the global metadata registry lock. Token destruction
does not enter the wrapper registry. Failed storage and application access to
the reserved lifetime GUID report a capability gap while preserving native API
results. A pre-existing private-data value under that GUID is never overwritten. Unsupported native private-data storage is not silently treated as a
persistent identity.

`capture_metadata` verifies real WARP resource/shader bindings across wrapper
destruction, concurrent rewrapping, copied buffer/DXBC data after caller memory
changes, exact compute results and native teardown without token cycles. It
recreates InputLayout and SO GeometryShader objects from retained metadata after
their originals are released, then checks Draw-generated stream bytes exactly.
The registry test also covers deterministic native address reuse, concurrent
failed initialization/retry, a false-success native setter and reserved-key
overwrites. The initial old mock lacked real private-interface retention; it
was corrected to implement that COM contract before this gate passed.

This iteration passes eight capture-only CTests, six full-profile capture tests
(including ten Qt cases), 24 factory checks and 20 swap-chain checks. GPU tests
run serially. GF2 diagnostics in `artifacts/capture-metadata-gf2-1` retain the
runtime hash before launch: two samples observe 94,414 then 225,060 successful
Presents, 138 native records created, 12 retired and no reported interface gaps.
Capture rejects, disconnect succeeds and CloseMainWindow exits the process;
no process exit code was recovered. This diagnostic preceded the final
pre-existing-GUID guard; its exact older runtime hash remains recorded. No scene/pixel or dual-player acceptance is
claimed, and accepted live GF2 captures remain zero.

An earlier build launched the specified GF2 executable and observed two successful
device creations and a 2560x1440 swap chain with 43,564 successful Present calls,
without hook/registry errors. GetBuffer exposed the known missing resource wrapper.
Capture was explicitly rejected, the session disconnected and the game closed
normally. This is an injection diagnostic, **zero accepted captured frames**.
Its local report is `artifacts/capture-bootstrap/gf2-diagnostic-1/status.json`;
the later controller session-identity and JSON dependency changes were validated
with self-owned probes, not another GF2 run.

## Resource readback foundation

`runtime/resource_data` owns texture creation subresources and current GPU
readback data independently of replay and format records. Creation hooks copy
all mip/array/volume bytes before returning to the caller, stripping row/slice
padding. Successful Map/Unmap calls track active maps without retaining mapped
pointers; snapshots explicitly reject application-mapped resources. This is not
yet a recording of Map writes or Update/Copy commands.

At an exclusive application-context boundary, `snapshot_resource` copies a
buffer or single-sample texture into native staging storage and reads its actual
contents. It supports the tested ordinary, BC, depth/stencil and NV12/P010/P016
layouts; unknown layouts, MSAA, shared/tiled/protected resources and other special
flags fail explicitly. Byte budgets, checked pitch arithmetic, cancellation,
device errors and polling deadlines bound readback. A deadline cannot interrupt
a driver call that itself hangs. Internal objects never enter the application
registry. Scoped predication suppression preserves the exact predicate/BOOL,
bindings and LastError, including exception paths. It follows the public
[CopyResource restrictions](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
and [mapped pitch contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_mapped_subresource).

`snapshot_uav_counter` separately reads each APPEND/COUNTER view's hidden value
with CopyStructureCount. Two views sharing one buffer retain distinct counter
identities; neither their creation parameters nor KEEP values stand in for GPU
data. These helpers require the caller to synchronize all contexts that can
modify the source; no Present safe point or cross-context barrier is implemented
yet. They are never invoked by the IPC service thread and are not connected to
frame capture, encoding or publishing. SO append position, live queries,
predicate reconstruction and sample-preserving MSAA remain unresolved.

The new WARP and hardware CTests compare bytes to independent application input,
not to the writer/reader. They cover padded subresources, dynamic Map/Update,
depth/stencil values, predicate restoration, preserved viewport/depth/UAV
bindings, cancellation/retry and two GPU-modified counters. WARP supports P016
and its bytes match; this machine's hardware rejects both P016 format support
and texture creation. The hardware test records that rejection, not a successful
P016 snapshot. An initial counter fixture incorrectly bound two write views of
one buffer simultaneously; after separating the dispatches, both devices read
the expected independent counters (5 and 7). The restriction is documented in
the public [UAV binding rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews).

This gate passes ten capture-only CTests, eight full-profile capture CTests
(including ten Qt cases), 24 factory and 20 swap-chain comparisons. Local evidence
is `artifacts/capture-resource-validation.json`. GF2 diagnostic samples observe
67,800 and 164,810 Presents with zero reported registry gaps; capture rejects,
disconnect succeeds and CloseMainWindow exits (exit code unavailable). The Unity
log contains managed exceptions, so this establishes neither gameplay nor scene
correctness. The first attempt exited because its Unity log path collided with
the controller's target.log; both attempts and the exact runtime hash are retained.
Accepted GF2 frames remain zero.

## Bound pipeline state

`runtime/context_state` samples immediate-context bindings into capture-owned
`PipelineBindings`, independently of replay and the GPA wire state block. It
reads all six shader stages, constant-buffer ranges, SRVs/samplers, IA buffers
and offsets, topology, rasterizer/viewports/scissors, OM states and factors,
RTV/DSV bindings, CS/OM UAV slots and resource minimum LOD. UAV slot limits follow
the sampled feature level (8 at 11.0, 64 at 11.1+), rather than interface version.
Constant ranges use the native Context1 getters when available. Separate OM UAV
queries avoid losing slots below the eight-RTV array capacity.

The runtime-side snapshot retains canonical native references, deduplicated by
persistent capture ID, and follows each view to its resource. These temporary
leases keep the sampled objects available for later initial-data collection;
the persistent metadata records still own no native references. Getter results
are released per returned slot, including aliases and exception paths. Only the
value model is intended for eventual encoding; native leases belong to capture.

Bound SO buffers, predicates, dynamic shader linkage and hardware protection
produce explicit reconstruction gaps. In particular, SOGetTargets exposes no
recoverable absolute append position; the getter's append convention is not
treated as captured data. Active queries, unbound objects' hidden state,
resource bytes and UAV counter values are outside this binding snapshot and
require the separate frame assembler. The helper requires an exclusive calling
context, rejects deferred contexts and pre-11.0 feature levels, and does not
establish multicontext synchronization or command-list execution ordering.

Native WARP/hardware tests exercise both 11.0 and 11.1. The test-only consumer
rebinds captured values after the original application references are released,
then checks indexed Draw pixels and Dispatch integers against independent CPU
expectations. Tests cover six stages' high slots, offset constant buffers,
ResourceMinLOD, CS slot 63, OM slot 3, independent counters remaining unchanged,
explicit SO/predicate/deferred gaps, LastError and native lifetime cleanup.
An initial 0.5-to-UNORM color oracle exposed hardware rounding to 127 versus
WARP's 128. That failure is retained; the final integer oracle uses exact color
endpoints (yellow shader output blended to green), with no comparison tolerance.
No file replay, Present integration or live-frame acceptance is claimed.

This iteration passes twelve capture-only CTests and ten independent-capture
full-profile CTests, including ten Qt cases. The selected full-profile run also
passes two existing capture-structure tests. Evidence and hashes are summarized
in `artifacts/capture-context-validation.json` and the development baseline.
No new GF2 diagnostic is claimed for this standalone helper.

## Intercepted command model

Generated Context proxies now observe calls before native execution. A temporary
`FrameRecording` scope assembles capture-owned commands, pipeline values, object
references and first-use GPU resource contents. Supported command kinds are
Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, Dispatch, ClearRTV,
ClearDSV and uint/float ClearUAV. Scalar arguments (including signed base-vertex
bits) and clear arrays are owned immediately. Binding setters are folded into
the actual pipeline state at each GPU command; they are not yet serialized as
individual setter events. Resources are sampled before their first recorded
read/write, rather than taking final images or assuming creation bytes remain
current. Each command also samples bound/explicit UAV counters, including after
KEEP or reset bindings.

This recorder is intentionally not connected to the controller or Present yet.
Its caller must establish an exclusive single-thread immediate-context boundary.
Other contexts/threads, reentry, deferred/indirect execution, Map/Update/Copy and
other unimplemented GPU commands reject the whole model with an API and reason.
Query Begin/End and DXGI mapping lifetimes are tracked outside recording, so an
active query or DXGI mapping prevents starting. Surface/DC mapping, device direct
subresource writes and resource offer/reclaim paths during recording also reject.
Foreign/reentrant callers mark failure without waiting for the recording mutex,
avoiding a wait behind readback while holding a driver's multithread lock. This
is not a general cross-context capture barrier.

Only one scope can be active, repeated start returns busy, cancellation stops
collection, and finish detects device loss and interface gaps. Recording errors
do not suppress the native call or replace its HRESULT/LastError. Command counts
and cumulative initial-resource bytes are bounded. Native staging/getter calls
do not enter the event stream. First-use sampling is valid only within this
restricted observed scope: unsupported resource mutations cannot be silently
omitted. File transactions, target exit and cancel/publication races remain the
future session/encoder responsibility.

The WARP/hardware command tests exercise the actual generated COM observers.
They record eight calls, release every source native reference and confirm that
all original GPU objects have died. A fixture-scoped independent test consumer
then creates a new WARP device from CPU descriptors, DXBC and initial bytes and
executes the recorded commands. Draw pixels and Dispatch integers match CPU
oracles exactly; removing all Draws or Dispatch produces the expected negative
result. The initial image is blue while the final image is green, and a source
buffer updated before recording contains 17 rather than its creation values.
Separate counter checks observe 3, 5 and 1 before successive Dispatch calls,
not guessed KEEP values. Failure tests cover limits, busy/cancel/retry,
abandonment, active queries, DXGI maps, foreign threads, reentry and unsupported
CopyResource while verifying that the native copy still executes.

This proves a limited command model, not an injected CLI capture or GPA file.
The test consumer neither implements the product replay nor invokes it. The
encoder, full-frame Present transaction, expanded mutation/command-list support
and dual-player/GF2 acceptance are still pending. Controller capture remains
explicitly unavailable, and accepted GF2 frames remain zero.

Fourteen capture-only CTests, twelve independent-capture full-profile CTests
(ten Qt cases), 24 factory comparisons and 20 swap-chain comparisons pass. Added
unbound depth/float-clear cases also pass in both build profiles and on both
source devices. Evidence is `artifacts/capture-stream-validation.json`; native
output and input hashes are retained separately. No new GF2 diagnostic or
dual-player file test is claimed for this command-model iteration.

## Build and control

```powershell
cmake --preset capture-only
cmake --build --preset capture-only-release
ctest --preset capture-only-release
./tools/package_capture.ps1 -OutputDirectory out/capture-bootstrap-new
```

Capture-only configure/build does not find Qt or build replay. Replay-only and
full composition configurations are `replay-only` and `capture-full`; supply
the installed Qt MSVC directory with `-DCMAKE_PREFIX_PATH` for those presets.
The full profile shares the existing `build/vs2022` directory. A packaged native
bootstrap contains the controller, runtime, app-local VC143 CRT, dependency
manifest and licenses; its manifest explicitly records no live frame capability.
Installation of the bootstrap currently requires an ASCII path; target executable,
arguments and working directory are Unicode.

```powershell
FloraCapture.exe launch --exe C:\Samples\Demo.exe --out C:\Captures\new-session -- "argument with spaces"
FloraCapture.exe status --session C:\Captures\new-session\session.json
FloraCapture.exe capture --session C:\Captures\new-session\session.json --swap-chain 1
FloraCapture.exe cancel --session C:\Captures\new-session\session.json
FloraCapture.exe disconnect --session C:\Captures\new-session\session.json
```

The capture command acknowledges an armed request; poll `status` for its
`capture_progress` and terminal `last_capture`. A zero capture-command exit code
means accepted, not completed. Cancel reports a request while GPU leases are
still draining; only a terminal Cancelled result confirms cancellation. Exit codes are 0 for success, 3 for a rejected
session request and 2 for parameter/transport failures. Session directories must
be new. No command overwrites the target's executable or existing captures.

The checked-reader extraction passes `core`, `format_container`, `format_codecs`,
`format_writer` and `capture_boundaries`. Container tests cover truncation,
invalid index bounds, duplicate identities, bad payload ranges, cancellation
and release of Windows handles after failures. The existing GF2/BF1 golden and
suppressed-draw controls pass with the rebuilt native CLI; local evidence is
`artifacts/capture-format-goldens/validation.json`.

The format writer and schema were reviewed and adapted from the preserved
reference workspace. Container serialization alone does not validate arbitrary
record ownership, variable layouts or full capture semantics. The previous
minimal authored-frame experiment is reference evidence, not a new live-capture
acceptance result in this repository.

A new C++ `FloraFormatAuthorProbe` now authors Clear, Draw and negative-control
files entirely from new records and freshly compiled shaders. All three run in
the current native FloraGPA CLI and installed original GPA kernel, matching the
CPU pixel oracle exactly. The negative file retains a red reference/preview but
executes to black in both players. The 22 checks are retained in
`artifacts/capture-authored-interop/validation.json`. These are authored format
fixtures, not intercepted application frames; original GUI inspection, live
Draw/Dispatch recording and GF2 six-frame acceptance remain pending.

Reproduce the writer gate by explicitly building `FloraFormatAuthorProbe`, then
running `tools/validate_authored_frames.py` with `--author`, `--replay`,
`--oracle-tools`, `--qt-bin` and a new `--out`. Python and the original private
player ABI are confined to this development comparison; the author and writer
are native C++, and the replay product does not load the original player.

Remaining release gates: complete Device/Context/resource tracking; initialization
and hidden-state snapshots; immediate/deferred command recording; frame boundary
and cancellation transactions; dual-player live-frame validation; six GF2 frames
across two starts and two scenes; portable package acceptance. Unsupported paths
must reject explicitly, and no preview image can substitute for actual replay.

Original GPA binaries and Ghidra are development oracles only. The old workspace
and evidence are unchanged. Generated captures, builds and raw analysis stay
outside Git. Commits use the user's configured Git identity.

## Committed-source acceptance

`tools/validate_capture_source_build.py --out <new-directory>` archives HEAD,
verifies the original dependency bytes, configures without Qt/replay, builds,
runs serial CTest, packages through Windows PowerShell and controls a self-owned
target through the packaged controller with system-only PATH. The accepted run
uses source commit `df85054`; all five steps pass and package module observations
contain no GPA, Qt, Python or RenderDoc. Evidence is in
`artifacts/capture-source-build-2/validation.json` and summarized in
`capture-development-baseline.json`.

The first source-build attempt failed because the verifier removed the Windows
ProgramData environment needed by Visual Studio discovery. That failure is
retained separately. The corrected verifier preserves standard Windows setup
variables, records its own hash separately from the source commit and still
excludes Qt/SDK/reference paths. This validates the native bootstrap package on
this host, not live capture or independent-host deployment.

The object-forwarding commit `d8a144d` also passes all five isolated source-build
steps, including seven CTests and packaged native control. This clean build
regenerates every COM proxy without Qt discovery or pre-existing generated files.
The new evidence is `artifacts/capture-object-source-build/validation.json`;
its source commit and evidence hash are recorded in the development baseline.

The native-metadata commit `c8255aa` passes the same five isolated source-build
steps, now including eight serial CTests. The resulting package controls a native
target with system-only PATH and no Qt, GPA, RenderDoc or Python modules. Evidence
is `artifacts/capture-metadata-source-build/validation.json`; this remains a
metadata/bootstrap gate, not live-frame acceptance.

The resource-readback commit `343d7cb` passes all five isolated source-build
steps, including ten serial CTests. Native control through the resulting package
passes with system-only PATH, no Qt discovery and no GPA, Qt, RenderDoc or Python
modules. Evidence is `artifacts/capture-resource-source-build/validation.json`.
This verifies the new source/generator dependency closure and native package;
frame capture and dual-player live-frame acceptance remain pending.

The pipeline-binding commit `3ee4b0c` also passes all five isolated source-build
steps, including twelve serial CTests and native packaged control with system-only
PATH. No Qt is discovered and module observations contain no GPA, Qt, RenderDoc
or Python. Evidence is `artifacts/capture-context-source-build/validation.json`;
this remains a source/package gate, not a generated-frame acceptance result.

The command-model commit `3c6c40b` passes all five isolated source-build steps,
including fourteen serial CTests and the packaged native control lifecycle.
The build discovers no Qt; system-only PATH module observations contain no GPA,
Qt, RenderDoc or Python. Evidence is
`artifacts/capture-stream-source-build/validation.json`. This verifies the new
observer and model sources independently of cached builds; it does not establish
GPA encoding, Present integration or formal live-frame acceptance.

## Observed-model encoder milestone

`FloraCaptureModel` holds CPU values, owned DXBC/descriptors, resource bytes and
commands. GPU layout arithmetic moved into this library without changing its
checked size rules. `freeze_frame` copies persistent metadata and releases the
frame's native reference graph on the caller thread. `FloraCaptureEncoder` links
only this model, `FloraGpaWriter` and Windows GUID generation. It cannot link the
runtime/object registry, reader, replay, Qt or D3D execution libraries. The runtime
is not yet linked to the encoder; only the native probe composes these pieces.

The encoder validates identity/type references, sequence, descriptor/resource
shapes, tight subresource lengths and supported state before opening a writer.
Missing hidden UAV counts, unsupported state and unavailable initial bytes reject
explicitly. Fault gates cover existing destinations, wrong object kinds/shader
stages, missing references, truncated data, sequence errors, unsupported LOD,
pre-cancellation and interruption after temporary-file creation. Interrupted
transactions leave neither a published frame nor a partial file.

`FloraCaptureEncodeProbe` observes actual hardware Clear/Draw/Dispatch calls through
COM wrappers under an explicit recorder scope. All source GPU objects die before
encoding. Five generated files (including removed-Draw and removed-Dispatch
controls) pass 94 serial CPU/native/original-player checks in
`artifacts/capture-encoder-interop-final/validation.json`. Black/red/green 8x8 RGBA
bytes match exactly. Negative files retain the original red/green reference image
but execute to black in both players. Native event inventory and shader DXBC /
disassembly exports also pass. The original GPA kernel hash is
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.

Reproduce with `tools/validate_capture_encoding.py --probe <FloraCaptureEncodeProbe.exe>
--replay <FloraGPA.Cli.exe> --oracle-tools <reference tools> --qt-bin <Qt bin>
--out <new directory>`. This is a development validator; neither encoder nor
capture product uses Python or original GPA. Capture-only passes fifteen CTests;
the full profile passes thirteen capture CTests. No replay/format source changed
in this milestone; existing golden evidence is not presented as a fresh run.

This is a limited encoding milestone, not stage D completion: presentation
metadata is synthesized for a normal offscreen output texture. There is no real
swap-chain boundary, IPC activation, background writer or cancel/publication
linearization yet. State setters are represented by per-command snapshots rather
than individual events. The dual-player matrix covers only Clear RTV, Draw and
Dispatch with ordinary buffers and single-sample RGBA8 textures; other implemented
record layouts still require interoperability cases. Shader auxiliary qwords are
opaque and zero only under this tested profile. Constant-buffer subranges,
nonzero resource LOD, dynamic linkage, predicates/SO, high UAV slots, nonzero view
planes, special texture layouts, MSAA/shared/tiled/protected resources reject.
Map/Update/Copy, deferred execution, original GUI inspection and six GF2 frames
remain pending. Controller capture requests still fail explicitly.

The encoder/model commit `e8c5576` passes all five isolated source-build steps,
including fifteen serial CTests, native package control and the runtime module
audit with system-only PATH. No Qt is discovered; the packaged runtime loads no
GPA, Qt, RenderDoc or Python. Evidence is
`artifacts/capture-encoder-source-build/validation.json`. Encoder linkage is
validated by the independent probe; the shipped runtime remains bootstrap-only
until Present/IPC integration is implemented.

## Real Present transaction milestone

The capture object layer now composes a native `capture_session` transaction with
the independent encoder. Internal control requests select an observed swap ID
and an absolute new output path. An accepted request arms; the next successful
non-TEST Present starts observation, and the following non-TEST Present snapshots
the final backbuffer and freezes the CPU model before forwarding presentation.
Only successful presentation starts the background encoder. Actual swap-chain
descriptors and Present interval/flags replace the probe's synthetic presentation.
Empty Present1 follows the same legacy Present encoding; dirty/scroll metadata
is explicitly unsupported. TEST Present neither starts nor ends recording.

The application thread owns GPU snapshots and temporary COM leases. Before
ResizeBuffers or selected swap destruction, the transaction releases those
leases and rejects the frame, preserving normal application resizing. Cancel
from another thread requests cancellation; active GPU leases drain at a target
callback. The encoder and cancelling control thread share a publication mutex
and an atomic published flag: completion wins after publication, cancellation
wins before it. The writer never replaces an existing destination. CPU-only
finalization can continue while the application presents subsequent frames.

The native boundary probe creates a real DXGI window/swap chain and records three
successive frames, including Present1. Control requests run on separate threads.
It checks busy/unknown selection, TEST boundaries, early and active cancellation,
resize/recovery, unsupported Update rejection, continued rendering after
Disconnect, source-object lifetime closure, and absence of partial files. Twelve
cancel/finalize race attempts check status/file agreement; the retained final run
observed twelve cancellations. Cancellation after completed publication also
returns completed rather than cancelled. Occlusion and device-removed HRESULTs
are injected into the boundary callback, not induced on the physical adapter.
A context call injected between snapshot and Present return rejects the frame;
this guard does not establish general multithread capture ordering. Missing output
directory tests cover writer failure, not physical disk exhaustion.

All three actual boundary files execute to exact red 8x8 RGBA bytes in FloraGPA
and the installed original GPA kernel. Their captured backbuffer bytes, native
event counts and independent CPU oracle match. The 22 checks are retained in
`artifacts/capture-present-interop-final/validation.json`, with request/race/fault
outcomes in `native/results.json`. Reproduce with
`tools/validate_capture_present.py --probe <FloraCapturePresentTests.exe> --replay
<FloraGPA.Cli.exe> --oracle-tools <reference tools> --qt-bin <Qt bin> --out <new-dir>`.
Sixteen capture-only CTests, fourteen full-profile capture CTests, 24 factory and
20 swap-chain injected/native comparisons pass. An initial probe failure from
iterating a temporary JSON object was fixed by retaining the observation value;
the observed error was `Missing observed swap identity`.

This is an internal transaction gate: the private IPC command handler still
rejects capture, and the Qt availability flag remains false. No injected CLI
capture or GF2 frame is accepted yet. Pending work includes controller request
routing/default foreground selection, session result/exit reporting, timeout and
shutdown handling, actual device loss/occlusion, flip/multiple buffers, dirty
Present1, nonzero Present flags, broader state/mutation/deferred support and
original GUI inspection. Current boundary support requires single-buffer DISCARD
or SEQUENTIAL, one sample, RGBA8/BGRA8, and one immediate-context application
thread. The full plan and six-frame GF2 gate remain incomplete.

The Present-transaction commit `2b6c432` passes all five isolated source-build
steps, including sixteen serial CTests and packaged native control. The native
source copy discovers no Qt and the packaged runtime module audit observes no
GPA, Qt, RenderDoc or Python. Evidence is
`artifacts/capture-present-source-build/validation.json`. Its package-control
step still validates the bootstrap protocol, while the separate native probe
validates Present transactions through the internal API; injected CLI capture
remains pending.

## Startup-injected CLI and Qt workflow

Protocol v2 routes `capture`, `status`, `cancel` and `disconnect` into the native
Present transaction. Every request binds the session UUID, target PID and actual
named-pipe client PID; the controller also checks process creation time and pipe
server PID. Current-user pipe ACLs remain enforced. Unsupported/malformed numeric
identities and swap IDs reject without activating capture. Output directories are
passed at launch; only a validated UUID-derived filename is generated by the
runtime. Resource data never crosses the control pipe. Disconnect disables new
captures while retaining status reporting and transparent COM forwarding until
process exit. Protocol v1 sessions must be relaunched.

Eligibility currently requires a recently presenting single-buffer, single-sample
DISCARD/SEQUENTIAL RGBA8/BGRA8 swap with no reported interface gap. Automatic
selection requires exactly one eligible foreground-window match; otherwise the
caller must select a swap ID explicitly. Selection does not promise unsupported
GPU commands can be encoded: such calls reject the frame and report their API.
Status separates Tracking/active phases from completed/cancelled/failed request
results. Qt polls asynchronously, displays terminal outcomes, retains the last
successful file after a later failure, and emits only an absolute existing
`.gpa_frame` path into the existing replay-open flow.

`FloraCaptureLiveTarget` has no capture or replay implementation linked into it.
The controller test starts it twice using the direct D3D11 and DXGI factory paths,
records three consecutive frames per launch, exercises foreground selection and
captures again after an explicitly rejected UpdateSubresource. The retained
interop run produced ten files; all ten pass serial native/original-player
execution with exact CPU and captured backbuffer RGBA, event counts and independent
target module audits (65 checks). Evidence is
`artifacts/capture-ipc-interop/validation.json`. This self-owned 8x8 Draw target is
not the GF2 acceptance corpus. Its original-player harness is development-only.

The native controller gate also tests eight invalid protocol/session/process/swap
identity cases per launch, paused busy/cancel, disconnect without target death,
post-disconnect refusal, post-exit reporting, and no partial-file leaks. A Windows
signal-file deletion race in the test harness initially reported ACCESS_DENIED;
the preserved failure is `artifacts/capture-ipc-final-ctest.txt`. The harness now
retries only transient access/sharing errors with a bound, without restarting the
target. The corrected controller gate passes. Seventeen capture-only CTests,
fifteen full-profile capture CTests, 44 native/injected factory/swap comparisons
and eleven Qt cases pass. The full GUI/Worker build succeeds. Qt validation is
functional widget automation, not visual layout acceptance.

Reproduce with `tools/validate_capture_controller.py --bin <capture binaries>
--replay <FloraGPA.Cli.exe> --oracle-tools <reference tools> --qt-bin <Qt bin>
--out <new directory>`. `FloraCaptureControllerTests` and its CTest gate are native
and require no Python. The package validator now adds a separate packaged live
capture step with system-only PATH; its exact committed-source result is recorded
below after execution.

GF2 diagnostic: the new runtime observed 41,453 then 154,531 successful Presents,
one resize, zero interface gaps and a 2560x1440 RGBA8 swap with two buffers and
SwapEffect 3 (FLIP_SEQUENTIAL). CLI capture explicitly returned
`capture_capability_incomplete`; no frame was published. After disconnect the
newly launched process closed normally through its window. Unity exceptions
remain in the diagnostic log; no gameplay scene/pixel success is claimed.
Evidence and binary hashes are under `artifacts/capture-ipc-gf2`. The next capture
work must address real flip/backbuffer semantics before GF2 can reach command
coverage validation. Map/Update/Copy, deferred ordering, remaining hidden states,
actual device-loss/disk-full/shutdown races, original GUI inspection and GF2's
six dual-player frames remain release gates.

The CLI/Qt commit `98dd267` passes all six isolated source-build steps, including
seventeen serial CTests, bootstrap package control and the new packaged live
capture gate. With system-only PATH, the packaged controller/runtime generated
10 self-owned frame files across both creation paths and passed native outcome
checks. The clean build discovers no Qt; both module audits observe no GPA, Qt,
RenderDoc or Python. Evidence is
`artifacts/capture-ipc-source-build/validation.json`. These packaged files are
covered by native control checks; the separate interop run above provides the
original-player/CPU execution evidence. Independent-host and GF2 acceptance remain
pending.

## Flip presentation and tearing

The current boundary profile supports DISCARD/SEQUENTIAL with 1–16 buffers and
FLIP_SEQUENTIAL/FLIP_DISCARD with 2–16 buffers, one sample, RGBA8/BGRA8, empty
Present1 parameters and one immediate-context application thread. Present flags
may be zero or ALLOW_TEARING; other flags reject, while TEST never advances the
boundary. The encoder validates tearing's creation flag, windowed mode, zero
sync interval and flip effect before publishing. The actual descriptor and flags
are preserved in the existing legacy wire records; replay and format sources
are unchanged in this iteration.

DX11 retains logical backbuffer zero across flip presentation and unbinds its
writable pipeline binding on a successful flip. This follows Microsoft's
[DXGI guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi).
The capture transaction checks canonical backbuffer identity at frame end;
it does not infer a DX12-style physical buffer index. The native oracle checks
identity and the implicit RTV unbind after every Present, explicitly rebinding
before the next Draw. Tearing restrictions follow the documented
[Present flags contract](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).
Status now exposes last Present flags/interval and the completed frame's
successful non-TEST Present ordinal.

Eleven independent target launches cover direct creation, legacy factories and
CreateSwapChainForHwnd, including flip counts 2, 3 and 16, blit SEQUENTIAL count 3,
and both flip effects with tearing. Multi-buffer targets cycle five distinct
exact RGBA colors; the independent CPU oracle derives the expected color from
the captured Present ordinal. The retained run produced 55 files and passed
353 serial checks in FloraGPA and the original GPA playback kernel, including
captured-backbuffer equality and target module audits. Evidence is
`artifacts/capture-flip-tearing-interop/{validation,inputs}.json`. This is still
a small self-owned Draw oracle, not original GUI or GF2 scene acceptance.
An earlier run's expected event inventory omitted the existing replay diagnostic
`present_rtv_unbinds`; the preserved failure is `artifacts/capture-flip-interop`.
The corrected expectation never relaxes pixel comparisons.

Read-only inspection of the preserved original GF2 frame confirms an 88-byte
swap descriptor with BufferCount 2 / SwapEffect 3 and a 28-byte Present record
with SyncInterval 0 / flags 0x200. Evidence is
`artifacts/capture-flip-original-{descriptors,present}.json`.
A new GF2 diagnostic observes 49,422 successful Presents with these actual
runtime values and no interface gaps. Capture advances past presentation setup
and explicitly fails at `ID3D11DeviceContext::Map: GPU call recording is not
implemented`. No frame is published. Disconnect and normal window close succeed;
Unity exceptions remain, so no gameplay/scene success is claimed. Hashes, status,
request, exit and logs are under `artifacts/capture-flip-tearing-gf2`.
Accepted GF2 frames remain zero. Map/Unmap and other mutations, deferred ordering,
remaining hidden state, dirty Present1, physical disk-full/device-loss/shutdown
races and original GUI/six-frame GF2 acceptance remain incomplete.

The updated native gates pass 19 capture-only CTests and 17 full-profile capture
CTests, including eleven Qt cases. Additional Present transaction cases run the
cancel/publication, resize, TEST, unsupported-call and failure checks on
FLIP_SEQUENTIAL and FLIP_DISCARD. Encoder negatives reject invalid buffer counts,
unknown effects and invalid tearing configurations without publishing a file.
The previous committed-source package result above is historical; this exact
iteration's isolated archive/package result is recorded separately after commit.
The native/injected factory and swap-chain comparisons also pass all 44 checks.

Commit `7d36fd4` passes all six isolated committed-source steps:
configure, build, 19 serial CTests, package, packaged bootstrap control and
packaged live capture. The system-PATH package run completes eleven independent
target launches and 55 native-validated files. Configuration discovers no Qt,
and both runtime audits observe no GPA, Qt, RenderDoc or Python. Evidence is
`artifacts/capture-flip-source-build/validation.json`. The 353-check interop run
above supplies the separate original-player and exact CPU execution evidence;
this package result does not establish broader application or GF2 acceptance.

## Map/Unmap recording

The immediate-context recorder now owns successful READ, WRITE, READ_WRITE,
WRITE_DISCARD and buffer WRITE_NO_OVERWRITE observations. Map snapshots preserve
first-use initial GPU contents before native mapping; post-Map observation keeps
the actual HRESULT and valid returned layout. Immediately before native Unmap,
the runtime copies checked logical rows/slices into owned CPU bytes, excluding
pitch padding. These transient pointers never enter FrozenFrame or the writer.
Failed Map outputs are not dereferenced. Frame boundaries reject preexisting or
unfinished mappings; cancellation and unsupported thread handoff stop pointer
access while native calls still forward. Initial resources and mapped payloads
share the frame byte budget.

The encoder emits legacy writable Map records (0x246), READ observations
(0x34ec), Unmap observations (0x34ed) and owned GenData. It validates completed
calls, context/resource identities, subresource bounds, flags, byte lengths and
successful Map/Unmap pairs before opening a writer transaction. Runtime and
encoder use capture-owned data only. No replay, core, shared-format or private
GPA implementation was added to the capture dependency graph.

The Map pointer lifetime follows Microsoft's
[Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
and [Unmap](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-unmap)
contracts. Snapshot readback and copying write-combined mappings can stall the
application; this milestone establishes correctness for tested profiles, not
capture throughput. Planar mappings, null-storage default-resource mapping,
deferred contexts, GPU execution while another resource is mapped, and broader
multithread ordering still fail explicitly. More resource layouts need their own
wire interoperability evidence rather than extrapolation from RGBA8.

Native WARP/hardware tests cover all five Map types, owned successive values,
initial contents, pitched RGBA8 rows, cancellation, preexisting/unfinished maps,
foreign-thread Unmap and budget exhaustion. An actual invalid READ mapping of
a write-only resource preserves the returned failure. WAS_STILL_DRAWING is a
separate injected observer result, not a hardware scheduling claim. Eight encoder
negative cases reject malformed data, completion, subresource, type, flags,
result and pairing without publishing or leaking partial files.

Six scoped files pass 84 serial native/original-player checks: dynamic constants
produce separate red/green halves, a dynamic RGBA8 texture produces alternating
rows, NO_OVERWRITE targets unused storage before its first Draw, and staging
READ/READ_WRITE/WRITE calls execute alongside Draw. Removing Map/Unmap leaves
initial blue data while retaining the original final reference image; both
players execute the changed output. All original native objects die before
encoding. Event inventories and shader DXBC/disassembly are inspected. Evidence:
`artifacts/capture-map-interop-verified/{validation,inputs}.json`. Initial
validator failures used an incorrect Unmap diagnostic key and omitted the
existing READ synchronization counter; preserved directories are
`capture-map-interop-1` and `capture-map-interop-final`. Pixel rules were unchanged.

The real injected CLI fixture adds dynamic constant-buffer Map/Unmap on direct
and factory-created tearing flip chains. Thirteen launch modes generate 65 files
and pass 417 serial dual-player/CPU/module checks in the initial integration run
(`artifacts/capture-map-live-interop`). Final guarded-code regression evidence is
recorded below. This remains a self-owned oracle, not original GUI acceptance.

GF2 diagnostic evidence is `artifacts/capture-map-gf2`: 45,111 successful Presents
before the request, zero interface gaps, then an explicit
`ID3D11DeviceContext::End: GPU call recording is not implemented` failure.
Map is no longer the first gap. No frame was published; disconnect and normal
window close succeeded. Binary hashes identify this run before the subsequent
foreign-thread pointer guard hardening. Unity exceptions remain and no gameplay
scene success is claimed. Accepted GF2 frames remain zero. Query/End recording,
Update/Copy, deferred ordering, remaining hidden state and the six-frame/GUI
release gates remain incomplete.

Final pointer-guard code passes 22 capture-only CTests, 20 full-profile capture
CTests and eleven Qt cases. The final scoped and injected interop reruns retain
84 and 417 passing checks, respectively, with 65 injected files. Evidence is
`artifacts/capture-map-release-{ctest,full-ctest,qt}.txt`,
`artifacts/capture-map-release-interop/{validation,inputs}.json` and
`artifacts/capture-map-release-live-interop/{validation,inputs}.json`.
The previous source-build result is historical; this iteration's archive/package
result is recorded after the implementation commit below.
All 44 native/injected factory and swap-chain forwarding comparisons also pass.

Commit `63aff5a` passes all six isolated source-build/package steps, including 22
serial CTests, packaged bootstrap control and 65 packaged live files over thirteen
launches. System-only PATH discovers no Qt and loads no GPA, Qt, RenderDoc or
Python in the runtime audits. Evidence is `artifacts/capture-map-source-build`.
Its NO_OVERWRITE pixel fixture maps unused storage before its first Draw only;
broader streaming use requires sparse patches so unchanged bytes still in GPU
use are not rewritten during playback. That follow-up remains necessary.

## Sparse NO_OVERWRITE updates

NO_OVERWRITE now takes a fresh buffer baseline before each native Map and emits
only byte runs that actually changed, using legacy GenData differential records
(9/0x100). Unchanged bytes potentially still referenced by earlier GPU work are
not rewritten by playback. The baseline and final mapping bytes both count
against the frame budget; failed mappings discard their unused baseline. The
extra readback can stall capture and does not establish a performance claim.

The new streaming fixture reads the first float4 in a Draw, updates the second
float4 through NO_OVERWRITE, then draws from that second element. A third Map
makes no change. An independent wire parser verifies all three differential
records, restricts each patch to its newly written region, applies patches to
the known initial bytes, and confirms the empty patch. The original GPA kernel
and FloraGPA execute all six scoped files correctly, including the preserved
negative controls: 97 checks pass in `artifacts/capture-map-sparse-interop`.
A ninth encoder negative rejects a missing NO_OVERWRITE baseline. The C++ Map
suite also passes on WARP and hardware. This closes the full-buffer rewrite gap
identified above; other mapped layouts and application/query coverage remain
partial. No replay or shared-format source changed.

### Concurrent wrapper release regression

The first full-profile sparse-map regression hit a metadata-test access violation.
Repeated native execution reproduced it, and AddressSanitizer identified
`Object::Release` reading `impl_` after a second thread deleted that wrapper.
The forwarding reference still keeps the native COM object alive; the wrapper
itself is no longer safe to access after decrementing under the registry lock.
Release now saves the native identity inside that lock and uses only the saved
pointer afterward. Factory/swap wrappers already used this pattern.

The fixed original concurrency fixture passes 500 serial ASan runs. The fixture
now synchronizes eight rewrapping threads at each release boundary, forcing
wrapper generations to lose their references together; 100 further ASan runs
pass. Both successes and original failures are retained under
`artifacts/capture-map-metadata-*`, summarized with hashes in
`artifacts/capture-map-release-race.json`. ASan is development-only and is not
linked into the packaged runtime. This failure was fixed, not waived as flaky.

The sanitizer reproduction uses a separate capture-only CMake build with MSVC
Release flags `/O1 /DNDEBUG /Zi /fsanitize=address` and executable linker flags
`/DEBUG /INCREMENTAL:NO`, targets `FloraCaptureMetadataTests`, and runs
`ctest -C Release -R '^capture_metadata$' --repeat until-fail:100 -j1` with the
matching VS x64 ASan runtime directory on PATH. Normal packages retain ordinary
Release flags. The strengthened test is part of normal CTest too.

After the concurrency fix, the combined sparse-map code passes all 22 capture-only
and 20 full-profile capture CTests, eleven Qt cases, 97 scoped dual-player checks
and 44 native/injected factory/swap comparisons. Final logs and hashes are under
`artifacts/capture-map-fixed-*`. No sanitizer dependency enters these binaries.

The final sparse-map/concurrency commit `e804b95` passes all six fresh
committed-source configure/build/test/package steps, including 22 serial CTests
and 65 packaged live files across thirteen launch modes. Configuration finds no
Qt; system-only PATH runtime audits load no GPA, Qt, RenderDoc or Python.
Evidence is `artifacts/capture-map-sparse-source-build/validation.json`.
The separate scoped interop run establishes sparse-map execution; the packaged
live fixture's mapped writes currently use WRITE_DISCARD.

Next query investigation: read-only inspection of the preserved original GF2
frame finds two 0x34fb GetData observations and no saved 0x96 query/predicate
resource or recognized Begin/End record. The first GetData returns S_OK with
null output; the second returns S_FALSE. Evidence is
`artifacts/capture-query-next-wire.json`. This observation does not justify
silently dropping the live End call or guessing its completion dependency;
query wire/initial-state behavior still needs independent experiments.

## Query completion capture boundary

The generator now treats `GetData` as an unsupported query operation rather
than a passive getter. An End issued before the captured frame can make a
frame-local GetData establish a CPU/GPU completion dependency; omitting that
call would incorrectly report a complete capture. Begin, End and GetData now
reject with the API, persistent object ID, query descriptor and, for GetData,
requested size/flags. Native forwarding and returned data are preserved. No
caller output pointer is read by this diagnostic, and no query is converted
to a predicate or assigned a guessed result.

This boundary follows the existing original-capture experiments in
[Query completion](QUERY_COMPLETION.md), not just inspection of the GF2 file.
The preserved reference `analysis/query_resources/README.md`, `id_manager.c`
and `get_data.c` identify omitted ordinary-query resources/End records and
four-byte-only GetData observations in the pinned shim. Its original-player
resource factory uses CreatePredicate for family 0x96; inventing an EVENT
descriptor in that family is not a verified compatibility path. Existing
EVENT completion fixtures have demonstrated wrong original-player output.
The input hashes and RVAs consulted here are retained in
`artifacts/capture-query-research.json`; this batch reuses those reverse
engineering results and does not claim a new Ghidra analysis.

WARP and hardware tests cover rejection of all sixteen D3D11 query types,
including End-only EVENT/TIMESTAMP. Two additional cases per device complete
an EVENT before recording and call GetData during recording with either BOOL
output or status-only output. Both must reject while preserving native S_OK
and the BOOL result. A subsequent ordinary Draw capture succeeds. These are
scope/failure tests, not query serialization or dual-player acceptance.

A fresh GF2 diagnostic reaches 182,857 successful Presents before requesting
capture. Its first rejected operation is GetData on tracked object 137,
`query1`, EVENT(0), MiscFlags=0, ContextType=ALL, null output, size=0, flags=0.
The error does not claim a captured HRESULT: rejection occurs before native
forwarding. No frame is published. Disconnect and normal window close both
succeed. Exact binaries, request/status and process-exit evidence are retained
under `artifacts/capture-query-gf2`; accepted GF2 captures remain **zero**.

Query encoding and completion preservation remain open. Compatibility must
be demonstrated with a real ordering-dependent oracle in both players before
removing this guard. Replay, shared format code and the old migration parity
ledger are unchanged by this correction.

Regression: all 22 capture-only CTests and all 20 full-profile capture CTests
pass serially, including eleven Qt cases with no failures or skips. Logs and
input hashes are registered in `capture-development-baseline.json`. Prior
dual-player and committed-source package results retain their original commit
scope; this guard-only batch does not claim a new dual-player/package run.

## Whole-resource copies

Generated proxies now record `CopyResource` with two persistent object IDs.
The recorder retains both operands, including unbound resources, snapshots
their initial GPU contents before the first recorded use, and preserves call
order alongside Draw, Clear and Map/Unmap. The frozen model contains no native
pointer. The encoder emits the verified legacy `7/0x3e` envelope containing
link=0, context, destination and source. Capture still does not depend on
Replay/Core or load the original GPA runtime.

This increment supports matching-format, matching-shape, single-sample whole
copies. Shape mismatch, missing/aliased operands, another device, an immutable
destination and unsupported predication reject. Existing mapping, thread,
deferred-context and hidden-state guards continue to apply. Format-family
reinterpretation, MSAA, CopySubresourceRegion, CopyStructureCount and Resolve
remain pending; a whole-copy implementation does not establish those paths.
The [Microsoft CopyResource contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
permits some of those broader cases, so the explicit format/MSAA rejections
describe the current capture scope, not an API prohibition.

The WARP/hardware fixtures copy buffers and RGBA8 textures after their source
has changed from its creation data, then change it again between two copies
and Draws. Exact green/red output and the preserved blue destination initial
bytes prove ordering and initial-data capture. Separate unbound-resource
tests compare all subresource bytes of 1D/2D arrays and 3D textures with three
mips. These latter storage tests are native tests, not dual-player texture
dimension acceptance.

`tools/validate_capture_copies.py` produces four scoped COM-observed files:
buffer and texture cases plus controls that remove only CopyResource while
retaining the captured final reference. FloraGPA and the original private-ABI
player execute both positives to exact green/red pixels and both controls to
exact blue. The 68 checks include bounded independent copy-record/reference
parsing, executed event counts, shader bytecode/disassembly, resource inventory
and destruction of all source GPU objects before encoding. Evidence is
`artifacts/capture-copy-final-interop`; original GUI and startup-injected Copy
workload acceptance are not claimed by this fixture.

The regression covers 25 capture-only and 23 full-profile capture suites,
including eleven Qt cases. Eight encoder negatives verify located rejection
and absence of published/temporary files. The initial negative tests selected
an earlier missing-context/initial-resource guard; the corrected inputs now
exercise their intended copy-context and reference checks. Initial failures
and passing targeted reruns are retained in the baseline. No Replay/shared
format source changed, so existing golden frames were not rerun for this
increment. Query completion remains explicitly unsupported; GF2 acceptance
is still zero and this batch does not repeat the game diagnostic.

Commit `61ca31c` subsequently passes all six fresh committed-source build and
package steps, including a single successful run of all 25 serial CTests.
The packaged live regression produces 65 native-validated frames in thirteen
launches under a system-only PATH; configuration discovers no Qt and module
audits load no GPA, Qt, RenderDoc or Python. This verifies package independence
and the existing live profile; its workload does not yet exercise CopyResource.
Evidence is `artifacts/capture-copy-source-build/validation.json`.

## EVENT query compatibility research

Fresh read-only Ghidra inspection of the installed `dx11_player.dll` enumerates
48 D3D11-manager Erg class tables. No standalone EVENT/GetData wait executor
appears in that set. At RVA `0x574a0`, the predicate resource factory calls
CreatePredicate, followed by an empty Begin/End. End execution at `0x27b20`
requires D3D11PredicateRsc; Flush at `0x283a0` only forwards native Flush.
The project MD5 matches the installed DLL; its SHA-256 remains
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
Class tables, decompilation, program identity and hashes are retained in
`artifacts/capture-query-player-research/summary.json`. These conclusions cover
the inspected installed paths, not every possible GPA version/mode.

`tools/validate_capture_query_renaming.py` tests a different execution strategy
using the two hash-pinned original EVENT completion fixtures. Both save a full
65,536-byte dynamic vertex buffer. The experiment changes exactly one byte:
the second Map's type from NO_OVERWRITE to DISCARD. It preserves every payload,
resource, query observation, Draw and preview. Microsoft's
[dynamic buffer documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/how-to--use-dynamic-resources)
explains that DISCARD can supply new storage while pending GPU work continues
using the previous storage. It discards the entire buffer, so applying this
transformation to a sparse patch without recovering the full contents is unsafe.
This is a candidate resource-versioning strategy, not an encoded query wait.

The final experiment performs 44 serial runs:

- Twelve untouched-original observations reproduce eleven incorrect outputs
  (six original-player and five FloraGPA); one FloraGPA run happens to match.
  The original missing-boundary behavior is timing-sensitive, not accepted.
- Twelve renamed executions across both players match the independent
  application image exactly. Twelve Draw-zero controls produce the expected
  clear color despite retaining the original captured reference image.
- Two original-player runs accept an authored `0x34fa` End API record referring
  to the omitted query and still produce correct renamed output. FloraGPA
  rejects both with the located missing-predicate diagnostic. This experimental
  record replaces incidental nested GetDesc metadata at event 10; it is not
  claimed to have been saved by the original capture.
- Changing that record to the executable `0x243` End Erg rejects in both
  tools: two original Open/status-13 failures and two FloraGPA missing-resource
  failures. Accepting an API observation does not establish an executable End.

The final report is `artifacts/capture-query-renaming-verified/validation.json`.
The original player still uses the development private-ABI runner; its selected
adapter and GUI are not verified. Earlier experiments and their failures remain
available. In particular, the first End-record builder appended payload beyond
the index; the checked reader correctly rejected it. The corrected builder puts
payload before the index and adjusts index/preview offsets, retaining the
failure log rather than relaxing reader validation.

Production query rejection remains enabled. This research does **not** restore
the original query object/End/completion state and is not GF2 acceptance. Before
considering resource versioning for live capture, the capture-owned model must
record real EVENT lifetimes and returned BOOL/status observations, preserve
original API provenance, and establish correctness for repeated/pre-frame
queries and vertex/index/constant/SRV buffer use. The encoder must reject any
case whose semantics cannot be expressed and verified. No runtime, replay or
format code changed in this research batch; native CTest/goldens/package results
retain their previous commit scope.

## Capture-owned EVENT observations

The generated End and GetData proxies now retain EVENT observations after the
native call. Each query has a persistent End generation and the issuing context,
thread and immediate/deferred provenance. GetData stores its real HRESULT,
DataSize, flags and output-pointer presence. Only S_OK with a valid EVENT output
size copies an owned BOOL. Pending and failed calls never read the output pointer.
A matching End in the same frame has an explicit command sequence; a pre-frame,
unobserved or deferred End does not acquire an invented in-frame execution order.
These are CPU observations, not a cross-context GPU completion proof.

The observer performs identity/get-device operations only: it does not snapshot
resources, poll queries, flush or enqueue GPU work before/after GetData. Query
objects are retained until freezing, and the frozen model contains only owned
values and stable IDs. Capture and its encoder remain independent of Replay/Core.
CreateQuery and CreateQuery1 EVENT/ALL with zero miscellaneous flags are covered;
other query types and Begin semantics still reject with located diagnostics.

The encoder rejects every frame containing an EVENT observation before creating
a temporary or final file. Recording a true completion result does not yet give
the legacy GPA player an executable wait, so query serialization is still
unsupported. The resource-renaming research above is not enabled in production.

`FloraCaptureQueryTests` covers WARP and hardware, both query creation interfaces,
repeated End, pre-frame results, deferred provenance, actual failed GetData,
owned output after caller mutation, byte/command budgets, foreign-thread and
reentrant calls, cancellation, native-object destruction and encoder refusal.
Injected pending/failed callbacks use an inaccessible output address, never
passed to D3D, to detect accidental reads deterministically. Native readiness is
recorded as observed rather than asserting a timing-sensitive S_FALSE.

Regression passes 28 capture-only and 26 full-profile capture CTests, including
eleven Qt cases without skips. Query1 coverage added after the broad capture-only
run passes three targeted suites; the subsequent located encoder error passes
both profile checks. An initial invalid build-preset invocation and its corrected
build are retained. Shared format and Replay code did not change, so no new
golden or dual-player result is claimed. The previous committed-source package
evidence retains its original commit scope.

One fresh GF2 startup diagnostic reaches encoder rejection at selected-chain
Present 238. The game continues presenting (3,517 successful Presents in the
saved status), publishes no frame, disconnects and exits through normal window
close. This is an early startup frame, not scene/dual-player acceptance. Its DLL
hash predates the subsequent diagnostic-only addition of API/object/sequence to
the rejection message. Logs, input hashes and scope are recorded in
`artifacts/capture-event-model-validation.json`; accepted GF2 frames remain zero.

Commit `ca7e8d4` subsequently passes all six independent committed-source
configure/build/test/package steps and all 28 serial CTests, including the final
Query1 and located-error changes. The packaged regression produces 65
native-validated existing-profile files across thirteen launches under a
system-only PATH, without Qt discovery or GPA/Qt/RenderDoc/Python modules.
Its live workload does not encode EVENT queries. Evidence is
`artifacts/capture-event-model-source-build/validation.json`.

## Executable resource-read dependency research

Read-only Ghidra inspection of the same hash-pinned player locates the Map Erg
executor at RVA `0x27080` and its data-reference initialization at `0x1c840`.
Execution requires a nonzero resolved data reference. A `7/0x246` READ Map with
a `9/0x100` GenDataDiff containing zero ranges and zero bytes calls native Map
and Unmap without writing mapped memory. A zero data reference skips this path.
Decompilation and the read-only invocation are retained under
`artifacts/capture-query-sync-research`.

`tools/validate_capture_query_map_wait.py` replaces only the pinned incidental
nested GetDesc at event 10 with such an executable READ of the preceding copy's
staging destination. It adds an empty diff, preserving every other captured
command, resource descriptor, payload and preview. In particular it preserves
NO_OVERWRITE and all GetData observations. The Draw-zero control additionally
changes only Draw's vertex count. These are explicitly authored experimental
records, not new claims about what Intel's capture shim saves.

The initial 24-run experiment yields six exact original-player outputs and six
exact Draw-zero outputs. Of six zero-data-reference controls, five reproduce
the wrong output and one happens to match; this remains timing-dependent.
The old FloraGPA reader explicitly rejects all six authored READ Erg variants.
Evidence is `artifacts/capture-query-map-wait/validation.json`.

The reader now recognizes only the verified executable READ profile: an empty
diff, zero flags and a CPU-readable staging buffer. It treats the record as
an atomic Map/Unmap, allowing later captured API Map intervals without inventing
an unmatched mapping. Nonempty diffs, full payloads, missing data, other resource
profiles and overlapping API mappings reject. Replay executes native READ and
reports `map_read_erg_synchronizations`; it does not inspect Capture objects or
its control protocol. The production Capture encoder still rejects EVENT frames.

This is a resource dependency, not a general EVENT implementation. Microsoft's
[Map contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
describes access to a particular subresource, whereas an
[EVENT query](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
reports completion of GPU commands. The experiment fixes the known copy/read
dependency without proving completion of unrelated work, restoring the omitted
query object/End or validating pre-frame/deferred intervals. A general lowering
requires an explicit dependency proof and preserved provenance before production
query capture can be enabled. Original GUI and GF2 acceptance remain pending.

With the checked READ profile implemented, the follow-up experiment runs 32
serial cases: twelve exact dual-player positive outputs, twelve exact Draw-zero
outputs, six original-player zero-data mismatches and two checked-reader
zero-data rejections. FloraGPA still reports the original unresolved query
completion notices; the successful resource wait does not suppress them.
Eight relevant CTest suites pass, including 72 Map record, 28 Map readiness and
33 query completion cases without skips, using the existing original corpora.
All four existing GF2/BF1 golden/Draw-suppression controls pass with unchanged
event counts. Evidence and binary hashes are registered in
`artifacts/capture-query-read-erg-validation.json`. Capture runtime, encoder
and package results retain their previous commit scope; no new GF2 capture
attempt or full original-GUI acceptance is claimed.

## Capture-owned EVENT dependency inspection

`FloraCaptureModel` now analyzes the frozen Capture model without Replay/Core,
the format reader, COM objects or GPU calls. Each End receives an immutable
snapshot of the preceding recorded operations. Successful GetData observations
must match the same query ID, generation, context, thread provenance and earlier
End sequence. Repeated successful polling shares that snapshot; a subsequent
End receives its own. Later Draws cannot retroactively enter an earlier interval.

The report distinguishes pending, failed, unissued, pre-frame, deferred,
foreign-context and recorded-prefix observations. Only unpredicated whole buffer
copies into a read-only staging buffer become candidate READ dependencies,
retaining source/destination IDs and command order. Reference, descriptor, shape,
context and query-profile checks bound that scope. Other copies, Draw/Dispatch,
Clears and CPU mapping hazards remain explicitly uncovered. This is dependency
inspection, not complete frame validation or permission to encode a query.

An entry budget bounds accumulated prefix snapshots and observations; cancellation
is checked throughout. The encoder runs this inspection before its existing
EVENT refusal and reports interval counts, the latest matching prefix, candidate
copy waits and the first uncovered operation. It still publishes no query frame,
even for a prefix containing only supported copies: unrecorded work before the
frame and general EVENT completion are not proven by this report.

The CPU-only dependency test covers both query descriptors, interleaved identities,
reused intervals, all observation scopes, invalid/stale associations, uncovered
command kinds, unsafe copy destinations, cancellation and bounded allocation.
The native query fixture now copies known buffer words, completes an EVENT and
checks the application's bytes. After all original GPU objects die, its frozen
model retains the copy edge and excludes a later Clear from the End prefix;
the encoder continues to reject without temporary or final files.

Regression passes 29 capture-only and 27 full-profile capture CTests, including
eleven Qt cases without skips. Additional interleaved-query and sequence-zero
cases pass targeted tests in both profiles. The standalone dependency test has
only Windows/CRT imports. The first /WX build found a signed comparison against
an optional unsigned BOOL; the corrected unsigned comparison and original
failure log are retained. Replay/shared format code is unchanged in this batch,
so the prior golden and dual-player evidence retains its commit scope.

Two consecutive GF2 diagnostic requests fail safely at Present ordinals 43,700
and 73,165. Each records one End and three GetData calls. The first has two
pending observations and one successful pre-frame observation; the second has
one pending and two successful pre-frame observations. Neither has a successful
GetData associated with the frame's later End. Saved status shows continuing
rendering through 48,304 and 77,254 Presents. No file is published; disconnect
and normal window exit succeed. Evidence is
`artifacts/capture-query-dependencies-validation.json`.

The next required evidence is the completion/initialization boundary at frame
start for these pre-frame observations. Reusing a later in-frame End or treating
a copy-only recorded prefix as proof about preceding frames would be incorrect.
Accepted GF2 captures remain zero.

## Frame-start completion evidence

Recording now registers its observer before a private, unwrapped immediate-context
EVENT is submitted. It checks cancellation, elapsed time and device removal while
waiting for actual `S_OK` and `TRUE`. No private query object or internal command
enters the application object registry or event stream. Failed initialization
unregisters the recorder; a later request can start normally. IPC cancellation
reaches this startup wait before the request owns a finished recorder. Predication
at this boundary is explicitly unsupported and its binding is preserved.

Each application End receives a context-local serial in addition to its query
generation. The successful private completion retains the last such serial,
context/device identities, owner thread, elapsed time and native result in the
Capture-owned CPU model. Frozen frames retain this evidence after native objects
die. Dependency inspection accepts `preframe_completed` only for a successful
observation with matching immediate-context provenance and a nonzero covered
serial. Missing evidence stays unproven; a pending result for a covered End is a
contradiction and rejects. In-frame End serials must advance beyond the boundary.
This evidence neither creates a GPA query object nor authorizes dropping events;
the encoder's EVENT publication guard remains.

The initial hardware regression exposed an internal polling problem. A native-only
fixture, without Flora injection or wrappers, reproduced a stall after early
release of a submitted EVENT when subsequent polling used `DONOTFLUSH`. The
12-second external process deadline expired after iteration four. Allowing native
GetData to flush completed twenty iterations, including early release, followed
by another immediate EVENT with a live empty deferred context. The private wait
therefore uses flags zero; application GetData arguments are unchanged. Polling
has bounded checkpoints, but cannot preempt a native driver API call itself.
Microsoft documents both the [GetData result contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)
and the [DONOTFLUSH progress limitation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag).

All 29 capture-only and 27 full-profile capture CTests pass, including eleven Qt
cases with no skips. WARP/hardware tests verify copied bytes, viewport and error
state preservation, private-query isolation, busy, timeout, cancellation before
and after submission, foreign/reentrant calls, deferred-context refusal,
predication preservation and recovery. CPU tests reject forged identity/result
and serial associations. The initial missing-header build failure and hardware
timeout logs remain in ignored evidence.

Two GF2 diagnostic requests at Present ordinals 60,389 and 66,184 report one and
two `pre-frame-completed` observations respectively, each with one pending poll
and one in-frame End. They still refuse EVENT serialization, publish no files,
and continue rendering through 61,096 and 66,887 Presents. Disconnect and normal
window exit succeed. Evidence and hashes are in
`artifacts/capture-frame-start-validation.json`. This is not original GPA GUI or
six-frame acceptance. Replay/shared format is unchanged; previous golden,
dual-player and committed-source package evidence retains its earlier scope.
The next requirement is verified GPA EVENT initialization/completion semantics.

The subsequent isolated source build of `deca0ab` passes all six configure,
build, test, package and control steps, including 29 serial CTests. Its archive
build discovers no Qt; the package runs with system-only PATH and produces 65
native-validated existing-profile files in thirteen launches. Captured module
inventories contain no GPA, Qt, RenderDoc or Python. The source-build manifest is
`artifacts/capture-frame-start-source-build/validation.json`. These additional
checks cover the committed frame-start implementation; they do not add EVENT
encoding, dual-player query evidence or accepted GF2 frames.

## Whole-subresource CPU updates

Capture now records `UpdateSubresource` and `UpdateSubresource1` with zero copy
flags for a complete subresource (null destination box). The supported destination
is DEFAULT, non-depth-stencil, single-sample storage. Buffers and 1D/2D/3D textures
use the Capture-owned layout model; no Replay/Core helpers are linked or included.
The observer copies logical rows/depth slices before forwarding the application
call and charges them to the recording budget. The model retains original pitches
and API version, but owns packed bytes rather than caller pointers or row padding.
This follows the [native source lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource).

The encoder validates destination identity, context, usage, subresource, pitches
and payload length, then emits `7/0x247` or `7/0x255` referencing packed `9/1` data.
Initial GPU contents remain separate from creation data, update payloads and the
final image. Boxes, nonzero copy flags, planar formats, predication, MSAA and
non-DEFAULT destinations remain explicitly unsupported. Existing rejection and
recovery fixtures now use a valid partial boxed Update, preserving native
forwarding while exercising an actual remaining capability gap. EVENT dependency
inspection marks Update work uncovered; query publication remains guarded.

WARP and hardware tests cover buffer and texture updates, mip/array/depth storage,
padded rows and slices, immediate caller-memory reuse, byte-budget rejection,
native partial-update forwarding and subsequent recovery. The initial resource
is GPU-updated from creation blue to yellow before recording; two recorded updates
then produce green and red halves through Draw. All source GPU objects die before
encoding. Twelve malformed encoder cases reject without final or temporary files.
The first invalid-context negative test changed the frame's first context and hit
an earlier identity check; it was corrected to change a later Update. Its original
failure log is retained.

The direct dual-player matrix has sixteen files and 272 checks: both APIs across
Buffer/Texture1D/Texture2D/Texture3D, with eight positive files and eight variants
that remove the updates but retain the original reference image. Positive outputs
match the independent green/red image exactly; the negative executions remain
yellow. Actual event counts and exported DXBC are checked. Three-dimensional
fixtures sample the second slice, exposing incorrect depth-pitch handling.

The independent startup target also exercises both Update APIs through the CLI,
injected DLL and private IPC. Fifteen launches produce 75 files and pass 481
serial dual-player/CPU/module checks; ten of those files use the two new Update
modes. Busy, cancellation, boxed-update refusal, retry, disconnect and target exit
remain covered. The actual loaded target modules contain no GPA, Qt, RenderDoc
or Python. Final regression passes 32 capture-only and 30 full-profile capture
CTests, including eleven Qt cases without skips.

Evidence and hashes are in `artifacts/capture-update-validation.json`,
`artifacts/capture-update-all-dimensions-interop` and
`artifacts/capture-update-injected-interop`. All 91 saved original-player reports
identify the pinned GPA 2025 R1 player hash. That oracle remains the development
private-ABI runner, not GUI/adapter acceptance. No Replay/shared-format code
changed; golden evidence retains its previous scope. This batch does not rerun
GF2 or establish query compatibility; accepted GF2 captures remain zero.

The committed whole-subresource revision `c7512da` also passes a fresh source
archive build in `artifacts/capture-update-source-build`: six configure/build/
test/package/control steps, 32 CTests and 75 packaged captures in fifteen launches.
The package runs with system-only PATH, does not discover Qt, and its loaded
module inventories contain no GPA, Qt, RenderDoc or Python. This evidence is
specific to that revision and does not cover the boxed extension below.

## Boxed CPU updates

The Capture-owned model now stores destination box coordinates by value and
copies only the logical source region before forwarding `UpdateSubresource` or
zero-flag `UpdateSubresource1`. Buffer coordinates use bytes; texture coordinates
use texels. The encoder emits the box-presence byte and six coordinates in the
existing Update record, retaining original pitches and API version. Initial GPU
contents are captured separately, so untouched destination regions survive.
Runtime and encoder share only Capture's layout validation, with no new Core,
Replay, Qt or original GPA dependency.

Validation checks the selected mip, array subresource, nonempty bounds, complete
structured-buffer elements and BC block alignment before reading source bytes.
The original empty-box no-op is forwarded but remains an explicit capture
refusal. Boxed constant buffers, bit/packed formats, planar formats and nonzero
copy flags are also refused. The existing DEFAULT/non-depth/single-sample and
immediate-context restrictions remain. Deferred-context source-offset behavior
is not claimed; see Microsoft's
[UpdateSubresource contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource).

WARP and hardware fixtures exercise nonzero buffer, 1D/2D/3D origins, overlapping
updates, padded rows/slices, reused caller box/data memory and unchanged exterior
bytes. BC1 additionally updates mip 1 of array layer 1 and checks every compressed
block in every subresource. An initial test used an unaligned 6x6 mip edge; both
native drivers left it unchanged and the DX11 debug layer reported invalid block
coordinates. The validator now rejects that case, and the execution fixture uses
aligned 8x8 mip regions. Original failure/debug-layer logs are retained in
`artifacts/capture-box-bc-*`; the failed case is not counted as supported.

The serial direct matrix in `artifacts/capture-box-update-final-interop` contains
twenty files and 380 checks, covering both APIs across four resource dimensions
plus BC1 mip/array storage. Exact CPU images, executed event counts, DXBC exports,
the pinned original-player hash and dead source-object lifetimes are checked.
Ten negative variants remove Update while retaining the positive reference image;
both players then execute the initial yellow contents. Nine malformed boxed
encoder models refuse publication with no final or temporary files. The new
structured-buffer negative initially changed only its descriptor and hit the
earlier descriptor/shape consistency guard; synchronizing its shape metadata
makes the test exercise the intended element-alignment refusal. Its failed
regression log is retained.

The actual startup-injection target now includes boxed texture updates for both
APIs. Seventeen launches produce 85 files, including ten boxed-update files,
and pass 545 serial dual-player/CPU/module checks in
`artifacts/capture-box-injected-interop`. Repeated capture, busy/cancel, empty-box
refusal and retry, disconnect and normal target exit pass. Loaded target modules
contain no GPA, Qt, RenderDoc or Python. All 105 direct and injected original-player
reports identify the pinned GPA 2025 R1 kernel.

The complete application builds successfully. Regression covers all 36
capture-only suites (35 in the final sweep plus the corrected encoder case's
targeted rerun), 34 full-profile capture suites and two existing structure suites.
The Qt suite reports eleven passes without skips. Summary/hashes and retained
failure paths are in `artifacts/capture-box-update-validation.json`. Replay and
shared-format sources are unchanged, so existing golden-frame evidence is not
reclassified or replaced by these tests.

This remains scoped compatibility evidence from the development original-player
private-ABI runner, not original GUI or adapter acceptance. Other BC formats have
layout handling but no new format-by-format dual-player evidence. General EVENT
serialization remains guarded and accepted GF2 frames remain **0/6**.


## Ordinary EVENT observations with a completed frame start

The encoder now accepts a strictly bounded observational query profile. The
existing private frame-start EVENT must prove S_OK/TRUE on the recorded immediate
context. A successful application GetData is accepted only when its matching End
preceded that boundary. In-frame End is preserved as an API observation. An
in-frame S_FALSE is accepted only when it matches a recorded End and has null
output with zero size. These observations promise no in-frame GPU completion.
Successful in-frame GetData, unissued/foreign/deferred histories, failed calls,
missing start proof and unavailable pending output bytes continue to reject
publication. The existing executable-resource-read experiments are not enabled
as a general query wait.

Query identities remain stable, reserved non-resource IDs. A `7/0x3152`
Query.GetDesc observation stores EVENT/zero flags; `7/0x34fa` retains End; and
`7/0x34fb` retains actual HRESULT, output presence/BOOL, size and flags. No query
is fabricated as a GPA predicate. Query1/ALL uses the same ordinary query
semantics. Capture owns this encoding and never calls Core or Replay helpers.
The reader independently recognizes an API End only after unambiguous EVENT
metadata on an immediate context. It rejects edited End payloads, does not treat
executable End as metadata, and does not disturb real predicate intervals.
Frame validation labels these End observations explicitly. The existing
`query_completion_not_saved` warning is retained for successful GetData because
the file does not materialize a native query completion wait; no new wait count
is invented.

The self-owned native fixture drains an EVENT before recording, captures its
actual completed observation, copies data, clears an output and ends the query
again. Both CreateQuery and CreateQuery1 are exercised after source COM objects
have died. Negative files remove Clear but retain the final reference image;
both players execute the initial blue contents. Six encoder guards reject with
no file publication. The startup-injected target additionally uses an EVENT from
the preceding frame with changing texture updates and Draw output. Nineteen
launches produce 95 files, including ten query-mode files, and pass 609 serial
dual-player/CPU/module checks. Evidence is in `artifacts/capture-event-injected-interop`.

A fresh GF2 launch (PID 34332) produced three files at selected-chain Presents
71071, 157985 and 161265. Each preserves one pre-frame S_OK status-only GetData,
one End and one in-frame S_FALSE status-only GetData, three DrawIndexed calls,
six Map/Unmap pairs and the observed Clears. Both players execute all three files
with exact RGBA equality to the captured backbuffer under a zero-tolerance rule
fixed before replay. The game continued presenting between captures, disconnected
and exited through CloseMainWindow. Runtime hashes, query values, images and
results are retained in `artifacts/capture-event-observation-gf2`.

All three images are black. The game's own log reports UnauthorizedAccessException
while writing LocalCache logs and deleting a LocalCache code file; initialization
then fails. The current token has medium integrity and the inherited Users ACL
on that directory grants ReadAndExecute, not write access. No game permissions or
cache files were changed. These are startup diagnostic files, not accepted scene
captures: GF2 remains **0/6**, the second launch/two scenes and original GUI
inspection remain pending. The original kernel oracle still uses the development
private-ABI runner with its pinned player hash and unverified default adapter.

The first launch command mistakenly reused its evidence directory and was refused
before a process started; the corrected invocation used a new session subdirectory.
A predicate regression initially selected the older corpus lacking modes 8–11;
the existing expanded corpus passes the corrected run. These failures are retained.
The only compile repairs were a shadowed payload variable and the test target's
D3D11.3 header for Query1. No original workspace evidence was modified.

Final regression passes all 36 capture-only suites, eleven distinct relevant
full-profile suites after the corrected corpus rerun, and four final workflow
suites after rebuilding frame validation. Query completion has 42 cases and the
Qt capture workflow has eleven, both without skips. All four existing GF2/BF1
golden and negative controls pass. The four direct observation files pass 60
checks, including two execution-removal controls. All 102 original-player reports
from the direct, injected and GF2 diagnostic matrices identify the pinned kernel.
The evidence hashes and retained failures are summarized in
`artifacts/capture-event-observation-validation.json`.

Each GF2 file exposes 42 resources, six exported/disassembled DXBC shaders and
three DrawIndexed pipeline states in Flora. Offline validation reports zero
errors, one `query_completion_not_saved` warning and ten `offline_coverage_gap`
warnings for resource families without complete offline descriptor validation.
These checks do not replace original GUI inspection or the missing scene tests.
The most recent source-archive package proof still covers the earlier whole-Update
revision; this EVENT revision has build/test and live-module evidence only.

## Subresource region copies

Capture now records `CopySubresourceRegion` and `CopySubresourceRegion1` with
zero copy flags. Its own model owns the source box, destination origin, both
subresource indexes and stable object identities. The initial GPU contents of
both operands are retained before execution. The encoder writes the existing
`7/0x40` and `7/0x256` wire layouts without linking Core or Replay; those modules
and the shared format sources are unchanged.

The supported profile uses matching buffer/texture dimensions and exact formats,
single-sample resources and DEFAULT or STAGING destinations. Coordinate checks
use subtraction after validating the destination origin, avoiding integer wrap.
Different subresources of the same resource are allowed. Same-subresource copies,
nonzero flags, format reinterpretation, empty boxes, depth/constant-buffer storage
and block/packed/planar layouts remain explicit failures. Structured buffers
additionally require matching strides and whole-element source bounds and
destination offsets. Native application calls
continue forwarding even when capture refuses a frame. The distinction between
different-subresource copies and driver-dependent overlap follows the documented
[CopySubresourceRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion)
and [CopySubresourceRegion1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-copysubresourceregion1)
contracts; the implementation does not infer overlapping-copy support.

The native fixture exercises Buffer, Texture1D, Texture2D and Texture3D using
different source/destination extents, nonzero destination origins, null and owned
source boxes, source updates between copies, and Draws between operations.
WARP and hardware results match independently computed bytes, including the
destination exterior. Initial data differs from creation data, and every source
COM object is dead before encoding. Additional native checks copy between mips
and array layers of one resource and compare all subresources; these storage
checks do not claim separate dual-player mip/array coverage.

Sixteen direct files pass 320 serial dual-player/CPU/wire/shader checks in
`artifacts/capture-region-interop`. Eight negative files remove copies while
retaining the final reference image; both players execute the unchanged yellow
destination. Eleven malformed encoder models reject without publishing or
leaking a temporary file. The first context-mismatch negative changed the initial
context and therefore hit the earlier object-reference guard; it was corrected
to alter the later copy. Both logs are retained. The existing generic recorder
failure test now uses an unsupported empty region followed by a valid copy,
checking continued native forwarding after capture failure.

Separately, the committed EVENT revision `28ba156` passes all six clean
source-archive configure/build/test/package/control steps, all 36 serial CTests
and nineteen packaged launches producing 95 native-validated files. The package
runs with system-only PATH, no Qt discovery, and no GPA/Qt/RenderDoc/Python in
target module inventories. This updates the earlier package evidence for EVENT
and boxed Updates; it does not yet establish packaged region-copy support.
Evidence is in `artifacts/capture-event-source-build/validation.json`.

The current region-copy injection matrix completes 21 launches and 105 files,
including ten new region-copy files combined with changing texture updates,
Draw and preceding-frame EVENT observations. All 673 serial dual-player/CPU/module
checks pass, including repeated captures, busy/cancel, unsupported-call refusal,
retry, disconnect and normal exit. None of the target module inventories contain
GPA, Qt, RenderDoc or Python. All 121 direct/injected original-player reports
identify the pinned GPA 2025 R1 kernel. This remains development private-ABI oracle
evidence, not original GUI or adapter-selection acceptance.

All 39 capture-only CTests and 41 relevant full-profile suites pass, including
37 capture suites, two existing structure suites and two checked-format suites.
The Qt workflow reports eleven cases without skips. A full-build attempt overlapped
CTest and could not replace the running `FloraStructureUiTests.exe`; the failure
is preserved. After the test process exited, the full build passed and both
structure suites passed their serial rerun. Evidence hashes and retained failures
are in `artifacts/capture-region-validation.json`. Shared format and replay sources
are unchanged, so the preceding golden-frame evidence remains applicable rather
than being counted as a new run. GF2 scene acceptance remains **0/6**.

## BC region copies and complete tail mips

The capture-owned region validator now accepts BC1–BC5 Texture2D block layouts.
Partial copies require block-aligned source bounds and destination origins.
A null-box, zero-offset copy between equally sized subresources preserves the
complete physical blocks, including padding in small mips. This distinction
follows the documented separation between logical mip dimensions and physical
block storage in Microsoft's
[block-compression description](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression).
BC6H/BC7, packed/planar layouts, nonzero flags, format reinterpretation and
same-subresource copies remain guarded. Encoding, format and replay code are
unchanged; only the capture model's geometry validation gains this support.

The new self-owned fixture covers BC1, BC2, BC3, BC4 and BC5 UNORM, both region
APIs, separate resources and different subresources of one resource. It copies
aligned blocks between array layers at mip 1 and whole 2x2/1x1 tail mips, updates
the source between copies, and draws between operations. All ten subresources'
compressed bytes are compared with independently constructed block data. Initial
GPU bytes differ from creation data, and the encoder runs after every source COM
object has died. The pixel shader samples both tail mips as well as the edited
8x8 surface. BC4 scalar classification uses thresholds fixed in the test source;
all exported RGBA comparisons are exact bytes without tolerance adjustments.

WARP and hardware native tests pass. Separate Debug Layer runs exercise twenty
format/API/identity combinations on each device without any warning or error.
Six malformed source-bound/destination-origin models fail the encoder's block
alignment checks without publishing or leaking temporary files. The direct
matrix in `artifacts/capture-bc-region-interop` passes 1,000 serial checks across
forty files, including twenty copy-removal controls that retain the final reference
but execute yellow in both players. It verifies command payloads, alias identities,
mip/array indexes, exact pixels, shader exports and the pinned original kernel.
This adds dual-player evidence for same-resource subresource copies, previously
tested only against native storage. New format-specific execution evidence covers
the five UNORM variants; typeless, sRGB and SNORM variants share the block geometry
but have no new variant-specific oracle in this iteration.

The preceding region revision `abf5730` also passes six clean source/archive/package
steps, all 39 serial CTests and 21 packaged launches producing 105 native-validated
files. It discovers no Qt, runs with system-only PATH and has no GPA, Qt, RenderDoc
or Python in target module inventories. Evidence is retained in
`artifacts/capture-region-source-build/validation.json`; this proves the earlier
region package, not the new BC revision's package.

The new injection modes use BC1/BC3 mip-array storage with a changing source
block, a region copy, Draw, and a preceding-frame EVENT. Twenty-three launches
produce 115 files (ten new BC files) and pass all 737 serial dual-player/CPU/module
checks, including busy/cancel, failure/retry, disconnect and normal exit. Target
module inventories contain no GPA, Qt, RenderDoc or Python. All 155 original-player
reports from the direct and injected matrices identify the pinned kernel.

The complete application builds successfully. All 42 capture-only and 44 relevant
full-profile CTests pass, including forty independent capture suites, two existing
structure suites and two format suites in the full configuration. The Qt workflow
has eleven cases without skips. GPU checks ran serially, and the full build finished
before its CTest run. Evidence hashes are recorded in
`artifacts/capture-bc-region-validation.json`. No new golden-frame run is claimed
because replay/shared-format code is unchanged. Original GUI acceptance, adapter
verification and the GF2 two-scene/six-frame gate remain incomplete; accepted GF2
frames stay **0/6**.

## Deferred compilation history foundation

Deferred contexts now keep capture-owned CPU histories outside an armed frame.
Successful `FinishCommandList` freezes a separate immutable generation on the
returned list; a failed HRESULT leaves the pending history intact. The generated
wrapper never reads the output pointer after failure. Clear/copy arguments,
resource identities, Update boxes and packed Update source bytes are owned by
the history. Caller memory may disappear immediately after the API returns.
No replay model, GPU executor or native COM reference is retained by this model.

The bookkeeping follows Microsoft's [FinishCommandList contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-finishcommandlist)
and [ExecuteCommandList contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).
Finish TRUE marks the next batch as inheriting unresolved deferred state;
Finish FALSE starts a default-state batch. Executions retain list identity,
generation, context-local serial, thread and restore flag, including repeated
execution and deferred-context merges. These serials observe CPU entry order;
they do not establish cross-context GPU ordering or query execution success.
History is bounded to 4,096 calls and 64 MiB of owned source data per pending
batch, sixteen gap messages, and 256 recent executions per context. Execution
serials continue after older observations leave the bounded queue.

This is a foundation, not deferred frame encoding. Pipeline state shadows,
deferred Map data, initial GPU contents at execution, nested-list expansion,
predicate/query dependencies and ordered lowering remain incomplete. Calls
with incomplete arguments, inherited state, changed recording threads and
exceeded limits carry explicit gaps. A captured ExecuteCommandList still refuses
publication with list/context/generation/call-count diagnostics while forwarding
the native call. CPU histories do not replace the frame-start GPU snapshots.

New WARP and hardware tests compile on separate contexts/threads, overwrite the
Update caller data, execute lists in B/A/A order, and compare exact native pixels
and TRUE/FALSE topology restoration. They check immutable generations, actual
null-output behavior, a synthetic failed-Finish result, nested native execution,
Context3 creation, call/payload/execution bounds and metadata survival after all
native objects die. No deferred `.gpa_frame` is counted as accepted.

The capture-only sweep passes 44 suites; its strengthened deferred fixtures pass
again on both devices. The full build and all 46 relevant full-profile suites
pass, including eleven Qt cases without skips. The existing-profile injection
regression produces 115 files in 23 launches and passes 737 serial dual-player,
CPU and module checks. All 115 original-player reports identify the pinned kernel.
Evidence hashes are in `artifacts/capture-deferred-history-validation.json`.
Encoder, replay and shared-format sources are unchanged; no new golden run is
claimed. The latest source-package proof still covers revision `abf5730`.
Original GUI/adapter verification and GF2 scene acceptance remain pending at
**0/6**; this iteration does not change the game's permissions or cache.

## Deferred Map storage ownership

The deferred journal now pairs Map calls with their actual HRESULT and Unmap.
It keeps the borrowed pointer only in mutable context bookkeeping, removes it
before native Unmap, and copies checked logical rows/slices while access is still
valid. Immutable lists contain owned bytes, original pitches, type/flags and
completion status. Failed Map results never inspect the output structure.
Unfinished mappings at successful Finish, invalid pitches, mismatched threads,
overlapping Maps and exceeded call/payload budgets produce explicit gaps and
drop borrowed pointers. No immediate-context GPU readback is used during deferred
compilation, and no replay or format dependency was introduced.

The model follows the dynamic-resource restrictions in Microsoft's
[deferred rendering documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render)
and preserves the results and pitch fields of
[Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map).
NO_OVERWRITE payloads are observations with an explicit unresolved-range/rename
gap. They are not treated as full-resource writes or inferred GPU dependencies.
Mapped write-combine memory is read only for the capture copy; its performance
cost has not been benchmarked. Deferred frame encoding remains refused.

Hardware byte oracles cover dynamic Buffer, Texture1D, Texture2D and Texture3D,
two separately compiled DISCARD generations, B/A/A execution, native output and
owned metadata after all source COM objects die. WARP passes Buffer and Texture1D.
On this WARP runtime, both unwrapped and wrapped deferred Texture2D/Texture3D Map
return zero row/depth pitch. Those cases verify explicit refusal without reading
or guessing storage geometry; they are not successful texture captures.

The initial NO_OVERWRITE fixture overwrote bytes used by its preceding whole
CopyResource and failed the hardware oracle. Its corrected region copy reads
only the first 48 bytes before the last 16 bytes are written, preserving the
NO_OVERWRITE promise. The corrected native bytes and repeated execution pass on
both devices. Original failures and WARP pitch diagnostics are retained under
`artifacts/capture-deferred-map-*`. Separate Debug Layer runs on WARP and hardware
report no warnings/errors. Fault tests use inaccessible sentinel addresses to
verify failed results, byte limits, thread changes, unfinished Finish, null
success and overlapping Map at the call limit without reading invalid storage.

All 46 capture-only and 48 relevant full-profile suites pass, with eleven Qt cases
without skips and a successful full application build. Existing-profile injected
regression produces 115 files in 23 launches and passes 737 serial checks against
FloraGPA, the pinned original replay kernel and independent CPU expectations.
Evidence hashes are in `artifacts/capture-deferred-map-validation.json`.
These files do not exercise deferred serialization. Encoder/replay/shared-format
sources are unchanged; no new golden run is claimed. Original GUI/adapter and
GF2 scene acceptance remain pending; GF2 stays **0/6**.


The committed deferred-Map revision `c57bd39` subsequently passes six clean
source/archive/package steps and all 46 serial CTests. A fresh directory with
spaces, system-only PATH and no Qt discovery produces a standalone package that
completes 23 launches and 115 file publications, IPC error/recovery/cancel/
disconnect checks and native identity/exit checks. Module inventories contain
no GPA, Qt, RenderDoc, Python, FloraReplay or FloraCore. Evidence is retained in
`artifacts/capture-deferred-map-source-build/validation.json`, with independent
summary hashes in `artifacts/capture-deferred-map-package-validation.json`.
These package checks establish build/runtime independence and control behavior;
the separate working-build matrix above supplies dual-player pixel comparisons.
They do not establish deferred file encoding or GF2 scene acceptance.

## Owned deferred binding arguments

The native generator now emits typed argument observations for 46 binding APIs.
Forty-five are exercised as valid deferred calls; SetResourceMinLOD is recorded
with an explicit immediate-context-only gap, matching this runtime's Debug Layer
diagnostic. The per-call CPU payload owns scalar/array bytes and stable object
IDs for all six shader stages, 11.1 constant-buffer ranges, IA, RS, OM, SO and
predication. Fixed BlendFactor arrays are copied as four floats, not addresses.
Null arrays, explicit null object slots, count-zero arrays and ignored KEEP
arguments remain distinguishable. Payloads share the existing 64 MiB journal
budget and enforce API array capacities before copying. Immediate contexts do
not build or allocate these deferred argument payloads.

These are setter inputs, not inferred effective pipeline state. Automatic hazard
unbinding, inherited state, predicate/counter behavior and command-list lowering
remain unresolved. Creation graphs for dead or driver-elided binding objects
still need retention; owning their IDs alone does not reconstruct those objects.
This input-history step does not infer effective state from setter values;
the later native getter sampling is documented below. A pending-call
slot associates each payload with its originating observation/thread; omitted,
interleaved, duplicate and budget-limited inputs cannot replace an earlier call's
arguments. Finish marks missing binding payloads as gaps. Immutable list records
contain no native COM references or caller addresses and do not depend on replay.

Microsoft's [OM binding contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews)
specifies ignored parameters under KEEP. The wrapper previously unwrapped the
depth-stencil argument even when NumRTVs requested KEEP; it now skips that
lookup. Tests pass inaccessible sentinel pointers for ignored RTV/DSV/UAV/count
arguments and verify safe native forwarding, including immediate-context RTV
getter identity. Explicit initial-count KEEP values are retained without
inventing hidden counter values.

WARP/hardware fixtures cover the 45 deferred APIs, aliased buffer arrays, original
constant ranges, viewport/scissor/blend arrays mutated before Finish, exact
native green Draw pixels and Dispatch integers, retained CPU IDs after every
native object dies, and oversized/budget/duplicate/dropped-input guards. The
immediate-only negative preserves the requested LOD value in metadata and verifies
that native resource LOD remains unchanged. Final Debug Layer runs have zero
unexpected warnings/errors and exactly one expected MinLOD diagnostic per device.
Initial logs retain the immediate-only discovery, a zero-count UAV warning, an
invalid feature-level-11.0 UAV slot in the fixture, and build fixes for an unused
generated parameter and missing test-only WRL include. The final fixture corrects
the no-op/slot cases and checks the immediate-only diagnostic explicitly.

All 48 capture-only and 50 relevant full-profile suites pass, with eleven Qt
cases without skips and a complete application build. The existing-profile
startup matrix produces 115 files in 23 launches and passes 737 serial dual-player,
CPU and module checks. Evidence hashes are in
`artifacts/capture-deferred-bindings-validation.json`. These files still do not
exercise deferred encoding. Encoder, replay and shared-format sources are
unchanged; no new golden run is claimed. The latest clean source-package proof
covers `c57bd39`; the binding revision's package, original GUI/adapter and GF2
scene gates remain pending. Accepted GF2 scene frames remain **0/6**.

## Durable deferred creation graphs

Deferred binding arguments and direct Clear/Copy/Map/Update/query operands now
retain independent creation definitions. A native-only traversal closes view
resource and child device edges while the application object is still valid.
The journal owns descriptor bytes, shader/input-layout DXBC, semantic strings,
stream-output declarations/strides and original resource creation data. It owns
neither native COM objects nor live registry records. IDs are deduplicated within
the batch; successful Finish moves the complete graph into the immutable list.
Later metadata mutation or native destruction cannot alter that graph.

Definition bytes count toward the existing 64 MiB journal payload budget, with
an additional 8,192-object limit. New dependencies are staged before merging, so
budget, device or unsupported-creation failures do not publish a partial graph.
Cached identities still validate the expected device. Missing shader/layout
creation evidence, unclassified objects and linked shaders fail explicitly;
class-instance/linkage reconstruction and nested CommandList graphs remain
pending. Traversal uses scoped native identity/device/resource getters, without
GPU snapshots, pipeline changes, replay helpers or GPA modules.

The new WARP/hardware fixture destroys every source COM object, then recreates
the retained definitions on a fresh, unwrapped WARP device using CPU data only.
It covers buffers, a Texture2D and RTV/SRV/UAV dependencies, VS/PS/CS DXBC, an
unused geometry shader with SO declarations, input-layout semantics, sampler,
blend, depth/stencil and rasterizer state. Every retained node is recreated and
every device/resource edge is present. Independently expected pixels and compute
integers validate the reconstructed objects; semantic names mutated by the
caller and a temporarily modified source metadata blob do not change the graph.

Creation data is deliberately distinguished from execution-time GPU contents.
The reconstructed texture initially reads blue. A constant buffer was created
red, then updated green before the source deferred Draw. Source execution draws
green; recreation from creation definitions draws red, as independently expected.
Both compute paths produce 18/19/20/21. This is a creation-graph oracle, not a
complete frame replay: execution-time initial data still needs its own capture.

Atomic dependency-budget failure, duplicate definitions, cached/uncached device
mismatch, object-count overflow and unsupported class-linkage guards pass. Debug
Layer runs on both source devices and their recreated WARP devices have no warnings
or errors. Initial failed shader compilation is retained; the SO fixture required
a semantic-bearing output structure, which was corrected before validation.

All 50 capture-only and 52 relevant full-profile suites pass, with eleven Qt cases
without skips and a complete application build. Existing-profile injection again
produces 115 files in 23 launches and passes 737 serial dual-player/CPU/module
checks. Evidence hashes are in `artifacts/capture-deferred-definitions-validation.json`.
Encoder, replay and shared format remain unchanged; no new golden run is claimed.
Effective state/hazards, inherited state, hidden counters, execution snapshots and
deferred encoding remain unfinished. The latest clean source-package proof is
`c57bd39`; current package, original GUI/adapter and GF2 scene acceptance remain
pending. GF2 accepted scene frames remain **0/6**.

## Effective deferred pipeline bindings

Each supported deferred Draw/Dispatch, Clear, Copy, Update and Map action now
owns an immutable CPU snapshot of the native context's effective bindings before
that action. The sampler shares capture-runtime getter code with immediate
capture; neither path depends on replay. It reads the six shader stages, constant
ranges, IA/RS/OM, SO and predicate bindings. It does not issue draws, copies,
readbacks, flushes or state-setting calls. Temporary getter references are released
after a closed creation graph and CPU values have been retained. Binding-only
calls allocate no pipeline snapshot; snapshots and their variable arrays count
toward the existing 64 MiB payload limit.

The native runtime supplies hazard-resolved bindings, rather than a new software
simulation of overlapping views. WARP and hardware tests cover output-induced
SRV unbinding, rejected conflicting SRV inputs, non-overlapping array slices,
CS UAV hazards, KEEP, explicit ClearState and high shader slots. All six stages'
constant-buffer ranges are checked. This is evidence for those profiles, not an
exhaustive validation of every view format or protected context.

An action in a new list samples the retained context after
[`FinishCommandList(TRUE)`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-finishcommandlist).
Its definitions include inherited bound objects even if the earlier command list
and the application's shader references were released. `FALSE` and `ClearState`
are checked against native default bindings. The old unconditional inherited-state
gap is removed; unsupported actions and hidden execution state still refuse.
No prior list's Draw/Dispatch is copied into the next list.

Getter-visible bindings do not establish execution-time resource contents or
counter values. Deferred sampling never calls GetResourceMinLOD; resource clamp,
APPEND/COUNTER UAVs, SO append position and active predicates retain explicit
gaps. Counter reset setter arguments remain separate evidence. Early snapshot
budget failure and a later creation-dependency budget failure publish no partial
state. The existing thread/reentrancy, call-count and object-count guards remain.

The new fixture executes an original deferred Draw and a setter-free successor,
then independently recreates shaders and applies the captured state to reproduce
exact red pixels. A compute consumer produces exact integers 71/72/73/74. Both
WARP and hardware source devices pass. With the Debug Layer enabled, ordinary
profiles have no warnings/errors; deliberately conflicting bindings produce
exactly six warnings with identical IDs/severities/order in native and wrapped
runs. Initial build shadowing and a raw-baseline ignored-pointer sentinel crash
are retained in local logs. The latter fixture was corrected to use the null
native arguments emitted by the wrapper; wrapped KEEP still uses inaccessible
sentinels. No error tolerance was relaxed.

Full regression results and hashes are recorded in the accompanying capture
baseline and ignored `artifacts/capture-deferred-state-validation.json`.
Deferred execution-time snapshots, command-list lowering/encoding, original GUI
acceptance and GF2 scene acceptance remain pending. Accepted deferred files and
GF2 scene frames remain **0** and **0/6**, respectively. The latest clean
source-package proof still covers `c57bd39`.

## Precompiled deferred execution capture

The execution stage supersedes the earlier zero-file deferred milestones above.
Supported precompiled lists now expand at each actual immediate-context
`ExecuteCommandList` call, preserving execution order and repeated calls. CPU
provenance retains list/context IDs, generation, execution ordinal, source call
sequence and the restore flag. The file contains ordinary executable actions;
native CommandList groups are not encoded. The existing independent encoder is
unchanged, and no replay or shared-format implementation is reused or modified.

Native resource leases are separate from immutable CPU histories. Context/list
lifetime entries own resource references without referencing their parent COM
object. Successful Finish transfers the leases, failed Finish retains them, and
null-output Finish discards them. Execution borrows a shared lease while its
caller owns the list; no weak native pointer is promoted. Resource contents are
read before the first recorded use at the immediate execution position. Existing
initial data is retained across repeated lists and earlier recorded writes.
Creation bytes and compilation-time state do not substitute for those contents.

The supported lowering covers Draw variants, Dispatch, Clear, CopyResource,
CopySubresourceRegion/1 and UpdateSubresource/1 within the existing encoder's
profiles. Missing definitions, device mismatches, payload/command limits,
cancellation and unsupported actions reject publication while native calls
continue to forward. Deferred Map rename dependencies, nested lists, query and
hidden counter/predicate/SO/MinLOD state remain unsupported. Compilation during
the captured frame and execution on additional contexts/threads also refuse.
This is not general multithreaded deferred capture.

The WARP/hardware execution fixture compiles compute before graphics, changes a
buffer from creation value 9 to execution value 1, releases application resource
and shader references, and captures Draw/Dispatch/Dispatch/Draw. The result is
integer 3 and exact red pixels. The next frame starts at 3 and produces 4 and
green pixels. TRUE restoration and FALSE reset are checked against native state,
following the [ExecuteCommandList contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).
All source COM objects are destroyed before encoding; CPU histories remain
readable and native lifetime counts return to zero. Debug Layer runs have no
warnings or errors.

Four positive files and four negative controls pass 132 serial dual-player,
independent exact-pixel, event, inventory and DXBC/disassembly checks. Removing
one Dispatch changes output to blue; removing both Draws leaves black despite
the unchanged red preview. The original oracle executes the pinned GPA player
kernel through the development runner; this does not establish original GUI or
adapter acceptance. Startup injection additionally exercises five precompiled
Draw files in mode 23: the expanded matrix completes 24 launches, 120 files and
769 exact dual-player/module checks. All 54 capture-only and 56 selected
full-profile tests pass, including eleven Qt cases with no skips. Final evidence
hashes are recorded in the capture baseline and
`artifacts/capture-deferred-execution-validation.json`.

Reproduce the focused run with `tools/validate_capture_deferred.py`, passing
the capture-only `FloraCaptureDeferredExecutionTests.exe`, the replay CLI,
reference development tools, Qt DLL directory and a new evidence directory.
`tools/validate_capture_controller.py` performs the separate injected matrix.
Both validators execute GPU work serially; Python is only a development driver.

GF2 scene acceptance remains **0/6**. The user chose to restart Codex elevated
for the LocalCache permission blocker; the current process still reports no
administrator membership. No game permissions were changed. The latest clean
source-package proof remains `c57bd39`; current package proof, broader deferred
profiles, original GUI/adapter checks and the six GF2 scene files remain pending.

## Elevated GF2 preflight and mapped-resource dependencies

The 2026-10-12 elevated continuation supersedes the preceding permission status.
Administrator membership is confirmed, LocalCache initialization succeeds, and
GF2 reaches both its animated start screen and its main scene. Four diagnostic
launches are retained in `artifacts/capture-gf2-elevated-20261012`; they are not
the formal two-launch, six-frame acceptance series. No game permissions or
graphics settings were changed.

The start-screen preflight file contains a colored 2560 x 1440 frame, 18 Draws,
six Clears, three region copies and fifteen Map/Unmap pairs. Flora and the pinned
original GPA kernel execute it and exactly reproduce its captured backbuffer:
RGBA SHA256 `080045e251b46e82817935be8ac300e72f6d50ca5c4fcdc43b246a38f0bad75a`.
This is a same-frame zero-tolerance comparison, not a later game screenshot.
One existing missing-query-completion notice remains; exact pixels alone do not
prove all query synchronization semantics.

The installed GPA 2025 R1 Graphics Frame Analyzer also opens this file on
NVIDIA GeForce RTX 3070 Laptop GPU. Its API Log shows 27 events. Selecting
DrawIndexed event 11 exposes vertex shader SH:0x4, DXBC `vs_4_0`, constant/vertex
bindings, depth enabled with ALWAYS/ALL, stencil disabled, and SOLID/NONE
rasterization. Texture T:0x5a renders its 3584 x 2016 RGBA8 SRGB contents and
histogram. The final render target displays the captured scene. These are
actual GUI observations for this file, not GUI acceptance of the remaining
profiles. The analyzer's initial folder dialog could not be positioned reliably
by automation; its registered file-open command successfully opened the file.

Main-scene attempts exposed overly broad mapping guards. The runtime now allows
an untouched resource to remain mapped across a captured frame. It checks each
retained immediate or deferred dependency on every use, including when initial
data was already cached. Map retains only its context, device and direct operand;
it does not sample unrelated bound pipeline resources. The encoder independently
checks mapped direct operands and bound resources. The
[D3D11 Map contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
denies GPU access to the mapped subresource; unrelated GPU work may continue.
Our dependency checks remain conservative at whole-resource granularity and
include bound resources even when a particular shader might not use them.

Unmap without an owned in-frame Map, unfinished recorded maps, mapped dependencies,
foreign-context/thread execution and unsupported DXGI mappings still refuse.
No pre-frame mapped pointer is read. Status now reports the total number of active
mappings and at most sixteen resource/context/subresource identities. WARP and
hardware tests check untouched CPU bytes, native red output, independent Clears
inside Map/Unmap, concurrent maps with a bound mapped constant buffer, cached
dependency rejection and malformed encoder ordering. Interleaved generated frames
exercise real execution in both players; map-removal controls retain the original
preview but produce blue output.

The fourth launch passes those mapping guards and explicitly fails at
`ID3D11DeviceContext::ClearRenderTargetView: MSAA sample-preserving capture is not implemented`.
No main-scene file is published. The game continues rendering after refusal and
disconnect, and exits normally. Formal GF2 acceptance remains **0/6**.
MSAA requires independent per-sample snapshot and verified initialization records;
ordinary legacy GenData is insufficient, as the retained-sample experiment in
`D:/CDXrepo/FloraGPA/analysis/ACTIVE_MSAA.md` already demonstrates. Resolving the
texture or omitting its initial contents would not satisfy this gate.

Final test counts, input hashes and retained evidence paths are recorded in the
`elevated_gf2_map_scope` section of `capture-development-baseline.json` and the
ignored run summary. Replay and shared format sources are unchanged. The latest
clean source-package proof remains `c57bd39`; current package validation, MSAA,
broader query/deferred profiles and the six formal scene frames remain pending.

## MSAA sample readback foundation

The subsequent [capture-owned MSAA sample primitive](CAPTURE_MSAA_SAMPLES.md)
preserves color storage bits per array layer and sample, with native WARP/hardware
oracles and state-isolation checks. It is separate from ordinary resource data
and has no Replay dependency. A subsequent depth/stencil extension verifies
D16/D32/D24S8/D32S8 and typeless variants against independent native storage on
both devices. MSAA production snapshot/encoding guards remain;
verified dual-player initialization and production integration are still required before
restarting formal GF2 acceptance. This step does not turn the diagnostic files
above into accepted scene frames.

The subsequent executable-initialization experiment reconstructs 2x/4x RGBA8
array samples in both players using explicit helper Draw/Copy records, with
initialization-removal and application-Draw-removal controls. It also exposed
and fixed a production encoder scissor-mask omission; populated and empty arrays
now update in original GPA. The test-only MSAA writer is not production MSAA
capture support. Scope, retained failures and evidence are described in
[the MSAA report](CAPTURE_MSAA_SAMPLES.md#executable-initialization-experiment).

The capture encoder now accepts a separate, validated `initial_msaa` model and
emits the scoped RGBA8 initialization using build-time DXBC. Its positive files
pass both players with deletion controls; model limits and view/identity checks
refuse malformed input before publication. This supersedes the test-only writer
limitation for that CPU encoding path. At that stage runtime recording still
rejected MSAA and did not populate the model. Depth/stencil initialization, broader profiles and
the formal GF2 gate remain pending; see the report's encoder section.

The subsequent immediate-context path now fills the separate owned sample map
before first use of supported RGBA8 2x/4x quality-zero resources. Actual wrapped
Draws preserve their initial samples and native output, and freeze releases
all native owners before CPU encoding. Twelve files / 248 dual-player checks
include removal controls for both application and initialization Draws.
Deferred MSAA, unsupported initialization formats, query/predicate combinations
and MSAA swap-chain output remain refused. Startup-injected MSAA and GF2
acceptance remain pending; see the MSAA report's immediate recording section.

Two subsequent native startup-injected profiles now exercise persistent MSAA
color textures with direct/blt Present and factory/flip Present1. Three
consecutive captures, rejection/recovery, cancellation and disconnect pass;
eight actual files and four Draw-removal controls pass 280 exact dual-player
checks. The target independently verifies its native output after disconnect.
The source-package gate includes these paths as a separate seventh stage.
Depth/other initialization profiles and formal GF2 scene acceptance remain
incomplete; see the MSAA report's startup-injected section.

Committed package `4330a64` passes 96 CTests and all seven independent-source
stages. Its ten MSAA files plus four removal controls pass 325 dual-player
checks. A new GF2 main-scene attempt with that package pinpoints the next gap:
`ClearRenderTargetView` needs format 9 (`R16G16B16A16_TYPELESS`), 2x sample
initialization. The request refuses without publishing; the game continues
rendering and closes normally after disconnect. Formal GF2 acceptance remains
0/6. Raw evidence and scope are recorded in the MSAA baseline/report.

RGBA16 color initialization is subsequently admitted by the shared runtime/CPU
table and family-aware view checks. WARP/hardware tests verify all high/low
component bytes and exact FLOAT RTV writes through 12 files / 250 dual-player
checks. A new GF2 main-scene preflight now first refuses format 19
(`R32G8X24_TYPELESS`), 2x MSAA; it publishes nothing and the game continues
rendering. The next required implementation is depth/stencil initialization.
This diagnostic launch does not count toward the formal six-frame gate.

D32S8 initialization is now connected to the capture-owned immediate recording
model and CPU encoder after a separate 306-check wire experiment. Production
recording passes 12 files / 274 exact dual-player checks and rejects unverified
depth values, padding and view profiles before publication. A new elevated GF2
main-scene attempt first refuses `ResolveSubresource` recording; it publishes
nothing, continues rendering and exits normally after disconnect. Formal GF2
acceptance stays 0/6. Latest standalone source-package evidence is `a2a3465`
(98 CTests, seven stages), before depth integration. The MSAA report and baseline
retain each phase's exact scope, hashes and remaining work.
