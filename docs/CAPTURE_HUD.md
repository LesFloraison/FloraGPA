# Native capture HUD

Current integration work is described in **Runtime clean presentation storage**
below. Earlier sections retain their original component-only scope and evidence.

Implementation branch: `feature/dx11-capture`. This work is separate from
legacy migration parity and the six-frame GF2 acceptance. Delivery is incomplete.

## Control foundation

`FloraCaptureHud` owns mode, configurable hotkey validation, foreground/rising-edge
input decisions, monotonic per-swap FPS samples and expiring messages. It has no
dependency on Qt, Replay, Core, the file format, capture recording or GPA.
Runtime composition connects this model to presentation and capture control.

IPC and input handling share `submit_capture_request`: one busy check,
capability/foreground selection, UUID validation, output naming and arm operation.
Disconnect permanently disables the service while existing wrappers keep forwarding.

Foundation verification: `capture_hud_model`, `capture_boundaries`,
`capture_controller` and `capture_bootstrap` pass in the capture-only Release build.
The existing injected controller matrix still runs through the shared request service.
The foundation commit alone draws no HUD; integration is described below.

## Native pixel component

The independent `PixelTransaction` component rasterizes owned Arial text with
GDI, composites a small CPU region, and uses private native DX11 textures to
back up, display and restore that region. It changes no pipeline bindings and
creates no shaders. The synchronous region readback is an intentionally limited
first implementation; its performance cost is included in the measured FPS.

The supported pixel experiment is single-buffer DISCARD or SEQUENTIAL with
single-sample RGBA8/BGRA8. Multi-buffer, flagged/partial Present, protected/shared
presentation and unsupported formats skip before modifying the backbuffer.
Private references exist only for the transaction, so ResizeBuffers can succeed
after it ends. Runtime composition must additionally establish query, mapping,
predication and concurrency safety and exclude native calls from recording.

`capture_hud_pixels` passes 24 retained frames across both effects, with compact
and detailed modes, exact whole-buffer comparisons, output binding preservation,
TEST/partial skips, simulated failed-Present restoration, Resize and no debug-layer
warnings. A separate hardware `--exclusive` run passes 24 frames while checking
GetFullscreenState on every iteration. The earlier fullscreen attempts returned
0x887A0022 until the test window obtained foreground focus; those failures are
retained, not counted as successful fullscreen tests. This proves the scoped
single-buffer experiment only, not general flip or multi-buffer isolation.

Evidence: `artifacts/capture-hud-focused-tests.log`,
`artifacts/capture-hud-pixels-exclusive-verified.log` and the native test sources.

## Presentation investigation

`FloraCaptureHudBufferProbe` is a native development experiment, not a runtime
dependency or an acceptance test. Ignored results:
`artifacts/capture-hud-buffer-probe.json` and
`artifacts/capture-hud-buffer-probe-slots.json`.

On the local NVIDIA device, three-buffer SEQUENTIAL and FLIP_SEQUENTIAL preserve
the COM identity of GetBuffer(0), but rotate its contents after successful Present.
The submitted image becomes logical slot 2. Copying a backup to the retained
GetBuffer(0) therefore restores the wrong buffer. Attempted copies to the other
slots do not restore their pixels. FLIP_DISCARD rejects GetBuffer(1/2).
Single-buffer DISCARD accepted restoration in this small experiment.
The debug-layer follow-up `artifacts/capture-hud-buffer-debug.json` identifies
message 285: the destination buffer is read-only. These observations do not
establish a general safe restoration algorithm. The separate single-buffer test
above does not cover multi-buffer preservation or actual driver Present failures.

An unsafe presentation path must skip HUD before drawing and report its reason.
No change may assume that stable COM identity implies stable physical storage.

## Reference research

Read-only Ghidra inspection of GPA 2025 R1 `shimd3d64.dll`:
SHA-256 `cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31`.
Raw output is ignored under `artifacts/capture-hud-plan`.

* RVA 0x329270: HUD manager brackets view drawing with renderer virtual calls
  at offsets 0x38 and 0x40.
* RVA 0x328970: monotonic elapsed-time FPS refresh at approximately one second.
* RVA 0x327430: DX11 renderer initialization through 0x337d60; Arial view setup.
* RVA 0x2ff200: `GPA::DrawHUD` path temporarily changes a recording-related
  global around HUD rendering. This alone proves neither clean backbuffer
  restoration nor clean retained contents on subsequent frames.
* RVA 0x3000d0: frame/capture boundary work precedes the HUD call.
* RenderDirect3D11 vtable RVA 0x43af58: slots 7/8 lead to 0x337aa0/0x3384f0.
  They bracket HUD setup/drawing with context getter/setter helpers 0x340cc0
  and 0x340800. These are pipeline-state operations, not proof of pixel isolation.
* Present wrapper RVA 0x39620 calls 0x3000d0, invokes the saved original Present
  function with application arguments, then calls 0x300680. The latter contains
  metrics/frame-boundary work; the inspected paths have not established a safe
  copy-back algorithm for the observed read-only rotated buffers.

Original binaries, fonts and shaders are not product dependencies or copied assets.
The [Intel HUD reference](https://www.intel.com/content/www/us/en/docs/gpa/user-guide/2025-1/heads-up-display-hud-ui-reference.html)
describes the default mode/capture shortcuts and FPS timing.

## Runtime and controls (limited profile)

`HudDraw` composes the component inside Capture Runtime: input requests are
consumed before the existing capture boundary, the clean frame is frozen before
HUD work, the original Present/Present1 receives unchanged arguments, and the
region is restored before `capture_after_present`. Native-only interfaces under
`NativeCall` exclude private textures/copies from object registration, command
recording and deferred history. Restore failure disables subsequent capture and
fails the current presentation transaction without replacing the application's
HRESULT or LastError. No backbuffer reference survives the Present transaction.

The original single-buffer fallback requires `D3D11_CREATE_DEVICE_SINGLETHREADED`.
Clean presentation storage now enables supported multi-buffer chains and serializes
wrapped context access; see the integration section below. Active queries,
predicates, external/texture mappings and untracked interfaces still skip HUD.
Outstanding buffer maps are allowed only in the isolated copy-only path, which
does not bind or access those buffers. Broader concurrency and failure coverage
remain incomplete.

Protocol **v3** rejects older manifests and requests. Launch accepts:

```
--hud compact|detailed|off
--hud-hotkey Ctrl+F1|off
--capture-hotkey Ctrl+Shift+C|off
```

Hotkeys accept Ctrl/Shift/Alt with A–Z, 0–9 or F1–F24. Invalid/repeated components
and duplicate bindings reject before creating an output directory or launching
the target. Qt has a collapsed HUD settings section using the same async CLI.
`select --session SESSION_JSON --swap-chain ID` sets the hotkey's explicit target;
ID 0 restores automatic unique-foreground selection. The Qt swap selector sends
this command. It does not infer an arbitrary swap chain.

A target-thread `WH_GETMESSAGE` observer records non-repeat keyboard messages
from the foreground target process and always calls the next hook without
modifying the message. Normal Present consumes those requests; TEST does not.
The control worker alone performs UUID generation, selection, busy checking and
arming; the message hook performs no GPU or disk work. Disconnect removes all
input hooks, stops the worker/HUD and leaves COM forwarding active.
For `RIDEV_NOLEGACY` keyboards, the same observer reads `WM_INPUT` and tracks
ordered make/break events, including left/right modifiers and generic Shift with
no scan code. Existing raw keyboard target threads are observed without changing
their device registrations. When legacy messages are enabled, only those messages
trigger hotkeys, avoiding duplicate actions from the raw copy. Focus changes
resynchronize held chord state without firing it. Buffered-only Raw Input that
bypasses `WM_INPUT` remains unobserved and is reported in the input profile.

The initial Present-only polling approach missed short synthetic presses; native
legacy and raw message paths have now been verified with short key presses.
Pure state-machine repeat/focus tests are not evidence of a physical long hold.

Status reports mode, per-swap actual visibility/skips, measured FPS, input profile,
selected swap, request outcome and expiring messages. Only successful non-TEST
Presents contribute to FPS. Saved filenames last four seconds; errors last eight.
Request identity distinguishes repeated identical failures. Full request/error
details remain in CLI status and the Qt task log.

## Earlier single-buffer integration evidence

* Complete capture-only/full builds; **57/57** capture-only and **60/60** selected
  full-profile CTests pass, including **12 Qt cases**, no skips. Logs:
  `artifacts/capture-hud-frozen-*-build.log`, `capture-hud-all-capture-tests.log`,
  `capture-hud-full-tests.log`. Replay and shared format are unchanged, so no new
  legacy golden-frame claim is made.
* Native screen inspection verifies compact/detailed/hidden modes, measured FPS,
  adapter ellipsis, capture via Ctrl+Shift+C, and capture while hidden. The test
  application retains every pixel beneath the HUD; final reads after 8,605 and
  7,506 frames match the CPU oracle exactly. Three initial Update-only captures
  execute exactly in both players. These exploratory files remain ignored.
* The final standalone package runs with system-only PATH. Three consecutive
  files from `FloraCaptureHudLiveTarget` pass **82** serial checks: both players,
  exact clean preview/initial retained area/final reference, one application
  texture, two application shaders, no HUD buffers or extra Copy/Draw events,
  shader export, and three Draw-zero controls. Zeroing only the application's
  Draw changes the last output pixel in both players while retaining the original
  preview/reference. Final application bytes after 803 frames are exact.
  Evidence: `artifacts/capture-hud-final-20261013/validation`, `module-audit.json`;
  reproducible checker: `tools/validate_capture_hud.py`. No GPA, Qt, Replay, Core,
  RenderDoc or Python modules occur in the capture target inventory.
* Fresh GF2 launch: the hotkey produces a start-screen frame with 22 Draws that
  executes identically in both players and matches its captured reference. Main
  scene hotkey capture explicitly refuses MSAA sample preservation and publishes
  no additional file. Disconnect removes the input hook; the game continues and
  then closes normally. The game also receives the shortcut's C key, opening its
  equipment UI, as required by the non-consuming input policy. HUD rendering is
  skipped with `Multi-buffer HUD pixel restoration is unverified`; this is **not**
  GF2 HUD acceptance. Evidence: `artifacts/capture-hud-gf2-20261013`.

Hashes and scoped outcomes are recorded in `capture-hud-evidence.json`. Build
products, screenshots, captures and raw Ghidra output are not committed.

## Outstanding gates

General concurrent-context stress, buffered-only raw-input hotkeys, the broader DPI and
resolution matrix, native long-hold/focus stress, active-query/hidden-state failure
matrix and actual driver failure/device-loss cases remain open. Multi-buffer,
borderless and exclusive integration evidence is recorded below and does not
establish those broader cases. Formal GF2 acceptance remains 0/6.

## Clean presentation storage investigation

Further read-only Ghidra output is in
`artifacts/capture-hud-plan/hud-post-chain.{c,txt,log}`, for the same pinned shim.
RVA 0x339e90 unmaps the HUD vertex/index buffers, selects a HUD pixel shader,
binds each cached RTV and issues DrawIndexed (context slot 12). RVA 0x3384f0
then restores pipeline state through 0x340800. This is batch submission before
the native Present, not a post-Present pixel restore. Post-Present 0x300a40 is
conditional capture/frame bookkeeping; 0x3020e0 processes a pending output
string. These additional paths still provide no proof of retained-pixel cleanup.
The original behavior must not be used as evidence that our stricter isolation
requirement is automatically satisfied.

The recorded GF2 swap is two-buffer FLIP_SEQUENTIAL, RGBA8, sample count one,
with Present interval zero and ALLOW_TEARING. Its main-scene MSAA refusal concerns
other captured resources, not this final backbuffer's sample count.

`CleanBufferRing` is a native storage primitive, **not yet wired into runtime**.
Application logical buffers remain clean; only a copy into the real presentation
surface receives display decoration. After successful sequencing Present, clean
contents rotate in place while logical resource identities stay stable. No
persistent reference to a native swap buffer is held. The eventual runtime
facade must preserve GetBuffer/QI/GetResource identity, read-only slot behavior,
Present unbinding, direct/indirect Resize references and synchronization. Capture
must use the application's clean logical buffer, and hidden/disconnected modes
must preserve that same application resource model until destruction. A new ring
is required for every successful Resize, even when dimensions do not change.

The native `capture_hud_clean_buffers` experiment passes on the local NVIDIA
hardware: ten profiles (two/three buffers, SEQUENTIAL/FLIP_SEQUENTIAL/FLIP_DISCARD,
plus tearing for flip), 240 decorated frames and 360 readable retained-slot
comparisons. Baseline and isolated runs match exact bytes before Present and in
every readable retained slot afterward. Present and Present1 alternate; the HUD
test marker's region is never overwritten by application updates. Flip-discard
uses a full application redraw and makes no claim about undefined post-Present
contents. TEST and synthetic failed/occluded/non-sequencing ring transitions do
not rotate clean data. No Debug Layer warning/error occurs. This is a storage
algorithm test, not a COM facade, actual driver failure or GF2 acceptance test.

Both capture-only and full profiles build this target and pass the new test plus
the architecture boundary check. Exact output is retained in
`artifacts/capture-hud-clean-ring-ctest-details.log`; earlier handwritten prototype
output remains in `artifacts/capture-hud-clean-buffers.json`. Existing 57/60 counts
above refer to the previous integrated revision, not a new complete regression.

Microsoft documents the slot access and implicit unbinding requirements in
[DXGI_SWAP_EFFECT](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect)
and the [flip model guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model).

## Presentation resource identity foundation

`PresentationResource` connects an original native texture identity with clean
GPU storage. The application's resource methods still use the original object,
including DXGI usage flags. GPU operands use the clean texture. Returned view
resources are canonicalized back to the original identity, and metadata lookup
uses that same identity, avoiding private clone definitions in captured files.
Native views retain the original buffer through a reserved lifetime interface so
indirect references continue to prevent native ResizeBuffers. Creating a view
first validates the original resource, preserving read-only slot restrictions.

The hardware Debug Layer test `capture_presentation_resources` verifies cross-
interface identity, GetResource and context getters, original DXGI usage, exact
clean snapshot bytes despite different native pixels, a single metadata record
per public buffer, matching native read-only rejection diagnostics, and native
Resize failure/success before/after a retained view is released. Unexpected
warnings/errors are checked separately from the intentional native rejections.
The initial missing test include dependency and extended descriptor parameter
name build errors are retained in the ignored build logs.

Registration is currently exercised by that test only. Automatic swap-chain
activation, synchronized Resize registration transitions, implicit Present
unbinding and clean capture boundary selection are still pending. No GF2 visible
HUD or new frame-file compatibility claim follows from this identity test.

Both complete builds succeed; all 59 capture-only and 62 selected full-profile
CTests pass serially, including 12 Qt cases. Logs are
`artifacts/capture-presentation-resources-{all,full}-{build,tests}.log` and
`artifacts/capture-presentation-resources-ctest-details.log`. Replay and shared
format remain unchanged; no new legacy golden-frame or GF2 run is claimed.

## Native Present binding transaction

`PresentBindings` temporarily mirrors clean-buffer SRV, RTV and UAV bindings on
the native presentation buffer. The real Present decides which bindings survive;
only surviving mirrors are mapped back to the application's original views.
It never restores a binding that Present removed. All mirror allocations finish
before changing state. Native getter arrays are sampled in batches, unrelated
views remain bound, and UAV counters use KEEP values. This component still
requires the caller's context exclusion and internal recording scope; it is not
automatically enabled in the runtime.

The hardware comparison covers 192 native/facade pairs: feature levels 11.0 and
11.1, Present/Present1, SEQUENTIAL/FLIP_SEQUENTIAL/FLIP_DISCARD, normal/TEST/
DO_NOT_SEQUENCE/invalid-interval calls and RTV/six-stage SRV/CS UAV/OM UAV
bindings. HRESULTs, surviving view identities and Debug Layer diagnostic IDs
match. Unrelated UAV counters remain exactly 37, and LastError is preserved.
The initial experiment found that updating one OM UAV slot cleared unrelated
slots outside that range; the implementation now preserves the whole surviving
range. Initial failed results remain in the ignored artifacts.

Clean-storage comparisons now also exercise real DO_NOT_SEQUENCE calls: sixteen
profiles, 384 decorated frames and 600 readable retained-slot comparisons. All
initialized native and clean bytes match. DO_NOT_SEQUENCE prevents content
rotation on this adapter even when flip Present unbinds views. Initial native
slots must be initialized with sequencing presents; otherwise comparisons read
undefined contents. This corrects the experimental fixture, not the rotation
implementation. Native Present returning OCCLUDED was recorded as a failure;
after activating the visible diagnostic window with a click, unchanged native
cases passed. OCCLUDED is never accepted as successful presentation.

These are component checks, not new GF2, exclusive-fullscreen facade, capture
file or runtime concurrency acceptance. Automatic activation and capture/Resize
transactions remain pending. Replay and shared format are unchanged.

## Runtime clean presentation storage

`PresentationStorage` now connects the tested clean-buffer and binding components
to wrapped swap chains. Application GPU operands resolve to private clean storage,
while public COM identities, descriptors and DXGI usage remain native. Capture
freezes that clean resource before decorating the native presentation surface.
After the real Present, surviving bindings are translated back and logical clean
contents advance only on a successful sequencing Present. The newly writable
native slot receives its clean contents before the next capture boundary. The
submitted physical buffer is never assumed to be the subsequent buffer zero.

Context calls and Present share a recursive exclusion gate; alias changes use a
separate gate so unrelated CPU device creation can continue while native Present
waits on the window thread. Resize drops aliases and native buffer references
before DXGI. Failure reattaches the prior clean generation; success allocates a
new generation even at unchanged dimensions. Hidden/disconnected sessions retain
the forwarding storage needed by existing views until their lifecycle permits
release, with no HUD or keyboard observer after disconnect.

GF2 exposed a required DXGI view rule: a typed UNORM swap buffer accepts an sRGB
RTV, but an ordinary typed UNORM clone rejects it with `E_INVALIDARG`. Clean RGBA8
and BGRA8 textures therefore use the corresponding typeless storage. View creation
first validates against the original resource; null descriptions are resolved from
a temporary native view to preserve DXGI default typed views. Public descriptors
and captured snapshot formats retain the original UNORM type. RGBA/BGRA Debug
Layer tests compare native sRGB clear bytes, descriptors, validation-only calls,
read-only restrictions, identity, mappings and Resize lifetimes. Earlier black
GF2 runs and the failed view HRESULTs remain in ignored artifacts.

The final injected retained-scene experiment covers two-buffer blt, two/three-
buffer flip, monitor-sized borderless and exclusive fullscreen. Each produces
three captures; all **15 files / 410 checks** pass both players, exact preview and
resource comparisons, shader/resource inventory and Draw-zero negative controls.
Application exit readbacks are exact in all five sessions. The four windowed
profiles additionally verify retained-reference Resize rejection, TEST, real
invalid-interval Present failure, partial Present1 and same-size Resize, reading
the untouched HUD region after each step. Exclusive fullscreen is queried on
every frame. This does not establish device-removal or arbitrary driver failures.

Reproduce with `tools/validate_capture_hud_session.py` followed by
`tools/validate_capture_hud.py` (use `--flip` except for blt). Evidence is under
`artifacts/capture-hud-final-storage-{blt,flip,flip3,borderless,exclusive}`.
The native application fixture has no Python dependency; these scripts are
development orchestration and comparison tools. Earlier longer runtime sessions
also retain exact readbacks and three-file dual-player results, but predate the
sRGB correction and are not substituted for final-code checks.

Private presentation copies also preserve the application's exact predication
binding and value. DX11 predicates affect CopyResource, so internal copies run
with predication temporarily disabled under the context gate and restore it
before native Present/application calls. The predicate query is never polled or
modified by production code. The native clean-ring test alternates both predicate
values, proves that every private copy executes, and checks the same predicate
identity/value afterward. HUD pixels remain skipped when a predicate is bound.
See [SetPredication](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication).

GF2 now shows the HUD on both the start screen and the main scene. A fresh
Ctrl+Shift+C start-screen capture executes 22 application Draws in both players;
both RGBA outputs, final captured backbuffer and preview are exactly equal, and
the inspected replay image has no HUD. Main-scene capture shows a failure and
reports unsupported MSAA without publishing a second file. Detailed/Hidden/Compact
cycling works; capture while Hidden still requests a frame (the equipment scene
instead refuses its pre-existing Map/Unmap boundary gap). Disconnect removes the
HUD and all input hooks while the game continues, followed by normal game exit.
Evidence: `artifacts/capture-hud-final-gf2`, plus the earlier sRGB diagnostic run
`artifacts/capture-hud-storage-gf2-srgb`. These are HUD checks, **not** formal GF2
six-frame acceptance. The last two local refinements shorten failure messages and
protect private copies from active predicates; the GF2 screenshots predate those
refinements, which have their own focused native tests.

Implementation commit: `179b359` (`Render an isolated DX11 capture HUD`). Complete
capture-only/full builds pass. Capture-only regression is **61/61** before the
final predicate/message refinements, with **7/7** affected tests rerun afterward;
the final full build passes **64/64** selected CTests, including **12 Qt cases**.
All GPU work is serial. Replay and shared format are unchanged.

The resulting standalone package also runs with only Windows system directories
in PATH. Its exclusive-fullscreen session produces three additional files and
passes **82** exact dual-player/preview/inventory/Draw-zero checks, with clean
application bytes at exit and no Qt, Replay/Core, GPA, RenderDoc or Python modules
in the target. The native oracle executable was added only for this packaging
test. Package evidence is `artifacts/capture-hud-isolated-package-check`; the
working-build package manifest is hashed in `capture-hud-evidence.json`. A fresh
Git-archive rebuild was not repeated. Broader outstanding gates above remain
unaccepted; this evidence does not turn migration parity or formal GF2 capture
acceptance into a completed milestone.

## Raw keyboard input acceptance

The independent raw-input fixture registers its own keyboard with
`RIDEV_NOLEGACY | RIDEV_INPUTSINK` and checks the registration every frame. Native
UI input produces no legacy key messages: Compact/Detailed/Hidden each capture
one file, with identical raw-event counts in the observer and application. The
three files pass **82** exact dual-player/preview/inventory/Draw-zero checks.
After **15,064** frames the application's retained bytes still match exactly.

A separate non-GPU foreground sink receives capture and mode chords while the
target continues receiving background raw events; neither mode nor file count
changes. Returning to the target restores mode switching. Disconnect removes
all observers: another capture chord adds six raw events to the application but
none to the runtime and publishes no fourth file. The keyboard registration
remains unchanged throughout. A mixed raw-plus-legacy fixture verifies one mode
transition and one file for one chord each; both input channels reach the app,
only legacy triggers, and its file passes **28** dual-player checks.

The native test initially exposed zero-make-code Shift from injected keyboard
input; mode switching worked but capture did not. The bounded fixture event log
located the issue, and the generic left-Shift fallback fixed it. Original failed
runs remain under `artifacts/capture-hud-raw-live{,2}`. Successful runs are
`artifacts/capture-hud-raw-verified` and `artifacts/capture-hud-dual-input`.
Later focus resynchronization has additional held-chord/release tests; the native
raw acceptance run predates that refinement. The model tests cover 1,000 repeated
make events, independent left/right modifiers, foreground changes, extra Windows
modifiers, invalid keys and focus resynchronization. Physical long-hold testing
and buffered-only Raw Input are still open.

The final input build passes all **61 capture-only CTests** and **7 focused full
application CTests**, including **12 Qt cases**. A fresh GF2 start-screen hotkey
capture executes 22 application draws in both players; the two RGBA outputs,
captured resource and embedded preview agree exactly at 2560×1440. Flora replay
does not substitute reference pixels. The main scene remains visibly hooked:
the first request reports `Capture failed: MSAA`; a later request reaches the
existing unmatched Map/Unmap refusal. Neither publishes a file. Disconnect
removes the HUD and all input observers while the game continues normally.
These results live under `artifacts/capture-hud-raw-gf2` and do not count toward
formal six-frame capture acceptance.

References: [Raw Input Overview](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input),
[RAWINPUTDEVICE](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawinputdevice),
[GetRawInputData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdata).

## Window DPI and render resolution

HUD geometry now uses the window's client/DPI coordinate space, then maps the
panel into the native presentation surface. A 400×300 backbuffer stretched into
an 800×600 client therefore has the same displayed panel footprint as an 800×600
backbuffer. Nonuniform stretch maps each axis separately. Unequal client/surface
sizes with a known non-stretch DXGI mode skip before any pixel change. The
compact panel's resolution row ends above its footer. Short and narrow clients
shrink the complete panel proportionally; unreadable sizes skip explicitly.

The runtime temporarily uses the window's DPI awareness context solely for its
geometry queries and restores the calling thread's context before drawing.
It does not alter process DPI settings. This follows the window-specific model
documented by [GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow)
and [GetWindowDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowdpiawarenesscontext).

`capture_hud_layout` measures actual changed GPU pixels across **28 layout cases**:
native DPI-unaware (96 DPI) and per-monitor-aware (144 DPI) windows, opposite
caller-thread awareness, compact/detailed, 800×600 / 400×300 / 1600×900 surfaces,
narrow/short/unreadable clients, and return to the original size after Resize.
Every case preserves caller DPI, LastError and alpha; whole-resource bytes match
exactly after restore and the debug layer reports no warning/error. Measured
displayed margins and panel dimensions stay constant across render resolutions.
Two additional native flip cases verify that `DXGI_SCALING_NONE` with unequal
client/surface dimensions refuses before drawing and leaves bytes unchanged.
The pure geometry checks additionally cover 96/120/144/192/288 DPI. Generated
visuals and native results are ignored under `artifacts/capture-hud-layout-final-native`.
This is not evidence of physically moving a window between differently scaled
monitors or changing the desktop's DPI during capture.

Complete capture-only regression passes **62/62**; focused full application
regression passes **8/8**, including **12 Qt cases**. The two final non-stretch
test cases also pass in both builds. Final injected blt, three-buffer flip,
monitor-sized borderless and exclusive-fullscreen sessions publish twelve files
and pass **328** exact dual-player/preview/inventory/Draw-zero checks. All four
applications exit with exact retained scene bytes. Windowed sessions additionally
exercise TEST, invalid Present, partial Present1, retained-reference Resize
failure and successful Resize. GPU checks remain serial.

A fresh GF2 run verifies all three modes and start-screen hotkey capture, with
22 application draws and exact dual-player/captured-resource/preview bytes at
2560×1440. The main scene visibly reports `Capture failed: MSAA`. Hidden mode
still starts a request; disconnect during that request reaches `Cancelled`,
removes input observers and publishes no additional file. Game rendering remains
normal until a normal close. Evidence is under `artifacts/capture-hud-layout-gf2`.
This remains scoped HUD evidence; formal GF2 six-frame acceptance stays **0/6**.

## Native multithread lock ordering

A bounded native reproducer found a real deadlock in the former global context
gate: the application owns `ID3D11Multithread::Enter`, a second thread owns the
capture gate while waiting inside native `Flush`, and the owner cannot make its
next wrapped context call. The original failure exits with watchdog code 86;
`artifacts/capture-hud-concurrency-repro.log` is retained.

Context/presentation gates are now per native device. Wrapped `Enter` acquires
that device gate before entering the native critical section; matching nested
`Leave` releases it after native Leave. Independent devices do not hold one
process-wide gate across application synchronization barriers. The gate registry
retains mutexes, not GPU objects. CPU view creation, view resource getters and
swap-chain GetBuffer acquire native protection before alias registry access so
a thread waiting in the driver cannot block the lock owner's operand lookup.
Present itself does not add an extended native Enter scope around a possible
message-pump wait. Protection settings and application calls remain forwarded.

The new test compares unwrapped and wrapped WARP/hardware devices with the Debug
Layer. It covers nested owner progress, waiting Flush resumption, independent
device progress, single-thread use with protection disabled, and eight concurrent
view-creation/operand-copy rounds per run. The enabled-protection concurrency
cases follow Microsoft's [ID3D11Multithread contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nn-d3d11_4-id3d11multithread).
An exploratory disabled-protection concurrent test was invalid: the unwrapped
Debug build itself terminates that program. Its failure is retained separately;
it is not used as a valid concurrency requirement or counted as a pass.

This fixes the reproduced lock inversion. It does not establish general
multicontext capture support or finish the HUD concurrency/failure matrix.
Current desktop activation is unavailable: actual Present tests report
`DXGI_STATUS_OCCLUDED`, and the computer tool cannot activate the native input
fixture. Present, injected HUD and GF2 regressions for this revision remain
pending an interactive desktop. Earlier successful screenshots/captures retain
their earlier source revision and are not relabeled as this revision's evidence.

Both capture-only and full builds succeed. The selected capture-only regression
passes **62/62 of 66 configured tests**, excluding the four desktop-dependent
`capture_present_bindings`, `capture_hud_clean_buffers`, `capture_controller`
and `capture_present` tests. Those four remain pending, not waived. The full
build passes **9/9** focused tests. All four WARP/hardware native/wrapped
concurrency cases additionally pass ten serial repetitions each (**40 runs**),
with no Debug Layer warnings/errors in the final valid test. Logs use the
`artifacts/capture-hud-concurrency-verified-*` and
`artifacts/capture-hud-concurrency-repeat-tests.log` paths. No new Qt lifecycle,
packaged injection or GF2 result is claimed for this revision.

## Injected presentation and restoration failures

`capture_hud_faults` and `capture_hud_faults_hardware` exercise the production
wrapper with real WARP/hardware DX11 resources and the Debug Layer. A test-only
swap-chain facade inspects the decorated native surface, then returns controlled
`DXGI_STATUS_OCCLUDED`, `DXGI_ERROR_DEVICE_REMOVED`, `DXGI_ERROR_DEVICE_RESET`
and `DXGI_ERROR_WAS_STILL_DRAWING` results. A fifth case returns `S_OK` but
fails the restoration-stage GetBuffer. The facade does not call native Present;
these cases do not establish actual device removal or screen presentation.

All five cases leave the application's resource bytes exactly clean, retain the
application-facing HRESULT/LastError and publish neither a frame nor a partial
file. Restoration failure creates an explicit capability gap: the next HUD draw
is skipped and the shared CLI/hotkey capture entry returns
`capture_capability_incomplete`. Final cleanup releases wrappers/native metadata
and produces no Debug Layer warning/error. Both tests pass serially in both
capture-only and full builds against production revision `9b8ada3`; this change
adds tests only. Final logs are
`artifacts/capture-hud-fault-request-tests.log` and
`artifacts/capture-hud-fault-request-full-tests.log`, with hashed evidence in
`docs/capture-hud-evidence.json` under `fault_progress`.

The first test run prematurely checked native metadata lifetime while its own
raw device/context/swap-chain references were still held. That failed run is
retained in `artifacts/capture-hud-fault-run.log`; the test now releases those
oracle references before checking native lifetime. No production fix was needed.
Real device loss, current-revision interactive Present/GF2 regression and the
remaining HUD acceptance matrix stay pending. Formal GF2 acceptance stays 0/6.

## Device-origin multithread interface

After desktop interaction was restored, GF2 exposed a missing interface path in
the per-device gate implementation: its `ID3D11Multithread` comes from the native
device and cannot be queried back to `ID3D11DeviceContext`. The former context-only
lookup threw before forwarding `SetMultithreadProtected`; the recorded gap then
correctly suppressed HUD rendering and capture. The failed game session remains
under `artifacts/capture-hud-resumed-gf2` with no accepted file.

The lock lookup now resolves a context-origin interface through GetDevice, or a
device-origin interface through its native ID3D11Device identity. Both routes
use the same device gate. The concurrency oracle adds `--device` and four
WARP/hardware native/wrapped cases. Before the fix, the hardware native case
passed while the wrapper failed with `Native multithread protection unavailable`;
both logs are retained under `artifacts/capture-hud-device-lock-*`.

After repair, capture-only passes **72/72** tests; the full build passes **12/12**
focused tests, including **12 Qt cases**. Repeated injected blt, three-buffer flip,
borderless and exclusive sessions plus a standalone system-PATH exclusive package
produce **15 files / 410 exact checks**. Application retained bytes, dual-player
execution, previews, command/resource inventory and Draw-zero controls all pass.
The package audit finds no Qt/Core/Replay/GPA/RenderDoc/Python modules in the
target. These results use the fixed runtime hashed in `device_lock_progress`.

A fresh GF2 process now visibly renders Compact/Detailed/Hidden with zero
interface gaps; Hidden mode still completes hotkey capture. Six diagnostic files
execute in both players with exact resource/preview/output bytes and no reference
image substitution. After the user reported an approximately three-second hold,
four files followed the first tool-triggered capture; the user confirmed several separate
presses. Files are spaced 10–23 seconds apart with no repeat burst. These counts
cannot isolate the response to one physical hold, so they do not by themselves
pass the single-hold acceptance test. The last file comes from the explicit
Hidden-mode test. This is scoped manual input evidence.

The game is downloading a new resource update; these six files belong to its
update screen, not the required start/home scenes. New start-screen capture,
main-scene MSAA refusal and disconnect checks remain pending. Formal GF2
acceptance is still **0/6**. The live updating session is retained so the download
can finish without another restart. The following continuation supersedes this
temporary update blocker.

## Updated GF2 start and home scene checks

After the update finished, the same injected session (`b3b60d11-18b2-4c69-9775-3e45a565c50c`,
PID 2020) reached the start screen and home scene on production revision
`ff94b30`. The visible game version was `4.0.5136.14794.28495.3242`. Compact and
Detailed HUD modes render there; the start-screen hotkey publishes
`frame-9e68818b-c434-4a5e-b649-6135734b0a41.gpa_frame` at Present 337886.

That 2560x1440 file executes in FloraGPA and the original GPA player with exact
agreement between both outputs, the captured backbuffer and the preview. Flora
reports no reference-image substitution. The inventory contains 108 resources
and 18 application Draws (one Draw and 17 DrawIndexed). A negative copy retains
the preview/reference bytes but sets all 18 draw counts to zero: both players
still agree with each other, and their output differs from the positive frame.
Vertex shader 25 and pixel shader 26 export as DXBC and assembly. Pipeline event
371 exposes 1302 known fields and four unknown fields; this is not complete
pipeline-format coverage. The initial export script used the wrong pixel-shader
type number, then read the inventory and corrected it without modifying the
capture or production code.

The home-scene request visibly reports `Capture failed: MSAA`; its full error is
`ID3D11DeviceContext::ClearRenderTargetView: MSAA sample-preserving capture is not implemented`.
It publishes no file, exits busy state and continues presenting. Its message
subsequently expires. Hidden mode removes the HUD but still accepts a new hotkey
request; on the equipment screen that request explicitly refuses an Unmap without
a recorded successful Map. This separate capture limitation also publishes no
file. The shortcut remains visible to the game, which can open its own C-key UI.

After restoring Compact and disconnecting, HUD visibility is false and input
observer threads are zero. A further hotkey leaves the last request unchanged and
publishes no file, while successful Presents reach 348924. The session has seven
files total: six earlier update-screen diagnostics and this start-screen file.
The game then exits through normal Alt+F4; process absence was checked. Screenshots,
status snapshots, exports and serial GPU validation live in
`artifacts/capture-hud-device-lock-gf2-completion`; hashes and outcomes are recorded
under `gf2_completion_progress` in `docs/capture-hud-evidence.json`.

The desktop/update blocker is resolved. These scoped GF2 HUD checks pass; the
remaining HUD stress/failure gates are not promoted, and formal two-launch,
six-frame GF2 acceptance remains **0/6**. MSAA capture remains unsupported.

## ResizeBuffers1 rejection and recovery

Microsoft specifies [ResizeBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1)
for swap chains created with a D3D12 command queue. A successful D3D12 resize is
outside this DX11 capture scope. `capture_hud_resize` and its hardware variant
instead compare the native DX11 rejection with production wrappers on blt and
flip chains, both with held backbuffer references and after releasing them.

All four native/wrapped pairs return the same `DXGI_ERROR_INVALID_CALL` and
LastError, with matching D3D11 Debug Layer warning/error IDs. The tests verify
exact retained pixels, public resource identity, reattachment to private clean
storage, subsequent successful ordinary ResizeBuffers to 96x96, exact new-buffer
pixels, no capability gaps and released object lifetimes. They do not perform
Present or prove D3D12 support. Capture-only focused tests pass **5/5** and the
full-build resize cases pass **2/2**, serially. This adds tests only; the runtime,
replay and format are unchanged from the GF2-validated production revision.
