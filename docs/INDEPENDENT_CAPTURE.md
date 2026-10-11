# Independent DX11 capture

Implementation is **partial**. The native controller now launches, injects and
captures a limited blit/flip DX11 profile through private IPC. Completed
`.gpa_frame` files execute in FloraGPA and the installed original GPA kernel.
Unsupported profiles fail explicitly. Qt exposes the same workflow and passes
only completed file paths into replay. The full planned release, original GUI
inspection and GF2 six-frame gate remain incomplete. GF2 passes its two-buffer
FLIP_SEQUENTIAL / ALLOW_TEARING boundary, then rejects the first unsupported Map.

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
