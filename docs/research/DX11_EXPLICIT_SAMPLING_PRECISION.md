# Minimum precision in native explicit sampling

## Implementation and scope

The native DXBC executor now supports floating minimum precision for
`sample_l`, `sample_c_lz` and their feedback forms `sample_l_s` and
`sample_c_lz_s` in VS, PS and CS. The parser previously rejected these operands;
the explicit GPU request also discarded their precision tuple.

The request now carries destination, coordinate and LOD/comparison precision
through `TextureInputs` to the private device. The stage-specific helper decorates
the actual sampling operands and declares minimum precision. A reduced temporary
is widened through the matching precision annotation before raw result storage.
The generated instruction words, including these annotations, form the existing
shader-cache key. Each execution supplies its current parameters independently.

Feedback status stays opaque, and binding/status operands remain full precision.
Unknown coordinates or scalar parameters remain unknown. GPU selection, private
resource ownership, cancellation/completion handling and full-precision sampling
paths are unchanged. Backend/helper identities remain `dx11-native/11` and
`dxbc-gpu/29`; the Worker executable digest distinguishes analysis contexts for
this implementation.

This increment does not implement minimum-precision explicit gradients or remove
the existing reduced direct-output destination restriction. The probe matrix
uses 2D R32_FLOAT resources; it does not establish every format/dimension or
captured-game workflow. P6 parity, original-fragment qualification and standalone
release acceptance remain open. No dependency is removed by this change.

## Baseline and original execution

The source baseline is `02b3d3767a1ae37f0595b614baf4be6b09292343`. The new test was
built and executed before changing the product. Its 288 configurations combine
three explicitly selected adapters (WARP, NVIDIA, Intel), three stages, explicit
LOD/comparison sampling, ordinary/feedback forms, float16/float2.8 precision and
four annotation scenarios (destination, coordinates, scalar parameter, all three).

The first test assumed that result-only comparison shaders would reduce the
sampling destination itself. FXC instead keeps that comparison instruction at
full precision and reduces downstream arithmetic. Thirty-six assertions exposed
this fixture assumption. The corrected test explicitly requires a zero sampling
precision tuple for those cases and retains them as controls. It does not skip
them or modify an existing golden. The initial build error and first matrix are
retained alongside the corrected baseline.

The corrected baseline records 38 passes (36 controls plus setup/cleanup) and
252 parser failures. Every failure names unsupported minimum precision on opcode
71, 72, 228 or 229. All 288 original shaders and their original GPU results were
saved before constructing the native `Program`.

`TextureOperationOracle::directTexture` executes the original compiled bytecode
on the explicitly selected stage. Each case uses four LOD/reference values, a
three-mip texture, linear sampling, a texel offset and arithmetic that preserves
the requested operand precision. The original reference and native helper are
different shader programs. Original bytecode and GPU output must match the
pre-change baseline exactly.

## Validation

The new suite records 290 passes, no failures and no skips (46,392 ms). All 252
previously rejected precision cases now execute; the 36 control cases remain
successful. The comparisons cover 1,152 original output vectors, 2,304 native
vectors across initial and reversed parameter order, and 576 unknown-input
results. The same helper object executes both orders. Resource snapshot bytes
must remain unchanged.

The independent Python verifier decodes shader stage/model, sampling opcode,
operand extensions and precision placement. It verifies the full matrix,
explicit adapter identities, unchanged baseline shader/results, exact raw output,
cache parameter changes and unknown validity. It rejects 1,728 corruptions of
result bits, cache results, validity, adapter identity, precision or opcode.

Eight related suites record 3,036 passes, thirteen failures and no skips:

| Suite | Passed | Failed | Elapsed ms |
| --- | ---: | ---: | ---: |
| DxbcExplicitSamplingPrecision | 290 | 0 | 46,392 |
| DxbcSampleLevel | 156 | 0 | 79,307 |
| DxbcSampleComparison | 190 | 0 | 129,363 |
| DxbcSampleGrad | 262 | 0 | 75,867 |
| DxbcTextureLoad | 1,634 | 0 | 319,814 |
| DxbcTextureFeedback | 249 | 12 | 26,132 |
| DxbcClampFeedback | 187 | 1 | 23,321 |
| NativeComputeDebug | 68 | 0 | 14,032 |

All thirteen failures match the retained previous failure cases and messages
exactly. The original null-shader creation probe's complete JSON result also
matches. They are the twelve Intel original null-output shader creation failures
and the Intel literal-zero SampleBias clamp-effect failure. These previously
recorded research failures remain open; this comparison does not waive them under
the P0 baseline policy or establish standalone release acceptance.

Integer bit-pattern equality here compares native execution with the original
shader on the same adapter. It is not a promise that minimum precision has one
cross-adapter representation. These focused runs are not P8 performance evidence.

## Reproduction

Build `FloraDxbcExplicitSamplingPrecisionTests` in Release and put the matching
Qt binaries on `PATH`. The CTest entry is serial and has a 300-second timeout.
Run GPU suites serially and read their Qt logs only after process termination.

```powershell
$env:FLORA_EXPLICIT_PRECISION_EVIDENCE = '<new-evidence-directory>'
& build/vs2022/Release/FloraDxbcExplicitSamplingPrecisionTests.exe -o '<new-log>,txt'
python tools/validate_explicit_sampling_precision.py --root '<new-evidence-directory>' --baseline '<retained-baseline-directory>' --out '<new-verification.json>'
```

The local evidence root is `artifacts/dx11-native/explicit-sampling-precision`.
`baseline-controls` is the corrected pre-product-change baseline, and `native`
contains the passing matrix. The adjacent
`dx11-explicit-sampling-precision-evidence.json` pins implementation sources,
build/test logs, generated bytecode and comparison reports. Generated executable
snapshots and probe artifacts stay outside Git.
