# Independent DX11 capture

Implementation is **partial**. The current native controller can launch and
instrument x64 DX11 applications, but frame requests return
`capture_capability_incomplete`. No live `.gpa_frame` writer is connected to the
runtime. This is not the planned capture release and does not pass the GF2
six-frame gate.

## Architectural contract

`FloraGpaFormat` is a C++20 wire contract. `FloraGpaReader` owns checked, immutable
mapped container storage. `FloraGpaWriter` writes container transactions to a
same-directory temporary file and publishes without replacing existing output.
Neither library links Qt, D3D, capture, replay or application analysis.

`FloraCore::Frame` retains resource interpretation, context recovery, hashing
and edited-view overlays; it shares the immutable reader between overlays.
Replay links the reader, not the writer. Existing RenderDoc recapture remains
an optional replay-analysis feature.

The native capture controller/runtime and future encoder must not include or
link FloraCore, FloraReplay, FloraExperiment or Qt. They use their own recording
model; only pure wire layouts may be shared. A configure-time transitive target
check and the `capture_boundaries` CTest enforce these constraints. Runtime
module and standalone-package audits remain separate evidence.

The Qt capture client belongs to the application composition layer. It uses
asynchronous CLI calls and shares only a completed file path with replay. It
never handles target COM pointers or writes GPA records.

The full profile exposes **File > Capture Application…**. The dialog supplies
application/arguments/working directory/output, swap-chain observations, launch
and disconnect controls, and a collapsed task log. Capture and open-result actions
remain disabled while frame recording is unavailable. The session persists when
the modeless dialog is hidden; disconnect is explicit and does not kill the game.
The controller path is the app-local `FloraCapture.exe`.

The Qt client suite passes malformed/oversized/conflicting CLI responses,
concurrent request rejection, target exit and a real native-controller lifecycle.
The full-profile run has no skips; standalone UI-only tests intentionally omit
the native-controller case when capture binaries are not configured. The full
GUI and Worker compile, and the existing view-overlay tests pass. None of this
constitutes acceptance of the unimplemented frame pipeline.

## Evidence and remaining gates

The C++20 native bootstrap uses pinned Detours 4.0.1 and the existing repository
nlohmann/json 3.12.0. The upstream Detours target alone needs MSVC permissive mode;
Flora capture sources use strict conformance. There are no runtime Python or GPA
dependencies. Controller sessions validate PID and process creation time, and
their local named pipe admits only the current user. Disconnect leaves transparent
hooks resident until normal target exit.

CreateDXGIFactory/1/2 now wrap Factory through Factory7 and interpose legacy,
HWND and composition swap-chain creation. Swap-chain GetParent preserves the
factory wrapper identity. CoreWindow creation forwards through the same wrapper
but has not been exercised by a real CoreWindow oracle. Adapter, Device, Context
and resource interfaces remain incomplete; unknown interface escape never enables
frame capture. The runtime reports swap-chain API/IID escape diagnostics.

Validation with the final native build: 19 swap-chain comparisons, 24 factory
comparisons and the `capture_bootstrap` CTest pass. These include native/injected
pixels, COM identities, reference observations, concurrent QI, TEST Present,
failed/successful resize and explicit frame-request refusal. Local results are
`artifacts/capture-bootstrap/{swap-chains-final,factories-final}/`.

An earlier build launched the specified GF2 executable and observed two successful
device creations and a 2560x1440 swap chain with 43,564 successful Present calls,
without hook/registry errors. GetBuffer exposed the known missing resource wrapper.
Capture was explicitly rejected, the session disconnected and the game closed
normally. This is an injection diagnostic, **zero accepted captured frames**.
Its local report is `artifacts/capture-bootstrap/gf2-diagnostic-1/status.json`;
the later controller session-identity and JSON dependency changes were validated
with self-owned probes, not another GF2 run.

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

The capture command currently rejects explicitly; idle cancel is not evidence of
working cancellation during capture. Exit codes are 0 for success, 3 for a rejected
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
