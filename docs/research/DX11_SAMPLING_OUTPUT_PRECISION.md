# Minimum-precision outputs from explicit sampling

## Implementation

Native VS/PS `sample_l` and `sample_c_lz`, including feedback forms, now retain
minimum precision when writing an output register. The previous parser rejected
every reduced output destination, and the sampling request always selected the
temporary-result GPU path. The request now identifies an output destination and
uses the existing stage-specific output declaration, operand precision and
floating output transport. The native register update still applies only the
instruction's write mask. Arbitrary PS output slots are mapped to the helper's
private output zero; the original destination remains the trace register.

Feedback status remains a separate opaque temporary. CS output destinations stay
rejected; valid CS sampling continues through its existing register/UAV path.
The implicit PS output and minimum-precision gradient limitations are unchanged.
Backend/helper identities remain `dx11-native/11` and `dxbc-gpu/29`; the Worker
digest distinguishes the changed analysis executable.

This is an incremental P6 change. It does not qualify original fragments,
application blending/render-target conversion, all dimensions/formats, source
debugging parity or standalone release acceptance. It does not remove any
RenderDoc dependency.

## Independent baseline

The product baseline is `c1e763e24839662c1170e2488bb18deb114980f9`. The probe and
its optional original-PS output-slot selection were added before the product
change. All original programs execute successfully on the explicitly selected
WARP, NVIDIA and Intel adapters at feature level 11.1. Twenty-four configuration
groups then fail native parsing at their first reduced output sampling operation
(opcode 71 or 72 at word 29). Qt records two setup/cleanup passes and twenty-four
failures, with no skips.

The fixture programs are independently assembled DXBC using a compiled output
signature, declarations, a seed output write and a sampling operation. They are
not claimed to be untouched compiler-generated application shaders. The ordinary
and feedback forms retain a real output data destination; the feedback form also
has a temporary status destination. The original GPU executes these programs
directly, while the native executor generates its separate helper shader.

The baseline 576-program matrix combines VS/PS, explicit LOD/comparison, ordinary/feedback
forms, float16/float2.8 output precision, full/reduced source precision and three
write masks. PS uses output slots 0, 3 and 7; VS uses slot 0. Each program executes
four LOD/reference values, producing 2,304 original vectors. All original
programs and results are retained before native program construction, including
the configurations after the first native rejection.

Review added twelve configuration groups with float32 outputs and reduced source
coordinates/parameters. These add 288 programs and 1,152 original vectors. They
were added after the product change and have direct original-GPU comparisons,
not a recorded pre-product-change native rejection. The final verifier requires
the original twenty-four baseline groups unchanged and all thirty-six final
groups present. The final matrix contains 864 programs and 3,456 original vectors.

The resource is a three-mip R32_FLOAT texture, with linear sampling and offset
(-1, 1). Ordinary samples use a reversed resource swizzle; comparison samples
replicate red. Seed values distinguish unwritten components from helper zeros.
Output observations use a 32-bit float target or VS stream output. They describe
logical output values, not the application's physical GPU register contents.

## Checks and reproduction

Known-input native results must match all original output words exactly. Four
parameter values run through the same helper object. Separate unknown-coordinate
and unknown-scalar requests must invalidate precisely the written components and
preserve the seed value/validity of unwritten components. The final matrix checks 1,728
such unknown-input results and requires resource snapshot bytes to remain intact.

The Python verifier checks the complete adapter/stage/program matrix, unchanged
baseline bytecode and original results where recorded, exact instruction sequence, output
declaration, output slot/mask/precision, bindings, resource swizzle, offsets and
source precision. It checks the known and unknown results separately and rejects
216 corruptions of data, validity, preserved components, adapter, precision or
matrix completeness. Higher precision on a selected adapter is preserved rather
than replaced by CPU rounding.

The final selected suites record 929 passes, one failure and no skips:

| Suite | Passed | Failed | Elapsed ms |
| --- | ---: | ---: | ---: |
| DxbcSamplingOutput | 38 | 0 | 29,219 |
| DxbcExplicitSamplingPrecision | 290 | 0 | 44,420 |
| DxbcSampleLevel | 156 | 0 | 80,977 |
| DxbcSampleComparison | 190 | 0 | 126,378 |
| DxbcClampFeedback | 187 | 1 | 22,101 |
| NativeComputeDebug | 68 | 0 | 12,543 |

The sole failure matches the prior Intel literal-zero SampleBias clamp-effect
failure, including its case and message. Other previously recorded research
failures, including original null-output shader creation, are not resolved by
this increment. GUI, CLI and Worker Release builds pass. These suite times are
observations of this run, not controlled P8 performance evidence.

Build `FloraDxbcSamplingOutputTests` in Release. Its CTest registration is serial
with a 600-second limit. Set the matching Qt binary directory on PATH and run GPU
suites serially. Read Qt logs only after the owner process terminates.

```powershell
$env:FLORA_SAMPLING_OUTPUT_EVIDENCE = '<new-output-directory>'
& build/vs2022/Release/FloraDxbcSamplingOutputTests.exe -o '<new-log>,txt'
python tools/validate_sampling_output.py --root '<new-output-directory>' --baseline '<retained-baseline-directory>' --out '<new-verification.json>'
```

Local evidence lives in `artifacts/dx11-native/sampling-output`. Initial compile
diagnostics and the first passing implementation run remain separate from final
verification. `baseline` contains the original twenty-four groups; `complete`
contains all thirty-six final groups. The adjacent
`dx11-sampling-output-precision-evidence.json` pins the
source, executable snapshots, logs and generated probe evidence. These binaries
and raw probe artifacts remain outside Git.
