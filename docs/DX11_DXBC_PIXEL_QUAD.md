# Native pixel derivative execution

`PixelQuad` provides four CPU DXBC lanes with a common private resource and GPU
identity. It executes legacy, fine and coarse X/Y derivatives through the selected
adapter's native pixel instructions. It does not yet provide captured quad input
discovery, implicit sampling, complete helper memory semantics, or debugger UI
integration. These remain required for P6 and complete RenderDoc removal.

## Execution and provenance

Inputs are explicit row-major 2x2 lane inputs. The caller must establish their
invocation provenance; this interface does not infer alignment from a selected
pixel coordinate. Coverage identifies target-writing lanes while all four lanes
remain available for derivative calculations. A successful discard removes the
lane from `activeMask` and continues its CPU instructions for helper arithmetic.
Output register values in a helper trace are execution values, not render-target
writes. Standalone `Lane` retains its previous terminating-discard behavior.

The scheduler waits for the same instruction, call stack and dynamic loop
iterations. All four prepared derivative results drain before another instruction
executes. A returned neighbor or incompatible dynamic derivative is rejected.
Gathering is limited to 4,096 ordinary instructions; the whole quad also has a
step budget and cancellation callback. Failed quads cannot resume. Each returned
step includes the lane, helper status before execution, resulting active mask,
instruction changes and GPU assistance provenance. No original invocation order
is inferred from this deterministic serialization.

`dxbc-gpu/14` uses a private 2x2 draw on the exact selected adapter, a pixel coverage
query, bounded nonblocking staging reads and a poisoned state after submission
failure. Raw inputs are constant-buffer bits; raw outputs are integer render-target
bits. Fine derivative validity requires the corresponding horizontal or vertical
pair. Coarse and legacy validity conservatively requires all four values because
the hardware's selected pair is not a portable guarantee. Missing components are
sanitized only for private GPU evaluation and remain unknown in the result.
Source swizzles and modifiers, destination masks, and saturation use the existing
instruction machinery. Unknown values are never promoted to known values.

Helper buffer, UAV, atomic and synchronization instructions currently fail before
performing their operation. This explicit pending path must be replaced with
independently validated helper semantics before full parity is claimed; it does
not remove an existing debugger feature. Existing independent CS and scalar lane
execution remains available.

## Validation

`FloraDxbcPixelQuadTests` generates fresh public-D3D11 references with
`FloraPixelDerivativeProbe` on WARP, NVIDIA and Intel, then interprets each original
compiled pixel program. All 1,008 cases compare 6,048 active output components.
Finite results and non-NaN bits are exact; NaN arithmetic is checked as NaN without
imposing unspecified payload equality. An additional direct-instruction check
compares 144 vectors bit-for-bit, including signed zero, denormals, infinities and
NaN payloads. The original independent matrix and inputs remain unchanged.

The 1,022-case suite also verifies partial validity by row/column, source modifiers,
masked and saturated output, explicit missing-GPU/single-lane/mixed-adapter errors,
returned neighbors, bounded rendezvous, helper memory rejection before mutation,
post-submission cancellation, poisoned-state rejection and retry with a fresh job.
The first run exposed a new unit test's incorrect positive-zero assumption on
WARP. The modifier/mask unit now uses nonzero positive and negative gradients;
the direct bit test separately preserves hardware zero signs. The first log is
retained and no previous golden was changed.

Build the `FloraDxbcPixelQuadTests` target and run `ctest --test-dir build/vs2022
-C Release -R ^dxbc_pixel_quad$ -j 1 --output-on-failure`. The strict evidence
validator is `tools/summarize_dxbc_pixel_quad.py`; it also requires seven adjacent
regression suites. Independent probe scope and raw identities are documented in
`research/DX11_PIXEL_DERIVATIVE_RESEARCH.md`.

## Final serial checkpoint

The GUI, CLI and Worker Release builds pass. Eight serial suites pass 2,919 tests
with zero failures or skips in 1,628.21 seconds. The pixel suite contributes 1,022;
the existing comparison, sampling group, gradient, level, typed pixel, group and
scalar execution suites contribute 190, 296, 262, 156, 580, 38 and 375 respectively.
Strict comparison, gradient, level and sampling-group validators also accept the
new helper identity. Five mutated derivative research inputs and five mutated
execution logs are rejected by their respective validators. The complete ledger
is `research/dx11-dxbc-pixel-quad-evidence.json`.

Observed wall times are not a controlled performance comparison:

| Existing suite | Previous checkpoint seconds | Current seconds |
| --- | ---: | ---: |
| Comparison sampling | 207.31 | 698.65 |
| Sampling groups | 33.15 | 40.61 |
| Gradient sampling | 116.04 | 363.36 |
| Level sampling | 129.59 | 383.65 |
| Thread groups | 2.55 | 2.54 |
| Scalar execution | 32.40 | 33.33 |

The current power scheme was observed as Silent. A separate ambient managed
`Thread.Sleep(1)` observation averaged 15.45 ms across 64 calls. There is no
equivalent previous power/timer observation, and this is not a measurement of
the native GPU polling loop. These facts are retained as investigation context,
not a causal attribution or excuse to relax latency gates. Performance acceptance
remains false; controlled operation latency, replay count and peak memory
measurements are still required for release.
