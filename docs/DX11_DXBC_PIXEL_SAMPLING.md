# Native implicit pixel sampling

`PixelQuad` now executes `sample`, `sample_b` and `sample_c` with the original
implicit hardware instruction on the selected adapter. CPU lanes retain control
flow, registers, source operands, trace changes and discard/helper state. The
GPU helper consumes the complete four-lane coordinate cohort and each lane's
bias or comparison reference. It does not replace implicit LOD with CPU-selected
derivatives or replace comparison filtering with a comparison after filtering.

This is a synthetic invocation checkpoint. Captured quad input discovery,
complete helper memory behavior, fragment selection, additional resource shapes
and debugger presentation remain open requirements for full P6 acceptance.

## Request and execution contract

Implicit operations require pixel stage and static texture/sampler bindings. The
scheduler gathers the same instruction, call stack and loop iteration in all
four lanes, then drains their prepared results before subsequent instructions.
Derivative and implicit sampling rendezvous share the existing 4,096-step wait
bound, whole-quad budget, cancellation and poisoned-task rules. A single scalar
lane cannot invent implicit derivatives. The execution model is
`coherent-pixel-sampling/2`; the GPU helper identity is `dxbc-gpu/15`.

The private helper preserves the native SRV descriptor, all mip storage, resource
minimum LOD, sampler descriptor, immediate offsets and original declaration
return classes. It uses a 2x2 integer target, matching pixel coordinates, a
coverage query, bounded nonblocking readback and the original adapter identity.
Shader variants are cached with a fixed entry limit. Original replay contexts
and resources are not used for these draws.

Source coordinate/reference modifiers, result swizzles and destination masks
remain part of CPU instruction execution. Unknown coordinates conservatively
invalidate the whole implicit footprint. Unknown per-pixel references or biases
invalidate their own lane's result. Missing binding/storage data remains unknown;
observed null resources retain defined zero results. A missing comparison sampler
is explicitly undefined and rejected. Known bias outside the documented finite
range is rejected rather than relying on driver-dependent incidental values.

`samplingBatches()` counts handled quad sampling cohorts, including defined null
resources resolved without a GPU operation. `memoryGpuAssisted` records whether
the instruction actually used the GPU helper. These counters are not interchangeable.
Helper output registers remain execution values, not proof of target writes.

## Independent evidence

`FloraPixelSamplingProbe` uses public D3D11 without replay, GPA or RenderDoc. It
records ordinary, biased and comparison sampling on R32_FLOAT Texture2D across
point/linear/anisotropic filters, address modes, mip controls, offsets, per-pixel
operands, non-affine inputs, discard, reconvergence, loops and partial coverage.
`research/DX11_PIXEL_SAMPLING_RESEARCH.md` specifies the matrix and SDK sources.
There are zero differences between full coverage and the surviving partially
covered pixels for this matrix. This observation is not a portable guarantee
for other shapes, formats or captured primitive/sample boundaries.

`FloraDxbcPixelSamplingTests` generates fresh independent references and executes
the original compiler-produced shaders. Its first complete run passes 168 tests:
147 configurations, nine modifier/mask/trace checks, nine cancellation checks,
one validity/error check, and setup/cleanup. The matrix compares 71,424 components
from 9,408 shader scenarios, rejects 192 undefined sampler scenarios, and handles
9,216 defined sampling cohorts. It checks private snapshot preservation, raw
results, masked resource component defaults, source modifiers, reverse trace
restoration, partial validity, invalid bias, scalar execution rejection, adapter
identity and cancellation after submission followed by a fresh job.

Build `FloraDxbcPixelSamplingTests`, then run `ctest --test-dir build/vs2022 -C
Release -R ^dxbc_pixel_sampling$ -j 1 --output-on-failure`. The strict validator
`tools/summarize_dxbc_pixel_sampling.py` additionally requires all eight adjacent
regressions and exact final CTest completion. Historical derivative/comparison
validators accept explicit backend/model arguments so old checkpoint evidence
can be rechecked without rewriting it.

## Remaining parity gates

The final Release build includes the GUI, CLI, Worker and all nine affected test
targets. The serial run passes 3,087 tests with zero failures or skips in
1,343.03 seconds. Strict implicit sampling, derivative, comparison, explicit-LOD,
gradient and compute sampling validators pass. The evidence ledger is
`research/dx11-dxbc-pixel-sampling-evidence.json`; it records source, binary and
log identities, required regressions and negative evidence mutations. Historical
goldens and the earlier divergent compute failures remain unchanged.

The implicit suite takes 283.78 seconds in the final run, compared with 176.77
seconds in its first run. Adjacent suite timings also vary. These observations
are not a controlled performance comparison and do not satisfy performance
acceptance. No system power policy, timer resolution or GPU wait implementation
was changed for this checkpoint.

Additional dimensions and formats, depth-compatible comparisons, min/max filters,
sample-frequency invocations, multiple primitives, implicit sampling followed by
further helper-dependent operations, captured resource/input provenance and full
source/debugger integration still require independent evidence. The implementation
retains the existing legacy debugger while these gates remain open. Neither this
checkpoint nor its synthetic tests permit claiming complete RenderDoc removal.
