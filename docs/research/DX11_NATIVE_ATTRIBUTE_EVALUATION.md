# Native pixel attribute evaluation

Implementation: `5cc8930c39c6df3102ad807177d6f2e01fea6e01`.
Previous head: `a939c454e0fcf40c7798651c7de5ccc50791eb75`.

## Implementation

The independent C++ executor executes full-precision DXBC `eval_snapped` (203),
`eval_sample_index` (204), and `eval_centroid` (205) through original-primitive GPU
observations. This is connected to private pixel output evaluation and native
pixel debug journals, including Worker jobs. Native analysis advances to
`dx11-native/18`; the ordinary input transport remains
`dxbc-pixel-raster-inputs/2`, and evaluated observations use the separate
`dxbc-pixel-attribute-evaluation/1` identity.

The private helper preserves the captured post-transform geometry, interpolation
declarations, original sample-frequency trigger, raster state, and actual
adapter. It evaluates the input on the GPU before gathering the four quad lanes.
The C++ executor selects physical components, applies destination masks, and
preserves raw bits and validity. Dynamic locations use integer source semantics;
snapped offsets use only their low four bits. Unknown location operands remain
unknown. Out-of-range sample indices produce unknown interpolated components,
without recording invented GPU values. Constant components retain their input
value. SV_Position evaluation is rejected, as required by the specification.

Each invocation owns its geometry bridges and bounded observation cache. A cache
hit still checks cancellation. New observations must match the original quad,
shader, sample/raster state, adapter LUID, and feature level. The trace completion
and shader output report retain the observed requests and component values.
Acquisition, observations, and cached results have explicit byte/time limits.
No application UAV slot, original GPA DLL, or RenderDoc implementation is used.

The semantic references are Microsoft's D3D11 functional specification sections
[22.4.23](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#22.4.23%20eval_sample_index),
[22.4.24](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#22.4.24%20eval_centroid),
and [22.4.25](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#22.4.25%20eval_snapped).

## Validation scope

Eight suites pass **1,813 checks**, with no failures or skips in the accepted
run. The machine-readable summary is
[`dx11-native-attribute-evaluation-evidence.json`](dx11-native-attribute-evaluation-evidence.json).

| Suite | Passed |
| --- | ---: |
| Pixel inputs and attribute transport | 201 |
| DXBC execution | 1,272 |
| Execution trace writer | 28 |
| Execution trace reader | 65 |
| Pixel shader outputs | 95 |
| Pixel debug jobs and sessions | 31 |
| Native debug sessions | 50 |
| Native compute debugging | 71 |

Independent validation checks 198 GPU input cases, 968,688 quad component
comparisons, 96 evaluated/control journals, 27 Worker jobs, and 618 rejected
alterations. Of the new input cases, 78 execute eval instructions and 18 are
constant-input controls. Nine Worker cases execute the new instructions;
centroid, dynamic sample, and dynamic snapped jobs observe respectively one,
two, and four distinct locations. They compare the selected C++ output against
the original compiled shader's GPU storage readback, with exact validity/bits.
The remaining jobs retain the existing successful flows.

All observed modules exclude RenderDoc and original GPA analysis DLLs. The two
ignored manifests cover 7,652 retained artifacts, including executable hashes.
Except for the repaired pixel-debug test executable, every executable is
identical across the split runs. The nine new jobs report 768–2,840 ms elapsed
and 40.5–136.1 MiB peak working set. These are small-fixture measurements, not a
P8 performance acceptance or a claimed speedup over the old backend.

All 20 implementation files were checked against their committed Git contents,
allowing only CRLF-to-LF normalization. The ignored `source-identities.json`
manifest has SHA-256
`9c97f2a415f038c954a389cf4ba71222c42e2a6e8465f9bfb4cf3acd4892312d`.

The GPU input matrix runs on explicit WARP, NVIDIA, and Intel devices, with one
and four samples. The 102 pre-existing input cases retain their assertions. New
cases cover the three evaluation instructions, noperspective interpolation,
constant inputs, sample-frequency shaders, ignored high offset bits, and partial
coverage with helper lanes. Independently compiled original HLSL outputs and a
separate bitwise quad oracle are compared against the private transport. Native
shader outputs and reversible reader journals are compared against those same
observations.

The installed compiler rejects eval instructions on constant input declarations.
The constant control therefore directly returns the provoking vertex's constant
input, as specified; it is not presented as a compiled eval instruction. The
product helper uses the equivalent masked move for constant components.

Synthetic GPA Worker fixtures add centroid evaluation, a dynamic sample index,
and dynamic snapped offsets. They retain the existing experiment, private
primitive, storage-restoration, breakpoint/watch, configuration, and session
archive checks. Baseline CLI and failure-recovery cases remain in the suite.
These generated fixtures supplement the independent GPU matrix; they do not
establish behavior for every shader in the real-capture corpus.

The C++ contract test covers lane-specific locations, reversed swizzles, partial
writes, unknown location validity, malformed observations, missing providers,
helper lanes, and rejection outside a pixel quad. GPU-backed tests additionally
check undefined sample results, cache reuse, and cancellation on a cache hit.

## Retained attempts

Raw evidence is under `artifacts/dx11-native/attribute-evaluation/` and stays
outside Git. `initial` retains the compiler's 18 constant-input rejections.
`integrated` passed all 182 checks before adding edge and validation cases.
The first `final` attempt retains 18 edge-test failures: the new test incorrectly
expected zero for untouched storage, while the fixture clears to integer
16777216. The final assertion checks the actual sentinel across the original,
oracle, and all eight transport targets. Earlier execution-contract failures
retain the rejected attempt to seed temporary registers as inputs; the fixture
now initializes temporaries through an actual MOV instruction. Neither change
weakens existing tests or golden data.

`verified` retains five passing suites and the first nine failing new Worker
cases. Their source fixture placed rasterizer-generated `SV_PrimitiveID` before
`DATA`, causing mismatched VS/PS hardware registers. Reordering the fixture
signature repairs that invalid stage linkage. `workflow` retains the next
three snapped failures; `snapped-diagnostic` preserves a complete reproducer.
The emitted DXBC evaluates `v1.xyzw` into `r0.xyzw`, then copies `r0.xyz` into
output `yzw`. Both the original GPU shader and the C++ journal produce
`[1, 1, 7, 3]`, rather than the source-level prediction `[1, 7, 3, 0.5]`.
The corrected new tests compare compiled execution against actual GPU checkpoint
storage. Their shader bytes are unchanged; the 18 pre-existing Worker cases
retain their literal assertions. `workflow-verified` passes all three remaining
suites. The accepted report records the superseded failing run explicitly.

## Reproduction and open gates

Build the targets named in `tools/run_native_attribute_evaluation.py`, including
the current CLI, Worker, and GUI. Run the script with `--binaries
build/vs2022/Release --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out <new-directory>`.
It executes GPU suites serially, retains executables, and records each exit code.
Then run `tools/validate_native_attribute_evaluation.py --root <new-directory>
--out <report.json>`. The validator checks raw observations, jobs, loaded modules,
artifact identities, and corrupted evidence. Its full artifact manifest stays
with the ignored raw evidence.

For the retained split run, validate with `--root
artifacts/dx11-native/attribute-evaluation/verified --workflow-root
artifacts/dx11-native/attribute-evaluation/workflow-verified`. The runner's
`--workflow-only` flag reproduces the latter three suites without repeating the
unchanged GPU matrix. A fresh default run executes all eight suites together.

Minimum-precision eval operands and other currently rejected operand controls
remain explicit support gaps. These are private primitive/quad observations,
not recovered original draw fragment identity, ordering, or eligibility. P1
debugger-engine closure, full P5/P6 parity, P7 dependency removal, and P8 standalone
release acceptance remain open. This increment does not authorize removing the
existing backend or claim that the portable package is independent of RenderDoc.
