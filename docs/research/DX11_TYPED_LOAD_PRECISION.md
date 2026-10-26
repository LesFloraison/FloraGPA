# Minimum precision in native typed-buffer reads

The native executor now carries typed-buffer destination and address precision
through VS, PS and CS SRV reads and CS typed-UAV reads. This removes the program
construction guard at Helldivers event 200, opcode 45, word 1853. The captured
shader still fails at the separate minimum-precision PS output restriction.
P6 parity and standalone release acceptance remain incomplete.

## Implementation and observed semantics

`dxbc-gpu/19` preserves float16, float2.8, signed16 and unsigned16 destination
annotations, and signed16/unsigned16 address annotations. It reads private
captured storage on the selected adapter. The integer index is observed after
GPU precision conversion before byte validity and out-of-view handling are
computed. Unknown bytes remain unknown; an unavailable address is rejected.
Cancellation before submission can be retried without changing captured bytes.

The typed instruction includes resource dimension and return-class extensions.
Its temporary result is widened using the same source precision. Raw/structured
CS stores can consume already widened temporary-register bits without a second
narrowing operation. This does not enable minimum precision on their resource
or address operands, or minimum-precision constants as their stored value.
Minimum-precision texture loads, typed stores and PS output destinations remain
separate requirements.

The original shader's `refactoringAllowed` flag is retained for typed loads.
On the NVIDIA adapter, omitting it from the isolated helper produced half-rounded
float16 load results while the original enabled shader retained full float bits.
Preserving the permission fixed the comparison. Both enabled and disabled
fixtures now pass; the implementation does not universally enable the flag.

The [D3D11.3 specification, sections 7.20.3.3-7.20.3.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
distinguishes stored source precision, operation/destination precision, address
precision and 32-bit constant storage. It permits implementations to exceed the
declared minimum and leaves out-of-range minimum-precision constant references
undefined. Accordingly, the large-index fixtures below are adapter-specific
observations, not a portable overflow or narrowing specification. They do not
establish equivalence for every shader optimization context.

## Independent reference and retained attempts

The reference executes FXC shaders directly through public D3D11 draws or
dispatches, then reads their integer targets, SO buffers or structured outputs.
It does not invoke the native interpreter, GPA or RenderDoc. A separate native
lane executes the same bytecode against private storage. Both paths record their
actual adapter identity. Compiler bytecode is retained separately; the only
fixture alteration is the explicit refactoring flag. The Python validator checks
that every other instruction and offset is unchanged.

The precision matrix contains 1,152 rows and five addresses per row: 0, 2, 3,
65,536 and UINT_MAX. It combines three adapters, four load paths, four destination
precisions, three address precisions, literal/runtime index seeds, full/partial
output masks and both refactoring permissions. SRV views use four-component
R32 formats; typed UAV views use mandatory R32 scalar formats. Views start at
element one and contain three elements. Literal seeds still participate in a
runtime arithmetic expression; these are not literal load-address operands.

Earlier attempts are retained outside Git. They include compiler-folded address
annotations, missing structured-store support, and the NVIDIA rounding mismatch.
Changing the fixture to multiply its runtime zero addend retained the requested
address precision. Adding resource extensions alone did not resolve the rounding
mismatch; preserving refactoring permission did. No existing golden, assertion
or skip rule was weakened.

## Validation and remaining gates

The complete typed-pixel suite passes 1,732 checks in 246.851 seconds, with no
failures or skips. The later targeted minimum-precision validity/cancellation
check also passes. Existing typed-buffer, typed-VS and execution suites pass
473, 580 and 445 checks respectively, again with no failures or skips. GUI, CLI
and Worker Release builds pass. These timings are test durations, not controlled
P8 performance comparisons.

Saved native and reference results match for all 5,760 cases and 23,040 output
components. Independent validation rejects 72 altered reports. The unchanged
default-precision matrix remains in the complete typed-pixel suite.

Six GF2 event 113 and BF1 event 2010 traces complete and rewind on NVIDIA, Intel
and WARP. Their 381 selected and 1,524 global steps pass C++ projection and
Python validation, including 120 corruption checks. These checks establish
private input provenance and reversible execution records, not original fragment
eligibility or independent complete-game PS output equivalence.

All three Helldivers runs now fail with
`Minimum-precision output requires a single VS destination`. No partial trace is
published. Full shader completion, PS output precision, helper memory semantics,
fragment qualification, public PS jobs/sessions, source/UI parity, full Pixel
History, dependency removal and independent packaging remain open.

The evidence ledger is
[dx11-typed-load-precision-evidence.json](dx11-typed-load-precision-evidence.json).
Raw binaries, test outputs and captured data remain outside Git.

## Reproduction

Build `FloraDxbcTypedPixelTests`, `FloraDxbcTypedBufferTests`,
`FloraDxbcTypedVertexTests`, `FloraDxbcExecutionTests`,
`FloraDxbcPixelGeometryProbe` and `FloraDxbcTraceReaderProbe` in Release.
Run GPU tests serially with Qt on `PATH`:

```powershell
$env:FLORA_KEEP_TYPED_PRECISION_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcTypedPixelTests.exe -o typed-pixel.txt,txt
build/vs2022/Release/FloraDxbcTypedBufferTests.exe -o typed-buffer.txt,txt
build/vs2022/Release/FloraDxbcTypedVertexTests.exe -o typed-vertex.txt,txt
build/vs2022/Release/FloraDxbcExecutionTests.exe -o execution.txt,txt
python tools/validate_typed_load_precision.py <saved-evidence-directory> --out <new-report.json>
```

The first log prints its retained evidence directory. For real frames, use the
existing pixel geometry probe with `centroid trace`, the trace reader with the
saved shader, and `tools/validate_pixel_execution_trace.py` on all nine output
directories. Probe failures must remain failures in the report.
