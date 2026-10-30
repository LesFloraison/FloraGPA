# Return counts for mixed depth and color bindings

## Result and semantics

Depth selections with a bound PS color output now reach native accumulation
instead of being classified as inapplicable. `native-depth-return-count/2`
selects the first declared, bound color output after inspecting the effective
shader, including extended minimum-precision declarations. It verifies the RTV
and DSV view extents and sampling before issuing a diagnostic draw. The actual
DSV remains bound on private resources. Count acquisition then reuses the existing
color transport and integer RT0 alpha-to-coverage protection.

`depth_transport` identifies the depth view and selected relative layer;
`count_transport` identifies the color source and diagnostic target. Width,
height, layer count, sample count and quality must match. Source mip and first
resource layer are separate identities: a DSV beginning at layer one can align
with an RTV beginning at layer two. Absolute GPA depth selections are translated
to the bound DSV domain before invoking the route. It does not infer alignment
from resource IDs or use the same absolute layer in both resources.

The native CLI/Worker uses `dx11-native/15` and
`native-pixel-draw-history/4`; stale contexts are invalidated and native debug
sessions accept the new history contract. The depth-only route remains
`native-depth-return-count/1`. Mixed bindings without a PS retain the prior
explicit inapplicable/null result. A mixed route without a bound shader color
output or matching 2D view fails explicitly. Existing auxiliary-target UAV and
transport restrictions remain required. No backend or adapter fallback is added.

Counts describe the private diagnostic draw. Original fragment eligibility,
instance attribution and original scheduling remain unknown. Raw query values
are retained but do not determine counts. A valid unknown at the float integer
limit remains null; it cannot satisfy a measured fixture count.

## Independent GPU evidence

The final matrix has 72 configurations and 180 selected sample observations on
explicit WARP, NVIDIA and Intel adapters. Each configuration uses one or four
samples and three overlapping triangles. Modes cover ordinary returns, depth
failure, shader depth, conditional discard, early-depth discard, explicit
coverage, unsigned/signed/minimum-precision integer alpha-to-coverage, RT7-only
bindings, and nonzero mip/array views with different first source layers.

Each case first runs the unmodified shader and reads its actual depth/stencil
storage. For late-test fixtures, the stencil increment independently counts
surviving returns. Conditional discard produces two; depth or shader-depth
failure produces zero. Integer alpha-to-coverage expectations come from the
actual unmodified GPU result rather than a vendor-specific guessed mask.
For early-depth discard, original stencil increments three while the diagnostic
return count is two. This explicitly distinguishes depth writes from returning
color samples. All diagnostic depth bytes equal the unmodified reference.

The final array fixture initializes every depth subresource, including unused
layers, so whole-resource comparisons do not depend on undefined storage. After
diagnostics and cancellation raised after GPU execution, selected color/depth
storage is restored; retry returns the same count. Invalid DSV coordinates are
rejected. The minimum-precision case covers the extended output declaration
found during code review; declarations are not assumed to contain three words.

The verifier checks complete sample matrices, shader hashes, view/domain joins,
raw counts, original stencil deltas and depth equality. It rejects 1,260 altered
or reference-incompatible reports. That includes 180 cases where an internally
valid unknown count is accepted as unknown but rejected against a known measured
reference. The prior 117 depth-only observations still pass and reject another
702 alterations.

## Worker and regression results

Five suites pass **713 checks**, with zero failures and skips:

| Suite | Passed |
| --- | ---: |
| DXBC pixel completion | 357 |
| Native color return count | 189 |
| Isolated draw history jobs | 56 |
| Native analysis | 61 |
| Native debug session | 50 |

The 51 owned Worker jobs include mixed depth, mixed null-PS, depth-only,
integer coverage and array selections. Nine GF2/BF1/Helldivers CLI controls keep
the independent prior color counts of one/two/one on all three adapters.
Artifact, context and completion-time module validation passes for all 60 jobs
and rejects 309 alterations. The module snapshots exclude RenderDoc and original
GPA private modules; this is not a complete process-lifetime release audit.

The updated color validator is additionally rerun against 450 retained native
observations and nine retained real controls, rejecting the same 1,800 corruptions.
Those are CPU revalidation of historical evidence, not new GPU executions or
additional current-suite passes. The final new evidence rejects 2,271 alterations;
including the historical verifier regression gives 4,071 negative controls.

GUI, CLI, Worker and all five test targets build successfully. The first focused
run passed 50 checks over a smaller 120-sample matrix. Its logs/data remain as
supplementary evidence; that earlier executable was not copied. The authoritative
`final/` run includes the initialized array fixture, complete precision/depth
matrix and copied executables. All builds and GPU runs in this increment passed;
no old golden or skip condition was relaxed. GPU work ran serially.

Real color-control jobs report 283–1645 ms of Worker analysis and peak working
sets of 371507200–2743869440 bytes. These are whole-job observations, not isolated
mixed-depth performance measurements or a P0/P8 comparison.

## Reproduction and remaining scope

```text
cmake --build build/vs2022 --config Release --target FloraDxbcPixelCompletionTests FloraNativePixelReturnCountTests FloraNativeDrawHistoryJobsTests FloraNativeAnalysisTests FloraNativeDebugSessionTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
python tools/run_depth_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-directory>
python tools/validate_mixed_depth_count.py --root <new-directory>/depth --out <mixed-verification.json>
python tools/validate_depth_return_count.py --root <new-directory>/depth --out <depth-verification.json>
python tools/validate_history_return_count.py --root <new-directory>/jobs --prior-counts artifacts/dx11-native/pixel-count-auxiliary/regression/real --out <jobs-verification.json>
```

Raw files are under `artifacts/dx11-native/mixed-depth-count` outside Git.
`dx11-native-mixed-depth-count-evidence.json` pins source, binary and evidence
identities. Original GPA/RenderDoc tools were not invoked for this increment.

P1 debugger-engine closure, complete fragment reconstruction and failure reasons,
full debugger parity, RenderDoc dependency removal and standalone package
acceptance remain open. These observed count populations do not close those gates.

## Evidence integrity

Implementation commit `734ea05bd106265b1c5b87333bbdc90880ca5ce2` pins the change. A separate read verified all 1,528 artifact sizes and hashes, 21 source Git blobs and hashes, and 642 retained reference artifact hashes. All 60 completed jobs identify the copied Worker executable hash. The author and committer match the requested identity, and `main` remains at the frozen baseline.
