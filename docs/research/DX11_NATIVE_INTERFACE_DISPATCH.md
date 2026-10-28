# Original DXBC interface dispatch observations

This research identifies a native execution path for DXBC class interfaces and
an unresolved original Intel GPU discrepancy. It does not close P6, qualify all
class resources, or remove the remaining runtime dependencies. No original GPA
debugger engine is recovered by these probes; the P1 host `+0x3c0` investigation
remains separate.

## Independent experiment

`FloraDxbcInterfaceProbe` links only the Windows D3D11, DXGI and shader compiler
libraries, plus the standard runtime. It does not link the project executor,
replay library, GPA or RenderDoc. It compiles the original HLSL, records its DXBC
and disassembly, creates real class instances and dispatches the original shader.
The output is seeded with `0xdeadbeef`, completion has a five-second deadline,
and D3D11 validation errors fail the probe. A process has a separate 30-second
deadline. GPU probes run serially on the explicitly selected adapter.

The six fixtures vary static/dynamic interface indexing, interface array length,
UAV slot and feature level. Named instances use `GetClassInstance`; created
instances use explicit constant-buffer offsets. Two classes implement distinct
integer arithmetic, and the array fixture has two call sites. Expected values
are computed directly from that authored arithmetic, independently of DXBC
interpretation. The single-interface source is the existing `classProgram`
fixture from main `3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28`.

| Adapter | Device | LUID | Successful probes | Failing probes |
| --- | --- | --- | --- | --- |
| WARP, vendor 5140 | 140 | 74655 | All six | None |
| NVIDIA, vendor 4318 | 11352 | 69256 | All six | None |
| Intel, vendor 32902 | 32103 | 74756 | `array`, `single-dynamic` | `static-array`, `single`, `single-u3`, `single-11_0` |

The final matrix contains 18 probes and 48 binding configurations. Of 192 output
words, 152 match authored arithmetic and 40 differ. All reports have empty D3D11
diagnostic queues. The single-interface Intel cases leave the seeded output
unchanged. Intel's static array writes different values; for mode zero it returns
`[50, 52, 50, 52]` instead of `[110, 112, 114, 116]`. Dynamic indexing controls
pass on the same adapter. Changing UAV slot 0 to 3 or requesting feature level
11.0 instead of 11.1 does not resolve the single-interface discrepancy.

These observations localize the discrepancy to original static interface
execution in this environment. They do not identify a driver root cause or prove
that every static interface shader fails. They are not GPU fallback permission,
an invented native result, a skipped test or an automatic P0 failure exemption.
The remaining mismatch must stay visible in the capability matrix and release
gate. No existing assertion or golden is changed by this research commit.

## Binding and instruction evidence

The independent compiler/disassembler emits function-body declarations (opcode
144), function tables (145), interface declarations (146), interface calls (120),
function-body labels (operand kind 17), interface operands (19), and `this[]`
(29). Function tables select a body for each call site. Interface arrays occupy
consecutive binding slots. `this[]` exposes a constant-buffer slot, constant-vector
offset, texture base and sampler base; these are read-only inputs to execution.
These semantics are specified in Microsoft's
[D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 7.19, 22.3.50–51 and 22.7.20.

Observed named-instance descriptors do not contain resolved resource bases and
have empty type names. The shader IFCE chunk and instance name/index are needed
to identify the shader-local type, function table and constant-vector offset.
Created-instance descriptors carry explicit bases and a type name. Their runtime
type ID must not be assumed to equal a different shader's local type index. See
[D3D11_CLASS_INSTANCE_DESC](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_class_instance_desc).

For the array fixture, IFCE's header contains counts for instances, types, slot
records and slots, followed by offsets for the corresponding records. The
observed instance records are 16 bytes, type records 12 bytes and slot records
16 bytes. A slot record points to 16-bit type indices and 32-bit function-table
indices. The named arrays resolve to constant buffer 1 with vector offsets 0
and 2, and a per-element stride of one vector. The independently disassembled
metadata agrees with these records and with original GPU arithmetic on the
passing configurations. Unused texture/sampler bases are `0xffff` in the metadata;
they must remain unknown rather than being filled from zero descriptor fields.

The current observations do not qualify class texture/sampler members, larger
constant layouts, multiple independent interface declarations, all shader stages,
or recursively composed class layouts. These require additional probes before a
general metadata parser and resource execution path can be called complete.

## Development results and unresolved checks

The working native implementation executes the array fixture on all three
adapters, reproducing 48 original GPU result words with eight calls and eight
returns per configuration. It preserves unresolved binding errors and detects
malformed interface tables, labels, indices and potential recursion. Named and
created GPA snapshots retain object 62 and independently resolved class bases.
WARP and NVIDIA captured trace checks pass, including reversible register/storage
state and unchanged original replay storage. The two Intel captured comparisons
remain failed against the existing single-interface shader. They are not removed
or relaxed. Product changes remain distinct from this research commit.
The final interface suite reports 21 passes and two failures, with no skips.
Seven existing execution, memory, group, input, journal, reader and debug-model
suites pass 1,796 checks without failures or skips (151.68 seconds). GUI, CLI and
Worker builds also pass. These regression results do not override the two new
failed comparisons or qualify a standalone release.

Retained development failures include the compiler's rejection of varying
interface indexing (`X4603`), a test compile error caused by treating a read-only
DXBC span as a vector, and an initial reference test that omitted `Replay::run`.
After correcting the reference invocation, WARP/NVIDIA comparisons pass while
the two Intel differences remain and reproduce without project code. None of
these fixes changes authored expected arithmetic.

## Reproduction and evidence

Build the `FloraDxbcInterfaceProbe` Release target, then run from the repository:

```powershell
tools/native/Invoke-DxbcInterfaceResearch.ps1 `
  -Probe build/vs2022/Release/FloraDxbcInterfaceProbe.exe `
  -OutputDirectory artifacts/dx11-native/interfaces-research-new
python tools/validate_native_interfaces.py artifacts/dx11-native/interfaces-research-new
```

Both commands return nonzero while arithmetic mismatches exist. The validator
distinguishes valid evidence from passing acceptance, checks all 108 evidence
file digests and the complete control matrix, and rejects 90 deliberate report
corruptions. The checked evidence reports `evidence_valid: true` and
`acceptance_passed: false`. Original binaries, shader outputs and synthetic GPA
captures remain outside Git. The companion JSON pins the probe, source,
validator, scripts and retained evidence identities.
