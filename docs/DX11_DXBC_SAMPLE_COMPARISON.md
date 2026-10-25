# Native DXBC comparison sampling

The independent VS/PS/CS executor implements `sample_c_lz` (opcode 71) with a
private D3D11 shader containing the same comparison instruction. It supplies the
original coordinate and reference bits, resource/view snapshot, sampler state,
immediate offsets and destination mask on the selected adapter. The GPU performs
comparison and filtering in their native order. No CPU substitution of a
filtered texture value or implicit switch of adapter is used.

The parser requires a comparison sampler declaration, a scalar reference operand
and replicated red resource selection. Source reference negation/absolute-value
modifiers use the existing register semantics. Resource return declarations and
instruction extensions retain their validation. The helper checks native
comparison-format support and uses a comparison-mode sampler declaration.
The GPU helper identity is `dxbc-gpu/13`; the compute scheduler stays
`bounded-sampling-rendezvous/3` and input snapshots stay at version 7.

Unknown binding information, required coordinate/reference components or stored
texture bytes produce unknown results. A verified null resource produces zero
when the comparison sampler is valid. A verified null sampler or a mismatch
between comparison/default sampler modes is an explicit error, including when
the resource is null. This follows the independent debug-layer observation in
[DX11_SAMPLE_COMPARISON_RESEARCH.md](DX11_SAMPLE_COMPARISON_RESEARCH.md).
Comparison sampling of 3D resources and gradient operands on this fixed-LOD
instruction are rejected. Existing noncomparison null-resource behavior is
preserved.

All diagnostics operate on private resources. A cancelled GPU readback commits
no lane register changes, invalidates the helper, and requires a new helper for
retry. Original texture bytes and their validity are unchanged. Ordinary masked
writes preserve unaffected register components, including their raw bits.

## Validation and limits

`FloraDxbcSampleComparisonTests` launches fresh independent batch and individual
invocation probes in VS, PS and CS on WARP, NVIDIA and Intel. Every defined
reference vector must agree between invocation modes. The native interpreter
then compares 30,618 per-stage raw vectors exactly. It additionally executes
162 complete compiler-generated compute programs, checking full output bytes
and validity plus unchanged input storage. Eighteen undefined-sampler requests
are rejected; their observed driver values are not used as numerical oracles.

The separate semantic cases cover reference modifiers, three destination masks,
ignored unknown components, unknown bindings/storage, invalid requests and nine
post-submission cancellation/retry paths. The required regressions include
explicit LOD, explicit gradients, divergent sampling groups, cooperative groups
and scalar execution. All GPU tests run serially.

The comparison-format matrix here is R32_FLOAT Texture2D with mip/view/resource
LOD controls. Depth-compatible typeless formats, additional texture shapes and
captured comparison shaders require further independent evidence. Implicit
comparison sampling (`sample_c`), implicit ordinary sampling, pixel quads/helper
lanes, broader VS/PS input extraction, complete trace/source/UI integration and
full release parity remain open. This checkpoint cannot close those gates.

Build `FloraDxbcSampleComparisonTests` and the five regression targets, then run
CTest names `dxbc_sample_comparison`, `dxbc_sample_level`, `dxbc_sample_grad`,
`dxbc_sampling_group`, `dxbc_group` and `dxbc_execution` with `-j 1`.
`tools/summarize_dxbc_sample_comparison.py` requires the complete matrix, all
semantic cases and all five regressions. Earlier sampling logs can be validated
with their original helper identity using `--backend dxbc-gpu/12` in the explicit
sampling and sampling-group validators.

The final six-suite run passes 1,317 tests with zero failures/skips in 521.05
seconds of CTest wall time. The comparison suite itself passes 190 tests in
207.158 seconds of Qt test time. GUI, CLI, Worker and the six test targets build
in Release. The evidence ledger is
`research/dx11-dxbc-sample-comparison-evidence.json`; reproducible logs and strict
adjacent capability checks are in `artifacts/dx11-native/sample-comparison-*`.
Negative evidence tests reject reduced comparison counts, missing complete
groups, accepted undefined samplers, missing configurations and omitted
cancellation results. Earlier probe/implementation evidence is preserved.
