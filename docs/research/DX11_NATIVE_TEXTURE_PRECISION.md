# Native minimum-precision texture loads

The independent DXBC executor now accepts minimum-precision `ld` operands for
Texture1D, Texture2D, Texture3D, Texture1DArray and Texture2DArray in VS, PS and CS.
Previously its precision gate accepted typed buffers but rejected these texture
dimensions. This is a P6 capability increment; full shader-debug parity, captured
pixel invocation qualification and standalone dependency removal remain open.

## Execution and validity

The program retains destination and coordinate precision, resource return class,
component selection, write mask, stage, immediate offsets and refactoring flags.
The helper emits typed DXBC resource extensions and executes the load on the
explicitly selected adapter. Temporary destinations widen through an operand
carrying their precision. VS/PS output destinations retain their stage output
declaration, signature and matching render/stream-output transport. Immediate
coordinate operands remain immediate, including in the address-conversion probe.

For minimum-precision integer coordinates, a separate GPU operation observes the
effective address for the CPU storage-validity calculation. The actual load still
receives the original operand bits and precision. This avoids both a guessed
CPU truncation rule and replacing the original load input with a transformed
coordinate. Unavailable texel data stays unavailable; verified out-of-bounds
results retain their defined validity. Partial register writes preserve the other
components. Private input bytes are not changed.

The GPU helper identity advances from `dxbc-gpu/21` to `dxbc-gpu/22`. Analysis jobs
also bind the actual Worker executable hash, so a rebuilt Worker cannot reuse an
old executable's cached result. Historical evidence for backend 21 is retained.
No RenderDoc or original GPA implementation is used by this path.

Minimum precision is not a promise that every adapter computes at exactly 16 or
10 bits. Direct3D permits a higher precision, including full-width execution.
The tests compare the native executor with the original test shader on the same
adapter and stage, not with a universal CPU rounding rule. See
[Using HLSL minimum precision](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/using-hlsl-minimum-precision)
and the [ld instruction reference](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld--sm4---asm-).

## Independent comparison

The owned fixture uses resource register `t3`, constant buffer `b2`, temporary
`r3`, and an independent output path. It submits the original DXBC directly to
D3D11. It does not invoke `GpuOperations` to produce the expected result. VS uses
stream output, PS uses a matching render target and CS uses a structured UAV.
The native executor separately evaluates the original load against private
snapshots and compares raw output words and validity.

The matrix includes WARP, NVIDIA and Intel; all five texture dimensions; all
three stages; float16, float2.8, signed16 and unsigned16 precision annotations;
full-width, signed16 and unsigned16 coordinates; constant-buffer and immediate
coordinates; temporary and applicable output destinations; full and partial
masks; a smaller mip; large positive and negative coordinates; and a second
observation with every source texel byte marked unknown. Integer and floating
raw results are compared with the same original GPU operation. Unknown data is
never replaced with an inferred known value.

Each retained record includes the original DXBC bytes, coordinates, precision,
selection, raw direct-GPU result, interpreter result, validity and helper identity.
The Python validator independently decodes the program header, declarations,
typed resource extensions and operands. It checks the complete matrix and rejects
altered outputs, validity, backend identity, missing cases and invalid bytecode.

## Retained development failures

The first build exposed a narrowing conversion in the test's D3D11 buffer flags;
the explicit unsigned conversion fixes that test construction. Initial original
shader creation failures were affected by a borrowed DXBC chunk whose temporary
byte buffer had expired. The fixture now owns the rewritten program bytes until
container creation completes. A diagnostic disassembler call against the invalid
container also threw and is retained in the failed-run logs. These observations
do not independently prove that an omitted resource extension caused the failures.

An intermediate matrix passed all 60 WARP configurations but rejected the 120
hardware configurations because the fixture accidentally requested a WARP helper
for hardware reference devices. The adapter-identity check correctly rejected
that mismatch. The fixture now passes the actual requested WARP flag; it does not
fall back to another GPU. The next 180-configuration matrix passed 3,600 register-
coordinate comparisons before the immediate-coordinate expansion.

These intermediate reports and logs remain in ignored `texture-precision-*`
artifacts. They are separate from final validation; no existing golden or skip
rule was changed.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcTextureLoadTests FloraDxbcExecutionTests FloraDxbcPixelSamplingTests FloraDxbcSampleLevelTests FloraDxbcSampleGradTests FloraDxbcSampleComparisonTests FloraDxbcExecutionTraceTests FloraDxbcExecutionTraceReaderTests FloraNativePixelDebugTests FloraNativeComputeDebugTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
$env:FLORA_TEXTURE_PRECISION_EVIDENCE = "$PWD/artifacts/dx11-native/texture-precision-rerun"
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^(dxbc_(texture_load|execution|pixel_sampling|sample_level|sample_grad|sample_comparison|execution_trace|execution_trace_reader)|native_(pixel_debug|compute_debug))$'
python tools/validate_texture_precision.py artifacts/dx11-native/texture-precision-rerun --test-log build/vs2022/dxbc-texture-load-Release.txt --out <new-ledger.json>
```

GPU tests must run serially and evidence output must use a fresh directory.
Minimum-precision multisample loads, texture UAV instructions, other remaining
execution semantics and final P5/P6 parity are not established by this change.
P1 engine recovery and P7/P8 removal/package gates remain required.

## Final validation

The Release GUI, CLI, Worker and ten affected test targets build successfully.
The serial CTest run passes all ten suites in 1,191.16 seconds: 3,766 Qt checks,
zero failures and zero skips. The final precision evidence covers 180 fixtures
and 7,200 direct GPU comparisons, including immediate coordinates. Independent
validation rejects all 54 corrupted reports.

| Suite | Passing checks | Elapsed ms |
| --- | ---: | ---: |
| dxbc_pixel_sampling | 186 | 428595 |
| dxbc_sample_comparison | 190 | 124878 |
| dxbc_sample_grad | 262 | 87532 |
| dxbc_sample_level | 156 | 85723 |
| dxbc_texture_load | 1625 | 325207 |
| dxbc_execution_trace_reader | 64 | 423 |
| dxbc_execution_trace | 21 | 807 |
| dxbc_execution | 1175 | 104392 |
| native_pixel_debug | 22 | 20587 |
| native_compute_debug | 65 | 12167 |

The ledger `dx11-native-texture-precision-evidence.json` pins the precision
reports, final validation, copied terminal test logs, build log, source files,
thirteen binaries and retained development failures. Raw shader reports stay
outside Git. The build retains existing C4838 and C4457 warnings in the shared
texture helper. No test timeout, golden, assertion or skip scope was relaxed.

These elapsed times describe this run, not a controlled performance or memory
growth acceptance. They do not establish full debugger parity, original captured
PS invocation qualification, removal of RenderDoc, or standalone package
acceptance. All remaining phase gates still apply.
