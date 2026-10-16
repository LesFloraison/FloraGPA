# Endfield active attachment, 2026-10-16

Status: **this DX11 capture improvement is accepted by the user**. Endfield
attachment, launcher stability, physical HUD switching and capture publication
were verified locally. On 2026-10-16 the user also confirmed that original Intel
GPA reads the latest captured single-frame file normally. This is a user-reported
compatibility result, not a new automated replay-fidelity measurement. The
original three-consecutive-cold-launch gate remains unverified (0/3 formally
counted); earlier observations below are retained as historical evidence.

## Implementation

The existing EXE field optionally supplies `monitor --target-exe`. The elevated
worker scans every 50 ms, matches the normalized full x64 image path and checks
user, desktop session and process creation time. Existing instances require a
restart. PID and creation time identify each candidate. It loads the protected
staged shim through a remote `LoadLibraryW`, with bounded pre-dispatch retries
(20, 250 ms), a five-second acknowledgement deadline, no duplicate outstanding
load and no termination of the game or remote thread. Parameters still in use
are retained on timeout/stop. The scanner runs separately from control/heartbeat.

AppInit merging and recovery remain enabled. Both entries use the shim's hook
installation, with peer-thread Detours coordination for active installation and
no transaction when there are no missing entry points. The internal lease and
initialization ABI is v2; capture wire format/control protocol are unchanged.
The per-activation progress mapping permits the same user's medium-integrity
game to acknowledge the elevated broker. Runtime session publication requires
an authenticated status handshake. Subsequent target status checks distinguish
real D3D11 Presents from DLL/runtime loading, report a 30-second graphics timeout,
and keep accepting late frames. UI selection prefers the exact target session.

## Local game observations

Target: `<GAME_DIR>/Endfield.exe`.

* First attempt: the user reported a slow, crashing Hypergryph launcher before
  the game started. Evidence is in `artifacts/endfield-active-20261016-01`.
  The global path had entered launcher helpers. A temporary directory exclusion
  was used at that point; it has now been removed. Redundant patch transactions
  are still skipped. See the generic video-interface fix below.
* Second attempt: PID 82976, session
  `27a03409-5e63-4cd2-8e70-b47e31e085cc`. The user saw the HUD briefly, then it
  disappeared. `artifacts/endfield-active-20261016-02/hud-disappeared.json`
  records 3,783 successful Presents; `final-live-status.json` records 15,085,
  approximately 60 FPS, two device creations, no creation failures and hook
  status zero. The attachment continued after HUD disappearance.
* The runtime reports `QueryInterface`, IID
  `10ec4d5b-975a-4689-b9e4-d0aac30fe333`, reason
  `Interface forwarding is not implemented`. Windows SDK `d3d11.h` identifies
  this as `ID3D11VideoDevice`. `hud_control.cpp` deliberately refuses HUD work
  after any interface gap (`HUD skips untracked interfaces`). Existing status
  does not establish whether the video device is the main presentation device.
* A capture request without swap selection first returned
  `swap_chain_selection_required`. Explicit swap 1 returned
  `capture_capability_incomplete`. No Endfield frame/replay success is claimed.
  These video-interface/capture limitations are separate from active attachment;
  this change does not clear the guard or pretend the interface is tracked.
* The development helper marked one short initial HUD interval as a pass.
  Its `rounds.json` and `finished.json` are raw evidence, **not stable game
  acceptance**. The later user report and continuing status supersede that
  preliminary result. Further cold starts were not counted.
* Monitoring was stopped normally; `finished.json` confirms registry restoration.
  Existing captures and staged DLLs used by the still-running game are preserved.

No evidence here establishes an ACE block. The observed active attachment and
continued real Presents contradict a blanket capture-hook failure diagnosis.

### User restart follow-up, 14:00 CST

Read-only inspection of `<USERPROFILE>/Documents/FloraGPA/Captures` found
seven additional Endfield main-process manifests written at 12:20:18, 12:24:43,
13:40:14, 13:40:59, 13:42:13, 13:51:48 and 13:58:33 (CST). They have distinct
PIDs/creation identities; PlatformProcess helper sessions are not counted. The
user reports reproducible brief HUD visibility across repeated launches.

The 13:42 and 13:58 sessions also contain distinct 52,084,924-byte frame files.
Both complete offline checked-reader preflight with zero errors, 14 warnings,
72 entries and `review_required`; neither establishes replay fidelity. This
adds successful file production to the earlier evidence, which had only an
explicit capture refusal. Capture paths, hashes, manifest identities and full
preflight reports are preserved under `artifacts/endfield-user-restarts-20261016`.

No Endfield process was running during inspection. These directories contain
manifests/frames, not historical HUD/status samples. The current Qt task log is
memory-only and omits successful periodic status responses. Therefore these new
files do not independently establish the reason for every HUD disappearance,
nor the duration of visibility. The earlier ID3D11VideoDevice diagnosis remains
confirmed for the observed PID 82976 only; stable three-launch acceptance remains
incomplete.

## Validation and remaining scope

VS2022 x64 Release controller, shim, runtime, Qt app and UI tests build. Earlier
focused CTest passes covered Auto-Detect policy, HUD options, controller,
bootstrap and 17 Qt test cases. Native active-only capture with AppInit disabled
passed (`auto-detect-active-only-20261016-03`); AppInit/active concurrency plus an
explicit peer passed (`auto-detect-active-race-20261016-03`). Owner-crash and
broker-crash recovery passed (`auto-detect-active-owner-20261016-01`,
`auto-detect-active-broker-20261016-02`); broker recovery took 13.859 seconds.
All those paths are under ignored `artifacts/`.

Final controller follow-up: `auto-detect-active-only-20261016-04` passed with
AppInit disabled, an asserted `graphics_confirmed` event, a completed frame and
restored registry. Policy and bootstrap CTests passed. Qt initially reported a
native-launch timeout (16/17); the isolated native case and the subsequent full
Qt CTest passed (17/17). The first failure remains recorded, with no established
root cause. Two additional HUD interface-guard CTests (WARP and hardware) failed
with JSON invalid UTF-8 byte `0xD2`; they are **not** reported as passed. Their
failure does not establish interface-guard correctness on this machine. Raw
initial/final logs and delivered binary hashes are in
`artifacts/auto-detect-final-checks-20261016`. Qt Core/Gui/Widgets and the Windows
platform plugin are deployed at version 6.11.2 alongside the Release application.

Real blocked-loader resource retention, permission denial, cancellation during
remote loading and retry-exhaustion integration stress remain unverified; policy
unit coverage alone does not establish those OS failure paths. Path/SID query
failure before the scanner can prove target identity still retries silently.
Full game compatibility remains incomplete. Video forwarding was subsequently
added as described below; video capture/replay is not implemented.

## Generic launcher crash investigation and fix

The launcher directory exception is removed from `auto_detect.hpp`; policy tests
assert that launcher executables in that directory remain eligible.

The 15:09 local reproduction produced `Games.exe.90672.dmp` (Windows CrashDumps).
Its exception is `0xc0000409`, subcode 7 (`FAST_FAIL_FATAL_APP_EXIT`), from
`ucrtbase!terminate/abort`. The failing thread is the system video decoder
`msmpeg2vdec`, entering D3D11's `CVideoDecoderOutputView` constructor and failing
while interpreting the texture format. It is not evidence of an ACE rejection.
Debugger output is retained in `artifacts/launcher-crash-dump-02`.

A small hardware D3D11 probe reproduces the boundary independently of any game:
create an NV12 decode texture, then call `CreateVideoDecoderOutputView` through
`ID3D11VideoDevice`. Native execution succeeds; the old injected runtime aborts
before returning the view (`artifacts/video-probe-before` and its Windows dump).
The device/texture were wrapped, but QI returned an unwrapped video interface.
Consequently a native D3D11 video method received a Flora resource proxy.

Video device/context and child interface families now use the same COM identity
registry and operand unwrapping as graphics. Nested decoder resource arrays,
processor stream surfaces and the documented HW-CENC session field are copied
and translated without modifying caller storage. Video context calls use the
device presentation gate and explicitly reject an active capture as unsupported;
this is forwarding compatibility, not a video encoder/replay implementation.
See the Microsoft contracts for
[processor streams](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_video_processor_stream)
and [decoder session structures](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ns-d3d11_1-d3d11_video_decoder_begin_frame_crypto_session).

The fixed injected probe succeeds (`artifacts/video-probe-after`). The new
`capture_video_forwarding` CTest compares native versus wrapped processor pixels,
decoder output creation, COM identity, nested resources, ignored disabled streams
and proxy lifetime. Both RelWithDebInfo and VS2022 x64 Release runs passed.
The direct-launch launcher probe reports two devices, 621 successful Presents
and zero interface gaps (`artifacts/launcher-video-fix-01/status.json`). This is
not yet Auto-Detect/Endfield acceptance. Screenshot automation returns a black
window for both native and instrumented launcher runs; visual confirmation is
pending. Launcher dirty/scroll Present1 capture and HUD remain unsupported.

Diagnostic Auto-Detect starts 02 through 07 timed out before readiness; an
attached worker stack was inside `RegSetValueExW/NtSetValueKey`. Recovery restored
the original registry values for each attempt. No security setting was disabled
and the third-party AppInit entries were preserved.

The production Release monitor subsequently started normally. Its first launcher
session (`artifacts/launcher-game-fixed-01`, PID 102120) passed 2,880 Presents
without the decoder crash. It exposed a further missing IID, `IDXGIKeyedMutex`
(`9d8e1289-d7b3-465f-8126-250e349af85d`), which is now forwarded. The hardware test
also verifies shared-resource identity and a key handoff in both directions
between two devices. Keyed synchronization during an active capture explicitly
rejects that capture; cross-device replay is not claimed.

Release policy, object registry, WARP/hardware context-state and video tests pass.
The broader run failed five Present publication tests (low-budget arming or
localized invalid UTF-8 diagnostics), and Qt passed 16/17 with a native capture
button timeout. Unmodified 2026-10-15 capture-only binaries reproduce both the
low-budget and flip invalid-UTF-8 failures; these are not waived or counted as
passes. Raw logs are in `artifacts/launcher-fix-validation`.

The new runtime passes active-only attachment with process AppInit disabled and
AppInit/active racing with an explicit peer. Both produce native fixture capture
files and restore the registry (`artifacts/launcher-fix-active-regression` and
`artifacts/launcher-fix-race-regression`). These fixtures do not establish
Endfield's three-launch acceptance. Release Qt Core/Gui/Widgets and qwindows
deployment are all version 6.11.2.

Final monitor `artifacts/launcher-game-fixed-02` was armed for the exact Endfield
path with the new video/keyed-mutex runtime. The normally launched Games.exe
PID 97048 reports two D3D11 devices, 566 successful Presents at the initial
sample and zero interface gaps. Its HUD is explicitly skipped for partial
Present1. The temporary
monitor disarms and restores its registry snapshot after 15 minutes or an
earlier stop signal. Visual launcher interaction and Endfield cold launches
still require user verification; UI automation cannot activate this launcher
and returns a black image even for the uninjected baseline. The isolated Qt
native dialog retry also timed out; the broader UI failure remains unresolved.

The user subsequently confirmed the launcher did not crash and manually started
Endfield (PID 4448, session `fb3074b3-6fc9-4603-aa93-37518e4d135c`). At 16:12:53,
the game had 39,918 Presents, approximately 60 FPS and zero interface gaps.
The user confirmed a persistent main-menu HUD, an initial in-scene disappearance,
and recovery in the actual scene after a further Ctrl+F1 at our request.
Two earlier mode edges at 16:05:56 and 16:06:00 switched compact to detailed to
off; the user explicitly says these were attempts to recover an already missing
HUD. They explain the later hidden state, not the initial disappearance.
Polling saw a brief stale HUD sample at 16:05:35, followed by drawn/visible
status at 16:05:37. These diagnostics do not prove on-screen pixel visibility;
the original transient disappearance remains unconfirmed. Do not count this as
three clean cold launches or as a complete HUD acceptance.

The main-menu capture at 16:01:38 is
`artifacts/launcher-game-fixed-02/sessions/fb3074b3-6fc9-4603-aa93-37518e4d135c/frame-b8cdb9a5-82fb-40c7-a156-65ebacb23146.gpa_frame`
(382,065,962 bytes). Offline checked-reader validation reports zero errors and
15 warnings, `review_required`; replay was not validated. Two later in-scene
captures failed at `ID3D11DeviceContext::DrawIndexedInstanced` with
`Resource capture byte limit exceeded` (1,024 MiB budget), and published no
files. Keep capture budget/compatibility separate from attachment health.

## Shared UI and hotkey resource budget (2026-10-16)

The next manual session (PID 102744) exposed a configuration gap: the UI showed
4 GiB, but both hotkey requests (`ac9b0491-9308-4dfc-9d5b-3af09dacc0f0` and
`1c14b0d2-95ba-4247-bce6-455f60e83647`) still used 1,024 MiB and failed during
`DrawIndexedInstanced` resource snapshots. The hotkey worker omitted the budget
argument and consequently used a compile-time default. Raw UI and runtime
evidence is in `artifacts/endfield-manual-1618`.

The runtime now owns a mutex-protected session budget. The UI sends an additive
`configure` command after session selection and budget changes, and waits for
the echoed `capture_limits.session_resource_limit_mib` before enabling capture.
Requests without an explicit override, including the existing hotkey worker,
inherit that value. Settings changes do not mutate an already armed capture.
No capture format or Auto-Detect shared-memory ABI changed. Older runtimes that
cannot acknowledge the setting are reported as needing a target restart.

VS2022 x64 Release builds pass. `capture_resource_limit` exercises native IPC
validation, budget inheritance, change while armed, and successful file
publication. `capture_ui` passes all 20 cases, including applying a budget
selected before launch, changing it while a status poll is busy, switching
sessions, configuration rejection, an older runtime, and native button capture.
Tests ran serially. Evidence is in `artifacts/resource-limit-fix-01`.

Fresh Endfield PID 98212 confirms the applied session budget of 4,096 MiB and
three physical hotkey requests accepted with that budget. All three completed:
`e4d4a045-4b15-480c-8d19-5b2b408523fd` (382,065,962 bytes),
`4abf6d39-3b78-4642-93ce-6c8018a37123` (1,919,740,440 bytes), and
`e1a4f62b-f678-49c9-8bb9-2ac0aa45b145` (644,549,021 bytes).
Files remain in the user's Documents capture session
`20261016-162948-577/auto-20261016-083013-845/bc915439-1503-4958-8f8c-289dabf55804`.
All pass checked-reader preflight with zero errors, 15 warnings and
`review_required`. This verifies capture publication, not full replay fidelity.

The actual game scene (character in water beside stone ruins) and visible Flora
HUD were observed directly. A subsequent Capture Next Frame button action with
swap chain 1 and 4 GiB produced
`03f676be-2f33-4ac0-8e57-846d1ef5239e` (2,172,755,848 bytes, Present 11,987).
The request reports 4,096 MiB and Completed, while hotkey attempts remain three,
distinguishing the button request from the previous physical hotkeys. Its
checked-reader result is also zero errors, 15 warnings, `review_required`.
This closes the shared-budget configuration fix and scene capture validation;
it does not close the separate three-cold-launch/HUD-transient or replay gates.

## Local keyboard observer registration (2026-10-16)

Endfield PID 88984 continued presenting with a visible Compact HUD, but reported
`Cannot observe target keyboard messages: 126`, zero input threads and zero
mode/capture hotkey edges. A button capture completed with the 4,096 MiB budget.
The staged runtime existed and matched the build used in the preceding successful
session. Evidence is in `artifacts/endfield-hotkey-1639`; this is a keyboard
observer registration failure, not evidence of a missing graphics hook.

The observer now passes a null module to `SetWindowsHookExW(WH_GETMESSAGE)` for
threads verified as belonging to the current process, as required by the
[Win32 contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw).
It no longer registers the runtime DLL as a hook module for this local callback.
Failure diagnostics include the target thread ID. The Capture dialog displays
`Hotkeys unavailable`, exposes the detailed error in its tooltip and records
error/recovery transitions without repeating the same error on every poll.
Button capture remains available when only keyboard observation fails.

VS2022 x64 Release compilation and Qt 6.11.2 deployment checks pass. Four serial
CTest suites pass: `capture_hud_raw_buffer`, `capture_hud_model`,
`capture_resource_limit` and `capture_ui`. The injected native fixture asserts
at least one installed keyboard observer and an empty input error; UI tests
cover warning display, repeated status polls, recovery and independent button
availability. Evidence is in `artifacts/keyboard-fix-01`.

Fresh Endfield PID 93852 loaded runtime SHA-256
`45D597623F5FC35F31C3996ECF4849BE577C5919EB704E950922441118C86F41`,
matching the new Release build. It reports one installed input observer and
no input error. Live samples show Compact -> Detailed -> Off -> Compact;
the user explicitly confirms normal HUD switching and a successful physical
hotkey capture. Diagnostics record one capture edge, one request attempt and
one accepted request. The request completed at Present 1,377 with a 4,096 MiB
budget and published `frame-554871be-9258-4050-9b59-1c8d87d70f78.gpa_frame`
(2,057,019,631 bytes) under the Documents capture session
`20261016-170539-304/auto-20261016-090605-626/3bb6c405-0618-4c6e-8377-ac101475370f`.
The checked reader reports zero errors, 15 warnings and `review_required`.
A subsequent direct window observation shows the Compact Flora HUD at 60 FPS
over the in-game menu. This closes this launch's keyboard registration and
hotkey capture verification, without claiming full replay fidelity, isolated
held-key behavior or three consecutive cold-launch acceptance.
