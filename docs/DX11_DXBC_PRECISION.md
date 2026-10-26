# Native DXBC arithmetic precision

This P6 checkpoint closes the selected BF1 and Helldivers VS execution failures
recorded by the vertex-input checkpoint. It does not establish complete shader
debugger parity or permit retiring the existing backend.

## Encoding and execution

The operand extension uses bits 14 through 16 for minimum precision. The
independent parser accepts default, float16, float2.8, signed16 and unsigned16
encodings, and rejects reserved values. Source modifiers remain separate.
Minimum-precision arithmetic executes on a private D3D11 device with the exact
selected adapter LUID; the C++ lane retains control flow, register state and
component validity. Missing assistance is an explicit error before state commit.
Minimum-precision resource/control operands and relative index conversion are
still rejected rather than interpreted as full precision.

The native instruction helper preserves precision on destinations, substituted
constant-buffer sources and result reads. Literal operands remain literal.
VS, PS and CS helpers use their respective shader stages and private outputs;
no original shader UAV slot is required. A VS output destination retains its
output register class and precision-bearing signature. Remapping it through a
minimum-precision temporary can introduce another narrowing step. In particular,
the initial implementation produced three different Helldivers normal components
on NVIDIA; preserving the output destination eliminated those differences.
Independent compiled output fixtures exercise the same distinction without the
game shader. This is an observed driver behavior, not a universal claim that
all minimum-precision output registers always retain 32-bit precision.

VS dot and multiply-add operations also use the selected adapter when assistance
is available. Separate CPU multiply/add rounding did not match the selected BF1
shader's original hardware execution. The helper retains each DXBC instruction,
its output mask, saturation, source selection and literals. Unknown components
remain unknown according to the C++ data dependencies; readback never promotes
them to valid values. Helpers retain bounded GPU completion, cancellation,
failure poisoning and bounded shader caches. Their identity is `dxbc-gpu/17`.
For a minimum-precision conditional move, both candidate branches must be known
before the result is marked valid: narrowing the condition can change its truth
value, so the unrounded CPU value cannot establish which unknown branch was used.

The encoding follows Microsoft's [tokenized program format declaration](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/Support/d3d12TokenizedProgramFormat.hpp).
Microsoft's [minimum precision documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/using-hlsl-minimum-precision)
allows execution at greater precision than the declared minimum. The product
therefore observes the selected device instead of applying a universal half-float
rounding rule. No external debugger implementation is used.

## Reproduction

Build `FloraDxbcExecutionTests` and `FloraDxbcVertexInputProbe` in Release.
The execution suite includes independent HLSL GPU references for minimum-precision
float/integer arithmetic, dot, multiply-add, modifiers and output signatures.
Raw instruction cases cover sparse validity and all four precision encodings in
VS, PS and CS on NVIDIA, Intel and explicit WARP. Cases also cover validity when
condition narrowing can change branch selection. Invalid encodings, unsupported
precision uses and missing assistance fail without publishing register changes.

Run selected real draw comparisons with the native probe:

```powershell
python tools/validate_dxbc_vertex_execution.py `
  --probe build/vs2022/Release/FloraDxbcVertexInputProbe.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --case D:/CDXrepo/captures/GF2_Exilium_2026_03_03__00_19_35.gpa_frame 113 `
  --case D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame 876 `
  --case D:/CDXrepo/captures/helldivers2_2026_04_02__18_02_58.gpa_frame 200 `
  --out artifacts/dx11-native/vertex-precision-validation
```

The default adapters are NVIDIA, Intel and WARP, executed serially. Each case
uses reference 0 and instance 0; these are selectable arguments. Reports retain
frame/shader/probe hashes, adapter/driver/helper identity, operation counts,
timing, every compared raw output component and validity. The script checks exact
equality and adapter identity. It never launches RenderDoc or original GPA.

## Validation

Validation records 2,762 passing checks across ten affected suites, with no
failures or skips. GUI, CLI and Worker Release targets build successfully.
The full execution suite was repeated after the final precision guards; the
resource/sampling/completion suites ran before those guards. All nine real
comparisons match exactly with no unknown output components: GF2 event 113 has
14 components after 12 steps, BF1 event 876 has four after 58 steps, and
Helldivers event 200 has 13 after 42 steps, on each of the three adapters.
The selected runs use 6, 45 and 24 GPU helper operations respectively. Measured
single-process wall times range from about 0.14 to 1.83 seconds; these include
input acquisition and the independent original-shader output comparison and
are not a debugger latency benchmark. Repetition, peak memory and full baseline
performance acceptance remain required. Log/report hashes, adapter identities,
raw failures from intermediate attempts and exact selections are recorded in
`research/dx11-native-precision-evidence.json`.

## Limits

These comparisons validate selected invocation outputs, not every intermediate
register, branch, input or optimization of arbitrary shaders. Whole-program
driver optimization can differ from isolated operations and requires additional
independent cases. Minimum-precision resource/control operands, index conversion,
PS output destinations and multi-target VS output instructions remain explicit
unsupported cases. PS input/quad capture and remaining execution semantics are
still open. Native
VS journal/session integration, complete Pixel History and standalone dependency
removal also remain open. Existing goldens and historical failures are unchanged.
