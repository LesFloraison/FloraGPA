# Native x64 DX11 Auto-Detect

Auto-Detect is implemented, but the three-game acceptance gate is **incomplete**.
See [the acceptance record](auto-detect-baseline.json) for separate attachment,
capture and executed-replay results. Existing GF2 fidelity limitations remain in
`migration.json`; this feature does not replace their evidence.

The subsequent [POOLS / HELLDIVERS 2 compatibility work](AUTO_DETECT_GAME_COMPATIBILITY.md)
adds timing-query recording and GPU completion replay, DISCARD HUD support,
serialized worker calls, GenerateMips and D24S8 MSAA initialization. The results
below describe the original acceptance build; OCTOPATH II needs a new full-game
run before its historical refusal can be marked resolved.

On this host, two Steam-launched MiSide sessions and two GF2 launcher sessions
complete attachment, HUD, capture and executed replay. Both GF2 title images and
the second MiSide image are byte-exact; the first MiSide image differs in two
pixels by at most one channel value. Two OCTOPATH II Steam sessions attach and
show HUD but refuse capture at TIMESTAMP End. These title/menu checks do not
establish broader scene fidelity. The earlier GF2 findings remain unresolved.

The final Release build, 20 selected serial CTests, the system-PATH capture
package probe, and four packaged golden/Draw-suppression checks pass. Both AppInit
registry views match their original snapshots; detection is off and no recovery
journal remains. Portable output is `artifacts/FloraGPA-AutoDetect-20261016` and
the independent capture package is `artifacts/FloraCapture-AutoDetect-20261016`.

## Operation

Open **File > Capture Application…**, leave Application empty, and enable
**Auto-Detect**. Wait for **Watching**, then start a new game from its usual
launcher. Steam and the independent launcher may already be running. Select a
detected session to use Capture Next Frame, Cancel, Disconnect or Open Capture.
Each session has its own UUID, PID, process creation time and output directory.

For a game that does not enter through AppInit, select its actual x64 game EXE
in **Application** before enabling Auto-Detect. This adds early active attachment
for that exact executable; the ordinary launcher still starts the game. The
monitor scans every 50 ms and only considers matching processes created after
activation, in the same user and desktop session. An already running target
reports **Restart required**. An empty Application retains global detection.

Target progress distinguishes **Detected**, **Attaching**, **Waiting for graphics**
and **Attach failed**. Expanded task logs contain the PID, creation time, attempt,
phase, elapsed time and Win32 error. Loading the shim is not capture success:
session publication requires an authenticated runtime handshake, and **Tracking**
requires a real device and recent successful presents. Auxiliary sessions remain
selectable, but do not replace the selected game's automatic session selection.

The native active loader makes at most 20 attempts, separated by 250 ms. It never
reissues an outstanding remote load. A five-second acknowledgement timeout is
reported without terminating the game/thread or freeing a still-used remote
parameter. Late acknowledgements remain observable. Installed hooks without a
graphics session produce a diagnostic after 30 seconds. No existing device is
retrofitted, and target restrictions are not disabled.

The setting is off at every application start. Hiding the Capture dialog keeps
detection active. Disabling detection stops new attachments but retains existing
session control. Application exit stops detection and disconnects sessions;
games are neither terminated nor forced to unload their runtime. The supported
scope is new interactive x64 DX11 processes for the current user and desktop
session. This is not late attachment, x86, DX12 or Vulkan support.

Administrator permission is requested for the temporary 64-bit AppInit entry.
Secure Boot and signed-AppInit-only policy refuse activation with an error; no
policy is changed. Microsoft documents the [Secure Boot platform limitation](https://learn.microsoft.com/en-us/windows/win32/dlls/secure-boot-and-appinit-dlls).
Existing 32-bit configuration, including this host's RenderDuck entry, is untouched.

## Components and ownership

* `FloraCaptureShim.dll` is a small native AppInit loader, without Qt, Python,
  JSON, or static D3D imports. Startup checks the read-only detection lease and
  installs entry detours. Full runtime loading, files and session IPC happen at
  the first graphics entry. Both static imports and LoadLibrary/LoadLibraryEx
  resolution are covered. A DXGI-first call completes D3D11 trampolines after
  loading the runtime's imports and before dispatching the call.
* The shim owns the five D3D11/DXGI entry patches. The versioned
  `FloraCaptureInitialize` ABI shares their native trampolines with the capture
  runtime, which returns wrapper entries. Explicit suspended launch retains its
  own bootstrap; actual loaded-runtime ownership and a Detours payload, rather
  than inherited environment variables, distinguish it from an automatic session.
* `FloraCapture monitor` is a persistent schema-1 NDJSON controller. Its private
  authenticated pipe talks to an elevated `monitor-worker`. `ready`, `heartbeat`,
  `session`, `attachment_progress`, `attachment_failed`, `error` and `stopped` events drive an asynchronous
  Qt client. Standard input `stop` ends detection. Existing one-shot commands use
  the detected version-4 session manifest unchanged.
  Optional `--target-exe <absolute-path>` selects active attachment. The internal
  lease/initialization ABI is version 2; capture files and control protocol remain
  unchanged. The versioned progress mapping is scoped to the activation epoch.
* The worker copies loader, runtime, controller and CRT dependencies into a unique
  administrator-protected Program Files directory. A creation-time-bound
  `monitor-guard` acknowledges readiness before registration. A durable protected
  recovery journal preserves original registry types and bytes. Restoration
  removes only our token after third-party changes and does not restore another
  process's changed LoadAppInit setting. Normal exit, owner death and worker death
  have independent cleanup paths. `monitor-recover`, also invoked at Qt startup
  when a journal exists, repairs a residual journal without disarming a live worker.

The lease validates owner and broker handles, creation times, user SID, session
and activation epoch. An inactive or stale lease never starts recording. Windows,
analysis tools, common browsers, launcher and crash helpers are excluded; game
names are not an allowlist. Protected version directories remain on disk because
already attached games may still reference them.

The loader follows the [Microsoft initialization constraints](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices):
no runtime LoadLibrary, filesystem session creation or COM initialization in
its DllMain. Active loads dispatch installation to a worker without waiting under
the loader lock. AppInit retains its startup entry installation. Runtime/active
patch transactions enlist peer threads; transactions are skipped when no new
graphics entry needs patching. Module lookup precedes the private patch lock. Arbitrary
third-party DLLs calling graphics from their own DllMain and arbitrary concurrent
native entry patching are not established by the current test matrix.

## Compatibility changes discovered by acceptance

OCTOPATH TRAVELER II uses a ten-bit swap chain. Capture presentation storage,
HUD blending and preview/replay export now support R10G10B10A2_UNORM while keeping
the raw captured ten-bit resource. Explicit view descriptors with Format UNKNOWN
are resolved on the original backbuffer before use with private typeless storage.
Native R8/BGRA/R10 view tests and the R10 capture/replay CPU oracle cover this fix.
That acceptance build refused OCTOPATH's TIMESTAMP query recording explicitly;
the later timing support does not by itself establish OCTOPATH game acceptance.

The already running GF2 launcher inherited an earlier explicit capture session's
environment. Using that variable as evidence of hook ownership incorrectly
skipped its new child. The loader now checks the loaded runtime; the runtime's
explicit bootstrap requires a real Detours payload. Launcher restart is unnecessary.

## Original implementation research

Read-only Ghidra work is isolated under `artifacts/auto-detect-research`; original
components are never application dependencies. SHA-256:

| Binary | SHA-256 |
|---|---|
| GpaServer.exe | `08B9606D881B9F7A2B51F89656D11B48E640413328ED629036F90BC195A19D4D` |
| shimloader64.dll | `AF68E28BA54AB0068C95F82081828788D6F59879283127DC330B818AF7369888` |

GpaServer RVA `0x249e40` is an error path referencing
`GPA::Monitor::InjectorGlobal::Enable` and failed global shim initialization.
Shim D3D11CreateDevice/AndSwapChain hooks (`0x12b90`, `0x12bf0`) use cached resolver
`0x5420`. It invokes `0xeee0` when the dispatch slot is empty; that routine examines
MonitorInjectionSettings under a critical section and selects a runtime factory
entry or native fallback. `0x82e0` lowercases module basenames and matches graphics
module tables. AppInit strings at `0x7f5a8`/`0x7f5c8` have data references. These
observations support global early loading and lazy API dispatch; they do not prove
complete equivalence of original filtering, registry restoration or lifecycle.

## Reproduction

Build `cmake --preset capture-full`, then
`cmake --build build/vs2022 --config Release`. Relevant CTest names include
`capture_autodetect_policy`, `capture_bootstrap`, `capture_ui`,
`capture_presentation_resources*`, `capture_hud_pixels` and
`capture_hud_clean_buffers`. GPU tests must run serially.

`tools/validate_auto_detect.py` is an opt-in development harness that temporarily
registers the loader and launches a native probe externally. It is never packaged
or used as an application backend. Select `--target FloraAutoDetectDelayedProbe.exe
--mode 45` for DXGI-first delay loading, `--stale-environment` for inherited launcher
variables, and `--stop-kind owner-crash` or `broker-crash` for cleanup. Every run
records both registry views before and after and checks continued session control
after detection ends. Generated captures, research and evidence stay ignored.
`--stop-kind residual` also kills the guard, proves the residual shim does not
load a runtime without a valid lease, and exercises recovery. `--concurrent`
checks separate sessions and captures; `--explicit-peer` checks entry ownership
while automatic detection and explicit launch coexist.
Use `--active` to target the copied fixture, `--no-appinit` to disable AppInit in
that owned test process, and delayed probe mode `46` to leave an early attachment
window before DXGI initialization. Active tests assert broker confirmation of
real D3D11 frame updates before capture.
