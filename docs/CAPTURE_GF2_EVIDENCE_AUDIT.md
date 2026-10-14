# GF2 retained-evidence audit

`tools/audit_capture_gf2_evidence.py` is a read-only development tool for the
retained `capture-gf2-current-20261014` experiment. It does not execute a player,
load GPA, change captures or introduce Python into any application package.
The tool exits nonzero on missing/malformed evidence or failed checks and refuses
to overwrite its output. Its narrow IGPA v3 final-resource extractor is not a
replacement for the production checked reader or a general format validator.

The audit hashes the requested runtime/controller/replay, compares them with the
recorded identities, verifies two distinct target sessions and three distinct
completed captures per session, and checks the selected swap chain continues
presenting. The observed target-module inventories must contain the session's
runtime and exclude Qt, Replay/Core, GPA, RenderDoc and Python. These are recorded
observations, not a new live package audit.

Each file's dedicated final RGBA8 resource is compared exactly with three saved
executions from each player. Original-player reference images must also match.
Execution reports must indicate actual DrawIndexed calls, the expected Intel
adapter and successful completion; Flora must explicitly deny using reference
pixels. Both Draw-deletion outputs must agree with each other and differ from
the final resource. The original player's pinned DLL hash is checked. No pixel
tolerance is permitted. Missing or multiple executed TGA files are errors.

Reports include hashes of input manifests, session/status records, player reports
and images; frame hashes are recorded separately. This makes the reviewed
evidence reproducible, but does not retroactively authenticate how older reports
were produced. Their producer scripts and original experiment remain necessary
context. Original reports lack a direct capture-path field; their association
uses the experiment directory layout and exact reference-image comparison.

## Results on 2026-10-14

Eight CPU-only test methods pass, covering truncated and malformed records,
missing references, duplicate IDs, unverified TGA layouts, row/channel ordering,
ambiguous images, exact differences and false execution claims in either player.

Both six-file audits complete all 134 checks without a parser error. With the
recorded runtime, exactly two comparisons fail: launch 1 / frame index 2,
Flora repeat 0 and original GPA repeat 2. Each differs at four RGBA8 pixels,
seven channels, maximum absolute channel difference 1. These reproduce the
existing failure; they are not newly executed GPU results. The other 34 saved
positive executions match exactly, and all six negative controls pass.

Requesting the current deferred-Resolve runtime adds the expected runtime hash
failure. The six files were captured with `f2d92dd9...`, while the current
runtime is `4b314e2f...`. Passing retained-file checks cannot establish acceptance
of that newer runtime. Both audit commands correctly return exit status 1.

The original GPA GUI was reopened on equipment frame
`23c72ea8-d6f0-4557-96e7-48513d0f27bd`. It displays the final image and 637 events.
Input to the loaded analysis view repeatedly times out after window refresh and
automation-session recreation. The process still reports responding, which does
not prove its details UI works. A manual Draw-selection check was requested;
no answer was available when this evidence was recorded. The screenshot proves
only the displayed final view, not individual shader/resource/pipeline inspection.

The formal six-file gate remains incomplete. This audit always reports
`formal_acceptance_established: false`; scene correspondence, detailed per-file
inspection and full current-package acceptance remain separate requirements.
Production Capture, Replay and Format sources/binaries were not changed, so no
new GPU CTest or golden-frame run is claimed for this tooling change.

## Reproduction

From the repository root, with development Python available:

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tools/test_audit_capture_gf2_evidence.py -v
python tools/audit_capture_gf2_evidence.py `
  --evidence artifacts/capture-gf2-current-20261014 `
  --runtime build/capture/capture/Release/FloraCaptureRuntime.dll `
  --controller build/capture/capture/Release/FloraCapture.exe `
  --replay build/vs2022/Release/FloraGPA.Cli.exe `
  --out C:/Users/Myrrr/FloraGPA-evidence/new-current-audit.json
```

Use the retained `capture-package/FloraCaptureRuntime.dll` to reproduce the
recorded-runtime comparison, with a different new output path. The
[baseline](capture-gf2-evidence-audit-baseline.json) records full binary identities,
source hashes and raw report/screenshot locations. Raw captures and outputs stay
outside Git.
