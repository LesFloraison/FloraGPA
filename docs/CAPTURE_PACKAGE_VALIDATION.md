# Independent capture and replay package validation

The capture-only and replay-only package gates pass on this host for source
commit `3a00e4af04cfe022c0fc4865f55edc77f31376f4`. **GF2 formal acceptance remains
0/6**: this does not resolve the real-game pixel variability or complete its
original GUI inspection. No production replay or format behavior changed.

## Packaging changes

The capture packager previously hardcoded private protocol version 3 while the
controller and runtime used version 4. The controller now exposes `--version`
as a JSON protocol/architecture identity. Packaging queries that executable,
validates the response and records its actual version. The source-build gate
also checks the packaged CLI identity against the injected runtime response.

The application packager previously read only `build/vs2022`. Its new
`-BuildDirectory` argument selects both the binaries and their CMake cache,
allowing the `replay-only` preset's `build/replay` output to be packaged without
collecting capture components from the full application's directory. The
committed-source validator accepts `--profile replay-only` and checks that the
generated projects and package exclude capture targets/components.

## Clean source and runtime results

Both builds used a fresh Git archive in a directory containing spaces, VS2022
x64 and C++20. Child environments retained Windows system paths; no workspace
build directory, Qt bin directory, GPA, RenderDoc or Python directory was on
their PATH. Replay configure received its Qt installation explicitly.

| Gate | Observed result |
|---|---|
| Capture configure | No Qt discovery and no Replay project |
| Capture build/tests | Build passed; all 114 CTests passed, GPU checks serial |
| Capture package | CLI, Runtime, three VC143 CRT DLLs, manifest, pinned dependency metadata and licenses |
| Packaged capture control | Bootstrap refusal/cancel/disconnect control passed; 24 live launches generated 96 frames, two MSAA launches generated eight frames |
| Capture version agreement | Package manifest, CLI and injected Runtime all report protocol 4 |
| Replay configure/build | `replay-only` build passed; no capture projects; Core, Replay, CLI and UI project references contain neither Capture nor Format Writer |
| Replay package | 62 files, including app-local Qt and the optional pinned RenderDoc backend; no Capture binaries |
| Import audit | Two Capture and five Replay application binaries checked; no forbidden imports |
| Packaged replay golden gate | Historical GF2/BF1 positive images match their established exact hashes; both Draw-suppression negatives differ as expected |
| Newly packaged MSAA captures | Eight captures plus four initialization/application-Draw removal controls: 12 files, 279 actual dual-player checks passed |
| Replay module audit | All 16 replay output reports use app-local Qt; no Capture, GPA player, RenderDoc or Python module loaded in those replay processes |

The live capture checks include busy requests, armed cancellation, repeated
captures, explicit resource budgets, unsupported-update failure/recovery,
disconnect with continued target execution, process exit, and partial-file
cleanup. They are deterministic self-owned programs, not the six GF2 acceptance
files. The MSAA gate independently computes expected pixels, verifies actual
execution in both players, and inspects recorded events, resources and DXBC.
The separate development oracle loads original GPA only in its own process.

Import inspection, build dependencies and runtime inventories support the
module boundaries together. Successful packaging alone is not evidence of
correct GPU execution. These are fresh-source and restricted-environment tests
on this machine, not certification on a separate clean Windows installation;
the installed compiler, SDK and graphics drivers remain available. Python
orchestrates development checks and is not shipped or loaded by either app.

## Failures and recovery retained

The first replay package attempt selected an older local runtime directory
that contained the correct `renderdoc.dll` but lacked `renderdoccmd.exe`.
Packaging rejected it before publication. After both runtime files matched the
pinned manifest in the preserved official 1.45 distribution, packaging was
retried using the **same unchanged source build**, and succeeded. The initial
source-validator report remains `completed: false`; the separate successful
`package-retry.json` records the recovery, inputs and package hashes.

An initial wrong-build refusal check encountered that same missing runtime
first, so it did not establish rejection of the build profile. Its log is
retained as inconclusive. The repeated check with valid runtime inputs verifies
the explicit replay-enabled-build error. Existing capture package output and
extra `--version` arguments are also rejected; existing evidence is preserved.

## Reproduction

```powershell
python tools/validate_capture_source_build.py --out <new-capture-evidence-dir>
python tools/validate_source_build.py --profile replay-only `
  --qt-root D:/Qt/6.11.2/msvc2022_64 `
  --renderdoc-root <verified-official-RenderDoc-1.45-directory> `
  --out <new-replay-evidence-dir>
```

Then run `tools/validate_native.py --isolated-env` against the packaged CLI and
the preserved golden files. For the generated files, use
`tools/validate_capture_msaa_injected.py --sessions` with the capture gate's
`packaged-msaa/results.json`, the packaged controller and the packaged replay
CLI; set `--qt-bin` to the replay package itself. All GPU runs must be serial.

The [baseline](capture-package-baseline.json) pins source archives, package
inventories, test results, module audits and the failed/retried packaging
evidence. Raw outputs are ignored under
`artifacts/capture-package-audit-20261014`. Old workspaces, captures and package
evidence are unchanged. Broader API coverage and the full GF2 gate remain open.
