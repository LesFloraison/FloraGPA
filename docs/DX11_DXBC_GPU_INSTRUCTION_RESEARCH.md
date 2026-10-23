# Native GPU assistance for DXBC instruction differences

Date: 2026-10-24. The original scalar/control suite passes all 374 cases after
adding a same-adapter VS instruction assist. All 17 previously failing cases now
match their independent GPU probes with unchanged exact assertions. This is a
bounded P6 execution-core milestone, not complete shader-debugger acceptance.

## Observed differences and execution strategy

The earlier CPU-only results remain in `DX11_DXBC_EXECUTION_RESEARCH.md` and its
evidence ledger. The failures depend on immediate versus constant-buffer operands,
destination aliases, and the selected adapter. Replacing them with vendor-specific
constants would not establish the original shader's result.

`GpuOperations` extends the private device previously named `GpuMemory`. It retains
the explicit adapter LUID, feature level and WARP selection and does not use the
reference device's immediate context. Its identity version is `dxbc-gpu/2`.
The CPU lane continues to own control flow, register addressing, validity,
instruction order, call stacks and reversible trace changes.

For immediate ftoi, ftou, f16tof32 and immediate usubb, and supported aliased
multi-result operations, the helper constructs a small VS containing the original
instruction. Immediate operands and opcode controls remain intact. Resolved
non-immediate operands are supplied through a private constant buffer. Destination
register kind and alias relationships are preserved; indices are canonicalized.
Components not written by the instruction are initialized from the preceding
register state. Unknown source information remains unknown in the CPU result.

The helper executes the VS on its private context and reads raw bits through
stream output, with no captured UAV slot requirement. Distinct temporary-register
results are captured in separate passes so each selected result has the same
consumption shape as the independent probe. Aliased destinations share one merged
capture. Each CPU step exposes `instructionGpuAssisted`; the helper counts actual
successful GPU passes, which can be one or two per assisted step.

Preserving the shader stage matters. This instruction helper currently accepts VS
only and rejects other stages explicitly; the existing buffer helper executes CS.
Neither path establishes cross-stage equivalence. The implementation contains no
RenderDoc interpreter code and loads no Intel GPA implementation.
The instruction probes and generated helpers use VS 5.0 with a uint4 output
signature. Other shader-model lowering and output-signature combinations are not
validated by these results.

## Isolation, failure and validity

The helper uses the same cancellation, bounded completion polling and poisoned
device policy as the buffer assist. Failed operations do not commit CPU register
changes. A replacement helper is required after a failed GPU pass. Driver calls
themselves are not guaranteed interruptible; production Worker isolation remains
necessary. A separate cache holds at most 256 VS/stream-output program pairs.

Only temporary and ordinary output destinations are implemented, with at most one
distinct output register. Other destination forms fail explicitly. Full PS/CS
execution, indexable destination assistance, null destinations and feature-level
coverage remain work items. These restrictions apply to the new optional prototype,
which has not replaced any production debugger action.

The initial exact comparison run passed 374 cases, with zero failures or skips,
in 58.84 seconds of CTest time. It retained every prior raw instruction, scalar
and vector immediate, constant-buffer, alias and HLSL case. No golden value,
tolerance, expected failure or skip was changed. Both comparison paths select
WARP, NVIDIA or Intel explicitly, and the implementation uses a separate device
from the full-shader reference probe.

Additional lifecycle validation checks reference-context constant-buffer bindings,
reverses register changes including validity, cancels a GPU completion poll,
rejects reuse after cancellation and succeeds after helper recreation. Unknown
input components remain unknown, and overlapping destinations are rejected before
submitting GPU work. Final totals and source/log hashes are recorded in
`research/dx11-dxbc-gpu-instruction-evidence.json`.

Final serial CTest results:

| Suite | Passed | Failed | Skipped | CTest duration |
| --- | ---: | ---: | ---: | ---: |
| dxbc_execution | 375 | 0 | 0 | 62.87 s |
| dxbc_memory | 246 | 0 | 0 | 48.28 s |

The combined run passed in 111.18 seconds. Raw instruction cases recorded 129
successful GPU passes across the three adapters; that count excludes the separate
lifecycle test. All 17 original failed identities, including operand form, are
recorded as resolved. The original CPU-only and intermediate failed runs remain
in the same ledger.

The first expanded run retained one new lifecycle failure: an overlapping
destination request was rejected only after an unnecessary GPU pass. The source
already contained an earlier validation, but its edit overlapped an active
incremental compile. Rebuilding that translation unit and removing the redundant
late validation made the focused lifecycle test pass. The failed run is retained,
and full-suite validation is repeated against the rebuilt executable; the test
assertion is unchanged. Compilation and source mutation must remain sequential.

## Limits and remaining evidence

An isolated instruction matching these probes does not prove that every compiler
context or longer shader will produce the same machine-level result. The original
GPU observations are not a recovered compiler implementation or a general proof
of hardware correctness. More shader contexts, stages and input sets are needed.
The comparison with CPU-only timings is also not a real-capture performance claim:
the expanded suite creates additional devices and performs additional submissions.

The runtime still needs actual VS inputs, PS quad/helper/sample semantics, CS group
coordination, typed resources, sampling and source/UI integration. Full Pixel
History, the GPA debugger implementation path and P7/P8 removal/release gates
remain open. No existing feature has been removed to obtain these results.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcExecutionTests FloraDxbcMemoryTests
ctest --test-dir build/vs2022 -C Release -j 1 `
  -R '^dxbc_(execution|memory)$' --output-on-failure
python tools/summarize_dxbc_execution.py `
  --log artifacts/dx11-native/dxbc-execution-after-gpu-memory.txt `
  --log build/vs2022/dxbc-execution-Release.txt `
  --source src/application/DxbcExecution.cpp `
  --source src/application/DxbcGpuOperations.cpp `
  --source tests/DxbcExecutionTests.cpp `
  --scope native_dxbc_gpu_instruction_research `
  --out artifacts/dx11-native/dxbc-gpu-instruction-current.json
```

The summarizer matches the original operand form as part of each failure identity;
a passing dynamic-input form cannot close a failed immediate form. Raw logs and
disassemblies remain outside Git under ignored artifacts. The small evidence
ledger preserves their hashes and complete prior mismatch vectors.
