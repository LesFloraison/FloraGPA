# Minimum-precision pixel sampling with per-sample clamps

## Scope and implementation

This P6 increment enables the native executor's existing floating minimum-precision
sampling path for PS `sample_cl_s`, `sample_b_cl_s` and `sample_c_cl_s`. It extends
the [clamp implementation](DX11_NATIVE_CLAMP_FEEDBACK.md) and
[ordinary precision sampling](DX11_PIXEL_SAMPLING_PRECISION.md).

The executor accepts minimum precision on sampled data, coordinates and the
optional bias/comparison operand. It keeps the feedback status destination,
resource/sampler operands and clamp at full precision. The GPU helper locates the
compiled clamp opcode and accounts for the additional status operand before
applying precision decorations. Its existing reduced scratch result and widening
move remain in use. The existing cache key includes the precision tuple and clamp
presence; changed clamp values are supplied at execution time.

There is no new fallback adapter or tool dependency. Backend identity remains
`dx11-native/11` and helper identity remains `dxbc-gpu/29`; the Worker executable
digest continues to distinguish analysis contexts after implementation changes.

This does not add minimum precision to the clamp itself, explicit-gradient
sampling or VS/CS sampling. Existing direct-output restrictions remain. Original
captured PS selection, source parity and the full instruction/format matrix remain
open P6 gates. No RenderDoc dependency is removed by this increment.

## Independent baseline and probes

The baseline is implementation commit
`e1dbe79727c32f01407e09958dad8e9d6d932782`, with the new probe added before the
product change. All eighteen new cases executed their original compiled shader
successfully, then failed native parsing with the expected unsupported minimum
precision diagnostic for opcode 230, 231 or 233. The baseline Qt result is two
setup/cleanup passes, eighteen failures and no skips.

The matrix explicitly selects WARP, NVIDIA and Intel at feature level 11.1. Each
adapter runs `min16float` and `min10float` for implicit, bias and comparison
sampling. A three-mip R32_FLOAT texture, linear sampling, an immediate texel
offset and reduced-precision arithmetic exercise sampled data, coordinates and
the dynamic bias/comparison parameter. Four lanes use different float32 clamps.

`TextureOperationOracle::directTexture` creates a pixel shader from the unmodified
compiled reference bytecode and executes it before constructing the native
`Program`. Every original result and shader is retained even when native parsing
fails. Native execution uses the independently generated helper shader. The
post-change reference bytecode and original outputs must match the baseline
exactly; they are not regenerated goldens accepted without comparison.

The tests compare 72 original result vectors to 72 native vectors, then reverse
the lane clamps and execute another 72 native vectors on the same GPU helper.
Each of eighteen additional executions marks one clamp unknown and verifies
validity `[15, 0, 15, 15]`, retaining exact results for the other lanes.
The Python verifier separately parses DXBC operands, checks explicit adapter and
feature-level selection, confirms that clamps change original GPU outputs and
rejects ninety corruptions of values, cache results, validity or case identity.

Coordinates and bias/comparison inputs are constant across the tested quad.
The reference executes each selected parameter on a one-pixel target. These
measurements do not establish arbitrary divergent captured-quad equivalence or
qualify original fragments. Precision equality is evidence for these shaders on
the recorded devices, not a cross-device floating-point equivalence guarantee.

## Regression findings

The focused post-change matrix records twenty passes and no failures or skips.
The three related regression suites record 498 passes, one failure and no skips:

| Suite | Passed | Failed | Skipped | Elapsed ms |
| --- | ---: | ---: | ---: | ---: |
| DxbcClampFeedback | 187 | 1 | 0 | 23,844 |
| DxbcPixelSampling | 243 | 0 | 0 | 567,067 |
| NativeComputeDebug | 68 | 0 | 0 | 11,652 |

These elapsed times describe these runs, not controlled P8 performance evidence.
The complete clamp suite's sole
failure is the existing Intel literal-zero SampleBias clamp-effect discrepancy
at `execution(32902-0-1-0)`. The native output agrees with the original shader,
but the independent clamp-effect expectation still fails. The 162-case legacy
verifier also retains that semantic conflict and its eight dynamic-bias
differences. No assertion, golden, skip or acceptance condition was weakened.

The new minimum-precision bias cases use a dynamic operand and do not claim to
resolve the Intel immediate-bias discrepancy. Full P6 and standalone release
acceptance remain incomplete.

## Reproduction and retained evidence

Build Release targets `FloraDxbcClampFeedbackTests`, `FloraDxbcPixelSamplingTests`,
`FloraNativeComputeDebugTests` and `FloraGPA.Cli`. Set the Qt binary directory on
`PATH`. Run GPU tests serially, and wait for each process to exit before reading
its Qt log.

```powershell
$env:FLORA_MINIMUM_CLAMP_EVIDENCE = '<new-minimum-evidence-directory>'
$env:FLORA_CLAMP_FEEDBACK_EVIDENCE = '<new-legacy-clamp-evidence-directory>'
& build/vs2022/Release/FloraDxbcClampFeedbackTests.exe -o '<new-clamp-log>,txt'
& build/vs2022/Release/FloraDxbcPixelSamplingTests.exe -o '<new-sampling-log>,txt'
& build/vs2022/Release/FloraNativeComputeDebugTests.exe -o '<new-compute-log>,txt'
python tools/validate_minimum_clamp.py --root '<new-minimum-evidence-directory>' --baseline '<retained-baseline-directory>' --out '<new-verification.json>'
python tools/validate_native_clamp_feedback.py --root '<new-legacy-clamp-evidence-directory>' --out '<new-legacy-verification.json>'
```

The clamp suite and legacy verifier are expected to report the recorded Intel
conflict on this driver. Their failure remains a release gate. Local evidence is
under `artifacts/dx11-native/minimum-clamp`, including baseline and corrected build
logs, original bytecode/results, changed-clamp/unknown-clamp results and complete
regression logs. Generated binary evidence stays outside Git. The adjacent
`dx11-minimum-clamp-precision-evidence.json` pins sources and retained artifacts.
