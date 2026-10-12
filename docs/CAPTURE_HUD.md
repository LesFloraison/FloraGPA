# Native capture HUD

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

Runtime drawing currently requires `D3D11_CREATE_DEVICE_SINGLETHREADED`, tracked
interfaces, no active query/predicate and no mapped resources. Other concurrency
models skip before drawing. Supported single-buffer blt presentation also works
in the scoped exclusive-fullscreen component test. The flip/multi-buffer gap
remains unresolved; GF2 therefore deliberately displays no HUD in this version.
This is a partial implementation, not the requested complete delivery.

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
Raw-input-only keyboard registrations are **not supported yet**. The initial
Present-only polling approach missed short synthetic presses; the actual native
message path was then verified with the same short key presses. Pure polling
edge tests are not evidence of native key-repeat/focus acceptance.

Status reports mode, per-swap actual visibility/skips, measured FPS, input profile,
selected swap, request outcome and expiring messages. Only successful non-TEST
Presents contribute to FPS. Saved filenames last four seconds; errors last eight.
Request identity distinguishes repeated identical failures. Full request/error
details remain in CLI status and the Qt task log.

## Integration evidence

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

Multi-buffer/flip pixel restoration, general concurrency isolation, raw-input-only
hotkeys, explicit borderless and DPI/resolution matrix, native long-hold/focus
stress, active-query/hidden-state failure matrix, actual driver Present failures,
and GF2 visible HUD acceptance. The successful single-buffer experiment cannot
be extrapolated to read-only rotated buffers. Formal GF2 acceptance remains 0/6.

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
