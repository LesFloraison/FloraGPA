# Native DXBC explicit-LOD sampling

The independent executor now routes `sample_l` through private D3D11 operations
on the selected replay adapter. It uses no RenderDoc interpreter code and no
Intel GPA private module. This is a P6 implementation checkpoint; implicit
sampling, derivatives, comparison sampling and full debugger parity remain open.

## Execution contract

The CPU executor validates sampler declarations, resolves the texture and sampler
registers, applies input selection and retains destination masks and raw bits.
The GPU helper recreates the observed texture, SRV, sampler and resource MinLOD
on its separate device. The operation executes in the requested VS, PS or CS
stage. A verified null sampler is bound as null, preserving native defaults.
The helper does not substitute another adapter or implement one vendor's filter
results as a universal CPU formula.

The [D3D11 instruction specification, section 22.4.18](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines explicit per-invocation LOD, sampler bias/clamps and the absence of an
implicit derivative requirement. The independent observations in
[DX11_SAMPLE_LEVEL_RESEARCH.md](DX11_SAMPLE_LEVEL_RESEARCH.md) retain the local
NVIDIA anisotropic and Intel fractional-MinLOD differences. Matching those
observations does not claim that every driver behavior conforms to the spec.

The input identity remains `independent-replay-resource-inputs/7`; the helper
identity advances to `dxbc-gpu/10`. Sampling steps mark GPU assistance explicitly.
Missing bindings, unknown coordinates/LOD and unavailable or partially unknown
texture storage do not acquire fabricated valid values. An observed null SRV
returns zero. Invalid declarations, inconsistent view metadata and unsupported
comparison samplers are errors.

Sampling currently requires all bytes of the private texture snapshot to be
known. A filtering footprint may span multiple mips, array slices or cube faces;
the implementation does not guess which unknown bytes contribute. Precise
partial-validity footprints remain an acceptance gap. The private texture path
also retains its existing 256 MiB storage bound and plane-view limitation.

All GPU reads have cancellation and deadlines. A helper that fails after
submission clears its private state and requires recreation. It never binds
diagnostic outputs, samplers or textures on the original replay context.

## Validation workflow

The final sampling suite passes 156 cases with zero failures/skips in 442.870
seconds. Its 102 independent matrix cases compare 19,278 single-lane vectors
against 6,426 native observations and verify all 102 complete CS group outputs.
The other cases are 42 synthetic capture shapes, nine cancellation/recovery
cases, one unknown/malformed-input case and suite initialization/cleanup.
Five adjacent suites add 3,062 passes: texture loads (1,445), typed pixel and
vertex operations (580 each), input snapshots (82) and scalar execution (375).
The combined focused run is 3,218 passes, zero failures/skips, 887.379 seconds of
summed Qt test time and 889.65 seconds of CTest wall time. GUI, CLI and Worker
Release builds pass. This is not a complete P0 baseline or standalone release run.

`FloraDxbcSampleLevelTests` launches the independent `FloraSampleLevelProbe`
serially on WARP, NVIDIA and Intel. The raw probe binaries, disassembly and
observations are generated afresh in temporary directories. The executor checks
all 63 coordinate/LOD records for each of 34 configurations in each of its three
supported stages, including resource result swizzling. It also executes the
probe's original compiler-generated CS through the cooperative thread-group
executor and compares the entire output buffer exactly.

This is a same-adapter comparison against independently compiled and dispatched
HLSL. The reference matrix uses compute shaders; it does not by itself establish
all native stage-specific behavior. Shader input selection, helper lanes and
source-level debugger integration are separate outstanding requirements.

Build `FloraDxbcSampleLevelTests` in Release and run `dxbc_sample_level` through
CTest with GPU tests serialized. The target builds the independent probe.
Focused texture-load and scalar-execution regressions are required because the
sampler declaration parser and private graphics operation infrastructure are
shared with existing instructions.

The additional synthetic capture matrix covers seven dimensions (1D, 2D, 3D,
Cube, 1DArray, 2DArray and CubeArray), RGBA32_FLOAT/RGBA8_UNORM and all three
adapters. It exercises restricted mip ranges, nonzero array/face starts, partial
destination masks, ignored offset axes, sampler snapshot wiring and full CS
output comparison. Nine cancellation cases stop at the first post-submission
readback check, verify no register step was committed, reject reuse of the failed
helper, then recover with a fresh helper on the same adapter.

The first complete matrix had six failures because the comparison-filter test
used one bit instead of the full reduction-mode field, incorrectly rejecting
maximum filtering. The corrected check distinguishes comparison, minimum and
maximum modes. An initial recovery fixture counted the constructor's cancellation
check, so its CS path canceled before submission; the final fixture resets the
counter after construction and asserts the exact post-submission check number.
Both failed iterations are retained in the evidence ledger.
`tools/summarize_dxbc_sample_level.py` requires both complete matrices, all
recovery cases and exact native group comparisons. A negative validation with
one group comparison marked unverified is rejected.

The implementation accepts floating-return sampleable views. Broader format,
nonfinite-input, cube-seam and stage-specific observations remain necessary for
full P6 acceptance. It also recreates private texture and output resources for
each operation; the large matrix's latency must not be confused with production
debugger performance acceptance. Bounded reuse and batched group operations
remain performance work. The original RenderDoc consumers and release packaging
are still present elsewhere in the project.
