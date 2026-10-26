# Minimum precision in native implicit pixel sampling

The native PS executor now carries floating-point minimum precision for temporary
destinations, coordinates and scalar parameters through `sample`, `sample_c` and
`sample_b`. This closes the parser guard first encountered at Helldivers event
200, DXBC word 506. It does not complete that shader or the P6 parity gate.

## Execution contract

Precision is part of the quad request, cohort agreement and bounded shader-cache
key. The helper preserves the original precision annotations at the hardware
sampling instruction. A separate sampled temporary is widened through a source
operand with the same precision before the existing raw output routing. Global
minimum-precision flags are retained. The helper identity is `dxbc-gpu/18`.
There is no CPU half-float rounding substitute or adapter fallback.

The [D3D11.3 functional specification, sections 7.20.3.3-7.20.3.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
permits precision annotations on texture fetches, distinguishes stored source
precision from the destination's operation precision, and treats addressing
precision separately. Constants still arrive as 32-bit values. These rules are
why both the sample destination and its later read need correct annotations.
Drivers may execute above the requested minimum; the implementation observes
the selected adapter rather than imposing one universal numerical conversion.

Output-register sampling destinations, minimum-precision explicit sampling,
integer sampling annotations and resource/sampler precision remain guarded.
Minimum-precision loads and PS arithmetic output registers are separate remaining
requirements. No existing backend or capability is removed by this checkpoint.

## Independent fixtures and retained failures

The reference probe executes shaders with public D3D11 draw/readback calls. It
does not call the native lane executor, GPA or RenderDoc. The owned DXBC container
codec is used only to serialize explicit comparison fixtures. Compiler-produced
bytecode is retained separately as `.compiler.dxbc` for those fixtures.

The six scenarios are destination-only float16, coordinate-only float16,
parameter-only float16, combined float16, destination-only float2.8, and combined
float2.8. All run on explicit NVIDIA, Intel and WARP devices. Each has 576 cases:
three sample operations, point/linear/anisotropic filtering, two offsets, eight
input/control-flow datasets and four coverage masks. The sample operation's
actual operand annotations are asserted before execution. The parameter scenario
has no parameter annotation for ordinary `sample`, which has no scalar operand.

Early HLSL casts were optimized away. A retained intermediate run then failed six
annotation checks because FXC promoted destination-only comparison samples. The
explicit fixture originally changed the destination but left its subsequent
read at default precision; NVIDIA returned zero for a case where the native
executor returned a nonzero value. The fixture now updates both annotations.
The original bytes, failed comparisons and logs remain outside Git. Assertions,
existing goldens and skip rules were not relaxed to make these attempts pass.

## Validation

The final sampling suite passes 186 checks; the complete DXBC execution suite
passes 445. Both have zero failures and skips. The unchanged sampling matrix
still checks 9,408 scenarios and 71,424 components, including 192 rejections of
undefined comparison-sampler cases. The new matrix compares 10,368 scenarios
and 80,352 active output components exactly. Independent Python comparisons of
the saved GPU/native results pass, including 126 corruption controls.

Six new GF2 event 113 and BF1 event 2010 runs complete on the three adapters.
Their 381 selected and 1,524 global steps pass the C++ reader and independent
Python journal checks, including 120 corruption controls and register rewind.
These are private pre-depth invocation results, not original fragment eligibility
or an independent comparison of the complete captured PS output.

All three Helldivers event 200 runs still fail during program construction. The
first remaining guard is now opcode 45 at word 1853: a typed-buffer `ld` with a
minimum-float16 destination and minimum-uint16 address. No partial journal is
published. Moving this guard does not demonstrate execution of the game shader.

GUI, CLI and Worker Release builds pass. The sampling suite took 401.546 seconds
and the execution suite 38.831 seconds in these runs. They include new coverage
and are not controlled performance measurements or P8 acceptance evidence.
The ledger is [dx11-pixel-sampling-precision-evidence.json](dx11-pixel-sampling-precision-evidence.json).

## Reproduction

Build `FloraDxbcPixelSamplingTests`, `FloraDxbcExecutionTests`,
`FloraDxbcPixelGeometryProbe` and `FloraDxbcTraceReaderProbe` in Release. Run GPU
tests serially with the Qt binary directory on `PATH`:

```powershell
$env:FLORA_KEEP_PIXEL_SAMPLING_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcPixelSamplingTests.exe -o sampling-tests.txt,txt
build/vs2022/Release/FloraDxbcExecutionTests.exe -o execution-tests.txt,txt
python tools/validate_pixel_sampling_precision.py <saved-test-directory> --out <new-report.json>
```

The test log records its retained evidence directory. The probe can also generate
an individual reference matrix using `NEW_DIRECTORY VENDOR PRECISION_SCENARIO`.
For log validation, call `summarize_dxbc_pixel_sampling.validate` with backend
`dxbc-gpu/18` and `minimum_precision=True`; historical validation remains available
with its original backend and default precision option. The existing real-frame
probe/reader and `validate_pixel_execution_trace.py` commands apply unchanged.

Fragment qualification, helper memory semantics, minimum-precision loads/outputs,
additional shapes and formats, public PS jobs/sessions, source-debug parity,
RenderDoc deletion and standalone release acceptance remain open.
