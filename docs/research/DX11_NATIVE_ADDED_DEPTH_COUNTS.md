# Depth counting without a bound shader color export

## Implemented behavior

`native-depth-return-count/3` closes the mixed-output route when no shader color
export is bound to a color view. It selects an existing RTV and adds an unused
RGB output to the shader. Existing exports and original instructions remain;
only the new output receives return markers. A missing PS uses the project's
owned empty shader. The actual private DSV remains bound throughout the draw.

The existing depth-only output helper retains its restrictions. The new
`addPixelCountTarget` exposes the already shared signature/declaration insertion
with existing color exports allowed. This handles a shader that exports to an
unbound slot without deleting that export. Stale debug/statistics chunks are
removed by the shared transformation; the original shader remains the source
for actual debugger execution, not this diagnostic program.

The added-output route currently rejects alpha-to-coverage, including when the
shader has other unbound exports. It does not fabricate alpha for the new output.
Existing declared/bound output counting, integer RT0 coverage protection, source
view alignment, private snapshots, deadlines and cancellation remain in use.

The native Worker now returns an observation for mixed depth selections without
a PS. Color-only selections without a PS remain explicitly not applicable. The
analysis context is `dx11-native/16`, history is `native-pixel-draw-history/5`,
and native debug sessions accept the new contract. Prior contexts are invalidated.

`original_null_pixel_shader` and the added-program hashes distinguish an owned
empty shader from an original application program. Counts describe diagnostic
returns, not original PS invocations, original fragment identities or scheduling.
Original fragment eligibility remains null. Neither query values nor final
resource equality are substituted for the count.

## Validation

Five suites pass **743 checks**, with zero failures or skips:

| Suite | Passed |
| --- | ---: |
| DXBC pixel completion | 387 |
| Native color return count | 189 |
| Isolated draw history jobs | 56 |
| Native analysis | 61 |
| Native debug session | 50 |

The mixed-output matrix now has 102 configurations and 255 sample observations
on WARP, NVIDIA and Intel. Five added modes cover an empty PS, absent PS,
pure depth export, early-depth conditional discard without color export, and
an original export to unbound RT7 while RT0 is bound. Each runs with one/four
samples and three overlapping primitives. These add 75 observations to the
previous matrix, including 30 explicit alpha-to-coverage rejection/restoration
checks. The final depth-only matrix retains all 117 observations.

Ordinary unmodified GPU stencil writes provide independent count references
for late-test cases. Early-depth discard increments original stencil three
times but returns twice; the test keeps these as different measurements.
Shader depth failure returns zero. Every diagnostic depth image matches the
unmodified reference. Color/depth storage is restored after execution and
post-draw cancellation, and retry returns the same count. Rejected added-output
alpha-to-coverage requests also leave original storage unchanged.

Independent validators check shader hashes, route selection, actual adapter,
source subresources, raw count validity and original storage. The 255 mixed
observations reject 1,785 altered/reference-incompatible reports; the 117 depth
controls reject 702. The Worker matrix has 51 owned jobs and nine real-capture
color controls, rejecting another 315 alterations: **2,802 total**. Internally
valid unknown counts remain unknown and cannot satisfy measured fixture values.

All 60 job artifact/context checks pass. The GF2/BF1/Helldivers color controls
retain independent prior counts one/two/one across all adapters. These real
controls check regressions; the new added-output route is established by owned
fixtures, not by claiming those color selections exercise it. Completion-time
module snapshots contain neither RenderDoc nor original GPA private modules;
they are not a complete process-lifetime release audit.

GUI, CLI, Worker and all five test targets build successfully. Both builds and
all GPU runs in this increment passed. GPU work was serial. No existing golden
or skip condition was relaxed. The final copied binaries, raw shader/evidence
files, terminal logs and manifests are retained outside Git.

## Reproduction and remaining scope

```text
cmake --build build/vs2022 --config Release --target FloraDxbcPixelCompletionTests FloraNativePixelReturnCountTests FloraNativeDrawHistoryJobsTests FloraNativeAnalysisTests FloraNativeDebugSessionTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
python tools/run_depth_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-directory>
python tools/validate_mixed_depth_count.py --root <new-directory>/depth --out <mixed-verification.json>
python tools/validate_depth_return_count.py --root <new-directory>/depth --out <depth-verification.json>
python tools/validate_history_return_count.py --root <new-directory>/jobs --prior-counts artifacts/dx11-native/pixel-count-auxiliary/regression/real --out <jobs-verification.json>
```

Raw evidence is under `artifacts/dx11-native/mixed-empty-count`.
`dx11-native-added-depth-count-evidence.json` pins sources and evidence.
A separate read-back verified all 1,486 artifact hashes, 22 source Git/working
hash pairs and nine prior count-report hashes. Implementation author/committer
match the requested identity, and main remains at the requested baseline.
This increment invokes neither original GPA nor RenderDoc. It does not close
P1 debugger-engine recovery, full fragment reconstruction/failure attribution,
complete debugger parity, dependency removal or standalone package acceptance.
Transport restrictions and the added-output alpha-to-coverage case remain open.
