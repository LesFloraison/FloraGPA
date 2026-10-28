# Native minimum-precision gather execution

The independent VS/PS/CS executor now carries minimum-precision operand metadata
through all four gather operations: `gather4`, `gather4_c`, `gather4_po` and
`gather4_po_c`. This removes their previous blanket minimum-precision rejection.
It does not complete the shader-debug or fragment-history parity gates.

## Execution contract

The CPU executor retains register identities, masks, control flow and validity.
The selected adapter executes a private shader containing the gather with its
destination, coordinate, programmable-offset and comparison-reference precision
annotations. Source modifiers and immediate operands remain explicit. The helper
does not replace minimum precision with a fixed CPU truncation rule.

Temporary results are promoted through a matching precision read before transport
to raw host words. Direct VS/PS outputs preserve their precision declaration and
output signature. Shader refactoring flags are carried into the helper. Private
resources and explicit adapter selection retain the existing failure/cancellation
contract. The helper identity advances from `dxbc-gpu/23` to `dxbc-gpu/24`.

Microsoft's [minimum-precision specification, section 7.20.3](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
distinguishes source storage precision, operation/destination precision and
address precision. Its [HLSL guidance](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/using-hlsl-minimum-precision)
allows devices to execute at greater precision. The tests therefore use the
explicitly selected GPU as an independent implementation of the authored shader;
they do not require NVIDIA, Intel and WARP to quantize identically.

Unknown texture storage invalidates the selected result conservatively. Unknown
constant inputs also remain unknown, except when the actual instruction selects
an immediate literal instead. Malformed precision codes, irrelevant operand
controls and unsupported compute output-register requests are rejected.

## Independent authored-shader evidence

The oracle submits original authored DXBC directly to a separate D3D11 device,
using `b2`, `t3`, `s4` and `r3` rather than the helper bindings. It does not invoke
the executor or its GPU helper to generate expected results. VS uses stream
output, PS a matching raw/type-preserving target, and CS a structured output UAV.
Original bytecode and both raw results are retained per comparison.

The matrix has 216 configurations across WARP, NVIDIA and Intel; VS, PS and CS;
2D, 2D array, cube and cube array; all dimension-valid gather opcodes; and float16
and float2.8 minimum result types. Eight modes vary mixed precision, destination
precision zero, programmable signed/unsigned offset precision, all float source
modifiers, source channel, full/partial destination masks, refactoring flags and
coordinate/offset/reference literals. VS/PS also exercise direct output registers.
Input values remain within the tested minimum formats' defined ranges.

Each comparison tests missing texture storage and independently removes each
applicable constant source. The constant-source tests require no GPU operation
for unknown input and a known result when the instruction instead uses a literal.
Selected authored snapshots also export, reload, rewind and restore execution
journals. Those snapshots do not establish original captured-pixel invocation
qualification or original GPU scheduling.

The independent Python validator decodes instruction operands and declarations,
checks source precision/modifiers/literal provenance, output transport, bindings,
raw results and the complete configuration matrix. Deliberate corruptions must
fail, including altered bits, precision, source validity and journal evidence.

## Retained development failures

The initial test build referenced nonexistent fixture helpers. Reusing the
existing synthetic capture writer fixed the build; the failed log is retained.
Three representative WARP stage cases then passed.

The first full matrix passed 182 checks and failed 36 NVIDIA float16 cases. The
diagnostic run localized the difference to untouched `y/z` components under a
partial `x/w` write. The oracle read all four components through a float16 source
operand, even though `y/z` retained their original full-precision initialization.
On this NVIDIA driver that read produced zero for the untouched components; the
selected gather components already matched the helper.

The corrected original shader first transports the raw register, then replaces
only the written components through the matching minimum-precision read. This
retains both checks: raw untouched components must preserve their original bits,
and written components must equal direct original gather execution. No expected
result is patched, tolerance introduced or configuration skipped. The original
failure remains recorded as a fixture-transport failure, not a historical main
failure. Original bytecode proves the corrected two-read transport.

The next full precision matrix passes 218 Qt checks, with 2,880 direct GPU
comparisons and 180 rejected corruptions. This intermediate run includes unknown
constant/literal tests but precedes the final journal checks.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcGatherTests FloraDxbcTextureLoadTests FloraDxbcExecutionTests FloraDxbcExecutionTraceTests FloraDxbcExecutionTraceReaderTests FloraNativePixelDebugTests FloraNativeComputeDebugTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
$env:FLORA_GATHER_EVIDENCE = "$PWD/artifacts/dx11-native/gather-precision-rerun/ordinary"
$env:FLORA_GATHER_PRECISION_EVIDENCE = "$PWD/artifacts/dx11-native/gather-precision-rerun/precision"
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^(dxbc_(gather|texture_load|execution|execution_trace|execution_trace_reader)|native_(pixel_debug|compute_debug))$'
python tools/validate_native_gather.py artifacts/dx11-native/gather-precision-rerun/ordinary --test-log build/vs2022/dxbc-gather-Release.txt --out <new-ordinary-ledger.json>
python tools/validate_gather_precision.py artifacts/dx11-native/gather-precision-rerun/precision --test-log build/vs2022/dxbc-gather-Release.txt --out <new-precision-ledger.json>
```

Use new evidence directories and run GPU tests serially. Read Qt logs only after
their processes finish. The focused regression exercises the shared texture
transport, executor, journals and native Worker consumers affected by this change.

## Final validation and remaining gates

The final serial regression passes **3,515 Qt checks across seven suites, with
zero failures and zero skips**, in 540.62 seconds. The gather suite passes 543
checks in 95.211 seconds. GUI, CLI, Worker and eleven related test executables
build successfully; only the seven named suites are claimed as rerun here.

The final minimum-precision validator accepts all 216 configurations and 2,880
direct GPU comparisons, including 72 journal round trips, and rejects 198
corrupted reports. The ordinary gather matrix also passes on `dxbc-gpu/24`: 324
configurations, 2,808 GPU comparisons, 108 explicit unsupported-format contracts,
36 journal round trips and 99 rejected corruptions. In total these runs provide
5,688 comparisons and 108 journal checks. Unsupported formats are rejection
contracts, not successful shader executions or skipped tests.

`dx11-native-gather-precision-evidence.json` pins both result matrices, completed
logs, implementation/validator sources, fourteen executables and retained
development failures. No original golden, assertion or skip range was relaxed.
These test durations do not certify P8 replay latency or peak-memory acceptance.

Minimum-precision integer
gather formats, feedback/status variants, precise unknown-texel footprints,
captured invocation qualification, full P1/P5/P6 parity and P7/P8 release and
dependency-removal acceptance remain open. Product code does not load original
GPA modules; the legacy analysis routes elsewhere in the branch still remain.
