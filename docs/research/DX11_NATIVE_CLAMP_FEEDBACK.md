# Native per-sample LOD clamp and feedback

## Scope

The independent executor now routes `sample_cl_s`, `sample_b_cl_s`,
`sample_d_cl_s` and `sample_c_cl_s` through its existing sampling paths while
retaining the additional per-sample clamp. This extends the
[texture feedback work](DX11_NATIVE_TEXTURE_FEEDBACK.md). It does not complete
native debugger parity or authorize RenderDoc removal.

The [Microsoft D3D11.3 specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 5.9.4.5.4 and 5.9.7, defines the clamp in SRV mip space and assigns these
shader operations to tiled-resource tier 2, with feature level 11.1 minimum.
The implementation sends the actual clamp operand to the GPU instruction;
it does not substitute sampler state or select another device.

## Execution and provenance

Pixel quad requests retain independent clamp values for all four lanes.
Compute gradient requests carry per-thread clamps through the existing cohort
batch, without changing thread indices or the group schedule. Unknown clamp
values leave dependent data unknown. Unknown source mapping provenance continues
to produce opaque unknown status; known non-tiled resources produce semantic
mapped status without invented hardware status bits.

The private GPU device inherits the reference device's adapter and feature level.
Before issuing a clamp instruction it verifies feature level 11.1 and tier 2.
The negative matrix explicitly requests feature level 11.0 and requires rejection
before any GPU operation. These are passing rejection tests, not skipped cases.

Unmodified scalar 32-bit immediate bias/reference operands in clamp instructions
are preserved in the implicit sampling helper. Its shader key includes their presence and raw bits, along with
whether the instruction has a clamp. A second execution on the same helper uses
a dynamic parameter to check that the cache does not conflate these forms.
GPU helper identity advances to `dxbc-gpu/29`; analysis identity advances to
`dx11-native/6`. The resource snapshot schema remains unchanged.

## Compiler framing

The compiled fixtures use the Microsoft metadata operand counts 6, 7, 8 and 7.
Some contain one extra zero DWORD; the tested gradient fixtures contain one extra
`0x2000` DWORD. The pinned Microsoft parser described in the
[DXBC format study](../DX11_DXBC_FORMAT_RESEARCH.md) reads the declared operands
and advances to the declared instruction boundary.

The shared compatibility parser accepts only the observed one-word trailers,
with `0x2000` restricted to the gradient-feedback opcode. It verifies complete
operand framing and the SRV/sampler operand kinds. Negative tests reject a
nonmatching trailer or an extra word. An independently rebuilt shader removes
the trailer and preserves every other instruction word; hardware execution
compares that shader with the original. No arbitrary instruction tail is ignored.

## Intel immediate-bias conflict

The first full matrix found eight native/original differences, all for Intel
pixel `SampleBias` with immediate bias zero. Three independent GPU programs
isolate the cause:

1. The original immediate-bias shader ignores changes to the per-sample clamp.
2. Removing its trailer leaves the same output.
3. Replacing the immediate bias with a runtime parameter holding the same zero
   applies the clamp. WARP and NVIDIA apply it in both forms.

For a three-mip texture containing 0.25, 0.5 and 1.0, clamps 0, 1 and 2 produce
0.25, 0.25 and 0.25 in the Intel immediate form, and 0.25, 0.5 and 1.0 in its
dynamic form. The same adapter, sampler, texture and numeric bias are used.

Preserving the immediate operand makes the native result agree with the original
GPU shader. That agreement does not establish specification conformance: the
separate clamp-effect assertion still fails. Both the C++ suite and independent
Python verifier keep this failure active. There is no vendor skip, changed golden
or acceptance waiver. The driver-dependent semantic discrepancy remains an open
release gate.

## Reproduction

Build `FloraDxbcClampFeedbackTests` in Release and run GPU suites serially. Set
`FLORA_CLAMP_FEEDBACK_EVIDENCE` to a new directory. The suite includes six parser
cases and a 162-case execution/capability matrix spanning WARP, NVIDIA and Intel;
implicit, bias, gradient and comparison sampling; PS, VS and CS where applicable;
SRV mip offsets, sampler maximum LOD, resource minimum LOD, fractional clamps,
non-finite values, unknown clamp values and unknown mapping provenance.

Each supported case records four source shaders: original feedback, ordinary
unclamped sampling, trailer-removed feedback and dynamic-parameter feedback.
The compute oracle reads all four outputs from the original four-thread group.
Pixel and vertex oracles independently execute each selected parameter value.
Fixture coordinates and gradients are constant; this does not qualify arbitrary
captured quads or divergent implicit footprints.

```text
python tools/validate_native_clamp_feedback.py --root <evidence-directory> --out <report.json>
```

The verifier checks the complete matrix, shader identities, trailer-only binary
changes, native/original data, validity, cohort counts and rejection behavior.
It separately computes expectations for the first three point-sampled mips and
returns a failure status when they conflict with the original shader.
Intermediate failed builds and probes remain in ignored local artifacts.

## Recorded results

Implementation commit `d8354e56ef45508743d1aaf6c2e0bd51dc67b3a5` is bound to
the [evidence manifest](dx11-native-clamp-feedback-evidence.json), containing
2,155 local artifact hashes and 14 committed source-file hashes.

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Clamp feedback | 169 | 1 | 0 |
| Existing texture feedback | 134 | 0 | 0 |
| DXBC execution | 1,212 | 0 | 0 |
| DXBC format | 16 | 0 | 0 |
| Sampling groups | 296 | 0 | 0 |
| Pixel sampling | 243 | 0 | 0 |
| Native compute debug | 68 | 0 | 0 |
| Native analysis | 46 | 0 | 0 |
| Total | 2,184 | 1 | 0 |

The independent verifier checks all 162 execution/capability cases and returns
exit code 1 for the Intel conflict. It confirms all valid native values against
the original shader, all original/trailer-removed comparisons, and the
literal/dynamic cache controls. These comparisons do not override the failing
semantic expectation. All 18 capability rejections occur before GPU execution.

The GUI, CLI and Worker Release builds succeeded. The initial Worker regression
run had three stale `dx11-native/5` version assertions; updating them to the new
`dx11-native/6` contract restored all 68 tests without changing data assertions.
These are incremental development builds, not standalone release qualification.

## Remaining work

The Intel conflict above remains open. Full texture-dimension/filter coverage,
empty-set specification behavior, remaining precision/operand forms, tiled
residency, captured invocation selection and complete debugger UI/workflow parity
are not established by these probes. The earlier NVIDIA buffer-feedback conflicts
also remain open. The GPA debugger engine provenance, full Pixel History,
geometry parity and independent release audit still block final acceptance.
