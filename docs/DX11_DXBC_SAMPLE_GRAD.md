# Native DXBC explicit-gradient sampling

The independent VS/PS/CS executor routes `sample_d` to a native D3D11 instruction
on the selected adapter, preserving both normalized-coordinate derivative
vectors. It does not reduce them to a CPU scalar LOD. Source selection/modifiers,
raw result bits, destination masks, view ranges, resource MinLOD and observed
sampler state remain part of the operation. See
[DX11_SAMPLE_GRAD_RESEARCH.md](DX11_SAMPLE_GRAD_RESEARCH.md) for independent
adapter, stage and invocation observations.

## Invocation context and scheduling

Single-lane assistance remains a single invocation in the requested stage.
Coherent CS sampling additionally has a private batch path: a structured input
buffer carries coordinates, X/Y derivatives and participation for each original
flat thread index. The helper uses the original thread-group dimensions, an
active-lane branch and one native dispatch. Inactive output records retain a
sentinel and unknown validity. No original replay resource or context is bound
for diagnostic execution.

The scheduler lets other runnable lanes reach sampling boundaries before issuing
a batch. Each batch includes only lanes at the same program, PC, call stack and
loop iteration, with the same texture/sampler selection and private resource
context. Other branches, terminated lanes and barrier waiters remain inactive
at their original flat indices. Operands are collected without advancing lanes;
all GPU results are read before any register update. Staged results take priority
over every other instruction until consumed, so no intervening write or barrier
can change the inputs. Cancellation during readback leaves every lane's
registers unchanged and rejects reuse of the failed group/helper.

Gathering is bounded by 4,096 other executed instructions. It then releases the
next ready sampling cohort even if other lanes have not arrived. This permits
progress when another thread is polling a value written after the pending
sample. The existing total step budget and per-step cancellation remain in
force. Each public step still executes exactly one real instruction. The bound
does not establish identical hardware invocation context for arbitrary long
divergence; that case remains outside the verified matrix.

The GPU helper identity is `dxbc-gpu/13`; the scheduler identity is
`bounded-sampling-rendezvous/3`. Input snapshots remain version 7. The batch validates
shared descriptors/storage once and retains per-lane operand validity. Unknown
active operands decline batching; existing scalar unknown propagation remains.
Observed null resources still produce known zero. Ignored derivative components
and array indices do not become extra required derivative dimensions.

This is a deterministic diagnostic serialization, not a reconstruction of the
captured GPU wave schedule. Six independently observed compute flow families
are checked in [DX11_SAMPLE_GROUP_RESEARCH.md](DX11_SAMPLE_GROUP_RESEARCH.md).
Arbitrary divergent loops/calls, differing dynamic bindings, unknown participating
inputs, PS quads and helper lanes still require broader context handling and
validation. Those cases cannot inherit exact parity from the verified matrix.

## Independent validation

`FloraDxbcSampleGradTests` and `FloraDxbcSampleLevelTests` compile the same fixture
in distinct modes. Both QTest logs retain the class name `DxbcSampleLevelTests`;
executable, CTest name, output path and evidence mode distinguish them. Reference
HLSL is independently compiled and executed by `FloraSampleLevelProbe`, without
using the executor, replay library, RenderDoc or GPA private modules.

The gradient fixture launches fresh single-invocation references in PS/VS/CS on
WARP, NVIDIA and Intel and compares 19,278 raw vectors exactly. It separately
retains 6,426 native CS batch observations. The original compiler-generated CS
programs execute as complete cooperative groups and their entire output buffers
must match the original batch dispatch exactly. Another 102 cases call the batch
interface directly, including the inactive 64th lane. Adapter LUIDs, sampler
state and reference invocation modes are checked explicitly.

The 42 synthetic GPA shapes cover seven texture dimensions, two formats,
restricted mips and array/cube-face starts, result masks, ignored unknown
components and equivalent swizzled/negated derivatives. Nine single-operation
and three whole-group cancellation cases test failure/recovery. The latter
verify all 64 register maps remain unchanged after post-submission cancellation.
Malformed group sizes, inactive groups, late unknown operands and null bindings
have a separate validation case. Existing group/barrier/atomic tests are also
required because the scheduler is shared.

The first full gradient run had 154 passes and two WARP anisotropic failures.
Independent follow-ups reproduced 68 differing lane records and both group
outputs under single-invocation execution. Matching only the shader stage did
not resolve that difference. The final design therefore compares single-lane
operations with independent single invocations and complete groups with the
unchanged batch oracle. Before changing the single-lane fixture, the new batch
scheduler demonstrably fixed both original full-group outputs while the original
single-lane/batch comparisons still failed. Those intermediate logs are retained.
No numeric tolerance, skip, reduced mask or replacement golden was introduced.

The standalone batch matrix passes 104 tests including setup/cleanup. The
subsequent targeted context, shape, cancellation and validation run passes 50
cases. The final complete gradient suite passes 262 tests with no failures/skips
in 370.775 seconds of Qt test time. Four adjacent suites pass another 2,014 cases:
explicit LOD (156), texture loads (1,445), groups (38) and scalar execution (375).
The five-suite total is 2,276 passes, zero failures/skips, 1,133.753 seconds of
summed Qt time and 1,135.50 seconds of CTest wall time. GUI, CLI and Worker Release
builds pass. This is not a complete P0 baseline or standalone release run.

Build both sampling targets and run `dxbc_sample_grad`, `dxbc_sample_level`,
`dxbc_texture_load`, `dxbc_group` and `dxbc_execution` serially.
`tools/summarize_dxbc_sample_level.py --grad` requires the complete lane, batch,
shape and cancellation matrices, exact full-group output, invocation modes,
backend/scheduler identities and regression results. Historical explicit-LOD
logs can be revalidated with their original `--backend dxbc-gpu/10` identity.
Earlier coherent-gradient logs additionally use `--execution-model serial-round-robin/2`.
Three negative validations reject a wrong group batch count, a wrong reference
invocation mode and a missing direct-batch record. The final ledger retains
source/build/binary hashes and the earlier failed iterations separately.

## Remaining acceptance scope

Implicit/comparison sampling, captured VS/PS invocation selection, helper lanes,
source/debugger UI integration and broader format/nonfinite-input behavior remain
open. Sampling still requires fully known private texture storage. The coherent
63-lane fixture uses one GPU operation instead of 63, but resource reuse and
production latency/memory acceptance remain separate work. Broader group shapes,
divergence and real-game sampling need additional independent validation.
Full Pixel History, RenderDoc consumer removal and standalone release gates
remain unfinished. This checkpoint does not claim complete P6 or release parity.
