# Native depth-only completion observations

This increment adds an internal P5 diagnostic for draws with a DSV and no color
targets. It does not switch public history or pixel debugging to a new backend.
Original fragment identity, order and eligibility remain unknown, and the full
debugger, dependency-removal and standalone release gates remain open.

## Execution and result contract

`addPixelCompletionTarget` adds a float RGB output declaration and signature to
an SM4/4.1/5 pixel shader without color outputs. Original executable instructions
and depth/coverage signature entries are preserved. The existing return marker
transform then writes `(1, 2, 3)` before main-entry returns. Stale statistics and
debug chunks are removed; this bytecode is diagnostic, not a source-debug shader.
Color signature/declaration entries must precede special depth/coverage outputs.
For a null original PS, the diagnostic compiles an empty PS before transformation;
the result explicitly records that the original PS was null.

`replayNativeDepthCompletion` first executes the unmodified draw on private output
clones. It then runs the marked draw on fresh clones with a private float RTV.
The request selects x/y, a layer relative to the bound DSV view, and an explicit
sample. Depth/stencil, discard, shader depth/coverage and original sample masks
remain active. The result contains the observed marker bits and separate original
and diagnostic query results. It does not identify an individual fragment.

Both passes share one cancellation/deadline scope. MSAA transport reads propagate
that scope through the shared native transfer helper and staging readback. The
logical transport budget reserves three sample-weighted RGBA32 planes in addition
to existing private-output and shader budgets. This is not a bound on driver
allocations. The caller's observer still owns its own readback/timeout limits.

The API currently requires a 2D-family DSV, no RTVs, an unused OM UAV slot zero,
and disabled alpha-to-coverage. It rejects configurations whose output/alpha
contract cannot be preserved by adding target zero. Array and mip selection code
is present but not validated by this increment's one-pixel fixtures. These are
limitations of a new internal diagnostic, not replacements for existing public
feature support. No support restriction is waived for final migration acceptance.

## Counter provenance

On Intel driver `32.0.101.8826`, early-depth discard cases retain depth/stencil
bytes but change occlusion accounting when a color output is added. Six final
selections exhibit this: early discard and early stencil discard at single sample,
4x sample zero and 4x sample three. Unmodified counts are respectively 1, 4 and 4;
diagnostic counts are zero. Counts refer to the whole draw, including when a single
sample is selected for marker readback.

The initial implementation incorrectly required those counts to match and failed.
The final API measures the unmodified pass separately, returns that pass under
`reference_draw`, and uses its values for top-level `passing_samples` and
`pipeline_statistics`. Diagnostic values retain their own explicit fields and
count domain. `diagnostic_counts_match` reports the difference. Neither replacing
the original count nor treating the diagnostic count as original eligibility is
permitted. Existing completion-query deviations remain open.

## MSAA allocation failure and correction

The first expanded matrix used typed RGBA32_FLOAT MSAA transport storage. WARP
and NVIDIA passed all sixty selections. Intel produced four device-hung errors
and twenty-four subsequent adapter-enumeration failures: the full run ended with
64 passes and 28 failures, including Qt setup/cleanup. No case was skipped.

A bounded single-case probe located the failure at private depth readback after
the diagnostic color read. Detaching the added RTV did not fix it. Reordering
the reads moved the failure to subsequent original-depth verification. An
unmodified private draw control passed. These observations agree with the
independent repeated typed-MSAA-copy probes already recorded in
[the MSAA merger study](DX11_NATIVE_PIXEL_MERGER_MSAA.md); they do not establish
that compatible-format copies are prohibited by D3D11 or that every Intel driver
has this failure.

The new transport now follows the project's existing MSAA allocation contract:
RGBA32_TYPELESS storage with an explicit RGBA32_FLOAT RTV and raw integer-family
sample readback. Single-sample storage remains RGBA32_FLOAT. The previously
failing Intel case and the complete matrix pass with repeated subsequent depth
reads and original-resource restoration checks. No driver reset, adapter fallback,
assertion reduction or new skip was used. Failed development logs remain recorded.

## Validation

The final completion executable passes **222 entries, zero failures, zero skips**
in 33.214 seconds. This includes ninety depth-only selections, three unmodified
MSAA private-draw controls, existing color/depth/UAV/class-linkage/merger/recovery
tests, and shader-boundary tests. All GPU cases execute serially on explicit WARP,
NVIDIA vendor 4318 and Intel vendor 32902. The hardware devices are 11352 and 32103;
drivers are `32.0.16.1742` and `32.0.101.8826` respectively.

The ten depth modes cover pass, depth failure, discard, early discard, shader
depth, shader coverage, null PS, stencil pass/failure, and early stencil discard.
Each runs at one sample and at 4x samples zero/three. Private depth/stencil bytes
must equal an independently replayed original draw. Original storage is reread
after diagnostics; RTV restoration, invalid coordinates and entry cancellation
are checked. The added shader tests reject existing color outputs, wrong stages,
invalid targets, truncated containers, cancellation and insufficient payload
budgets, with SM4/4.1/5 transformation coverage.

`validate_native_depth_completion.py` requires terminal Qt passes for every
selection, validates actual adapters and explicit count provenance, and compares
the original/prepared/marked DXBC bytes. It checks that only the additional color
declaration/signature and return markers changed, preserving executable depth,
coverage and discard operations. It rejects 720 corrupted observations or shader
payloads, including changed instructions/signatures with recomputed hashes.

The intermediate 95-pass/one-failure run exposed a test-fixture budget mistake:
removing stale STAT data can make the transformed shader smaller than the input.
The corrected budget test first removes STAT, then requires rejection when the
additional output exceeds that compact input's exact byte length. The allocation
assertion remains strict; the failed log is retained.

GUI, CLI, Worker and completion-test Release targets build successfully. This
does not constitute the P8 clean-profile build/package/module audit. See the
accompanying evidence manifest for file hashes and retained development outcomes.
The shared geometry replay regression also passes 257 entries in 81.103 seconds;
its validator checks 255 observations and rejects 1,836 corruptions with no pixel
or sample-count mismatches. The implementation commit is
`572693e30376bdbe1ebbc988ab5094af14623957`.

## Reproduction

Build `FloraDxbcPixelCompletionTests` in Release and put the configured Qt runtime
on PATH. Use a new evidence directory and wait for process completion before
reading its log:

```powershell
$env:FLORA_PIXEL_COMPLETION_EVIDENCE = 'artifacts/dx11-native/repeat-depth-completion'
& build/vs2022/Release/FloraDxbcPixelCompletionTests.exe -o artifacts/dx11-native/repeat-depth-completion.txt,txt
python tools/validate_native_depth_completion.py artifacts/dx11-native/repeat-depth-completion --test-log artifacts/dx11-native/repeat-depth-completion.txt --out artifacts/dx11-native/repeat-depth-validation.json
```

Individual original-draw controls use
`depthTransport:32902-original-control-4-0` (substitute the explicit vendor).
Preserve failing results and stop broad GPU testing on device removal; a later
pass alone does not erase a failed run or establish full pixel-history parity.
