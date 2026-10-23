# Native DXBC execution research checkpoint

Date: 2026-10-24. This is an unfinished P6 implementation checkpoint. The strict
GPU conformance test is failing and remains enabled. It does not close debugger
parity, authorize a GUI cutover, or establish standalone release acceptance.

## Implementation boundary

`DxbcExecution` is an independent C++ single-lane instruction executor. It uses
the project's DXBC container reader, public-format operand counts and existing
static call-graph validation. No external interpreter implementation was copied.
The input is original SM4/SM5 bytecode plus registers and constant-buffer values.
The output is one bounded execution step at a time: original word offset,
instruction identity, return stack, next instruction and register changes with
raw 32-bit component bits and independent validity masks. The caller owns trace
storage and cancellation; a step budget bounds loops.

Current operations include integer arithmetic and comparisons, shifts and bit
fields, floating arithmetic and conversion, component selection, dynamic array
indices, branches, loops, switch, calls and returns. Unknown data stays unknown.
An unknown branch or relative address fails before changing execution state.
Declared bound constant buffers have explicit lengths; an out-of-range read is
zero, while a missing binding is unknown. Multi-result sources and destination
addresses are evaluated before writeback. Source swizzles, masks and modifiers
are independent of the result's raw bit storage. The thread's floating-point
environment is restored after each instruction.

This is not a complete VS/PS/CS debugger. Typed resource access, texture sampling,
derivatives, quad/helper execution, groupshared memory, barriers, atomics, doubles,
minimum precision, actual input extraction, shader linkage, source-trace adapters
and GUI workflows remain unfinished. Single-lane discard is not a helper-lane
implementation. Scalar transcendental evaluation is not a guarantee of identical
device approximations. The core currently implements documented instruction
semantics, not arbitrary machine-code optimizations across original instructions.
It is not connected to production debug actions. Existing debug/checkpoint paths
remain available. GPA's concrete debugger-engine object remains unresolved.

Follow-up: private raw/structured buffer loads and stores are now implemented as
described in `DX11_DXBC_MEMORY_RESEARCH.md`. Typed resources and GPU-assisted
boundary behavior remain unfinished; this does not change the original execution
probe's acceptance status or replace its retained evidence.

## Independent probes and corrections

The test creates an explicitly selected WARP, NVIDIA or Intel D3D11 debug device,
runs a VS and captures its four output components through stream output. There is
no adapter fallback. Compiler-generated HLSL covers arithmetic, control flow,
dynamic arrays and reversible register histories. Raw instruction probes cover
operations and boundary inputs that FXC can fold away. Each raw program is also
disassembled by D3DDisassemble. Device identities, driver versions, instructions
and all CPU/GPU output components are recorded. D3D11 errors fail the test;
warnings are logged. The final run contains neither validation errors nor warnings.

The initial compiler-driven run passed 150 cases. Adding raw probes exposed both
implementation errors and limitations of the first probe. Corrections were based
on the [D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm):

- Sections 3.2.2 and 22.13 require float-format conversion toward zero, with finite
  overflow clamped to the largest finite half value. The original CPU conversion
  incorrectly used nearest-even rounding.
- Section 22.9.3 defines swapc's selection direction and requires distinct
  destination registers. The executor's original selection was reversed and its
  first same-register probe was invalid. The replacement tests both complete
  results in separate registers; a CPU rejection test retains the invalid case.
- Section 22.11.3 defines firstbit_hi as a leading-zero count. All three devices
  return 3 for 0x10000000. The [Learn instruction page](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/firstbit--sm5---asm-)
  instead describes 28 for that example. The conflicting documentation is not
  treated as an execution oracle.

Other same-register, disjoint-mask probes have not been dismissed as invalid.
They remain strict comparisons, alongside separate-register probes. The legality
and native lowering of their combinations require additional evidence.

## Open GPU discrepancies

For the separate-register probes, every dynamic constant-buffer input agrees with
the CPU result. Five operation/device combinations disagree for immediate inputs,
both with one vector immediate instruction and with separate scalar instructions:

| Device | Operation | Observed difference |
| --- | --- | --- |
| WARP | ftou | Immediate 2^31 produces 0xfffffffe instead of 0x80000000 |
| NVIDIA | f16tof32 | Immediate half values produce zeros; equivalent CB inputs preserve negative zero, subnormals and the largest finite value |
| NVIDIA | usubb | Immediate zero minus zero reports borrow 1; equivalent CB inputs report 0 |
| Intel | ftoi | Immediate infinity/large-positive inputs produce zero; equivalent CB inputs clamp to the signed range |
| Intel | ftou | Immediate positive infinity produces zero; equivalent CB input produces 0xffffffff |

These are observations, not a proven assignment of fault to a specific driver
component. The source-form dependence suggests constant evaluation or lowering,
but the current evidence does not recover the generated machine code. The GPU
comparison is independent of the CPU executor and is not automatically accepted
as ground truth. No vendor-specific fabricated value, relaxed tolerance, skip or
expected-failure waiver was added. Integer and raw-bit comparisons remain exact.
The additional alias probes and all unresolved rows are retained in the evidence
ledger, including complete output vectors rather than only the first mismatch.
The final expanded execution run has **357 passing, 17 failing and zero skipped
cases**. Ten failures are the five separate-register discrepancies in two literal
forms. Seven additional failures cover NVIDIA's immediate alias usubb and Intel's
alias imul, umul and uaddc with both literal and constant-buffer sources. The latter
three show that immediate lowering alone cannot explain every remaining failure.

## Reproduction and acceptance

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcExecutionTests FloraDxbcFormatTests FloraCheckpointTests FloraDxbcLogTests
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(dxbc_execution|dxbc_format|dxbc_output_log|checkpoint_model)$' --output-on-failure
python tools/summarize_dxbc_execution.py `
  --log build/vs2022/dxbc-execution-Release.txt `
  --source src/application/DxbcExecution.h `
  --source src/application/DxbcExecution.cpp `
  --source tests/DxbcExecutionTests.cpp `
  --out artifacts/dx11-native/dxbc-execution-current.json
```

The current execution test and evidence summarizer return failure. The other
three CTest suites passed: DXBC format, checkpoint and output log. An added CPU
test verifies unknown-address rollback, pre-state
multi-result addressing, missing versus out-of-range constants, malformed validity,
invalid swap destinations and recursive-call rejection. Half-conversion boundaries
include signed zero, subnormals, ties, finite overflow, infinities and NaN.

`research/dx11-dxbc-execution-evidence.json` records completed run totals, original
failure cases, output differences, driver identity and file hashes. Raw logs and
large disassemblies stay under ignored artifacts. The first raw run is preserved,
including the invalid swap probe; its 14 failures are not relabeled as historical
main failures. No existing golden or baseline exception has changed. Further
native hardware-assisted semantics and end-to-end debugging remain P6 work, and
the active conformance failures must be resolved before P6 acceptance.
