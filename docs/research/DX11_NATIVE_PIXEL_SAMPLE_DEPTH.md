# Native sample-depth observations

## Reproduced defect

The private shader/merger integration previously supplied the original PS
position-z observation as fixed-function raster depth. That input can use
pixel-center interpolation even when the selected storage location is an MSAA
sample. A new original-GPU D32 fixture reproduced 36 failures out of 60 cases:
four samples, three non-flat depth modes and three adapters. Single-sample and
flat-depth cases passed. The initial failed reports and test log remain in the
local evidence set; no golden was rewritten.

## Implementation

The native integration now obtains a separate, explicitly identified
`sample_depth_observation` for the selected sample and private primitive. An
owned helper shader observes `SV_Position` with sample, noperspective
interpolation on the same adapter. Its raw position bits, geometry digest,
primitive ordinal, sample, coverage, front-face value and complete raster
context are retained. The merger identifies this input with
`raster_depth_source: sample_position_observation`.

The helper uses the actual bound DSV format, including when the selected output
is a color target. Original PS input collection also uses that DSV format for
depth bias, while preserving the original input interpolation and execution
frequency. The original PS is still independently executed; its position input
is not replaced by the helper's sample position. Explicit shader depth output
continues to override raster depth inside the existing merger.

Microsoft's [D3D11.3 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 15.10 and 16.3, distinguishes center and sample position interpolation.
Position-z follows the corresponding interpolation location, and depth bias is
applied before interpolation. The PS position input has not yet undergone depth
format quantization. These rules motivate the separate observation and the
use of the actual DSV format. Independent original GPU writes validate the
implemented behavior.

The additional capture obeys the existing cancellation deadline and payload
limits. Missing, ambiguous or inconsistent sample observations fail explicitly.
The report's original-fragment eligibility remains unknown. This change does
not establish original early-depth ordering, side-effect scheduling or complete
Pixel History. Null-PS merger qualification and public pixel debugging remain
open, as do P7/P8 removal and standalone acceptance.

## Recorded validation

The new matrix passes 302 Qt checks (300 cases plus lifecycle), with no failures
or skips, in 223.642 seconds. The original failure run passed 26 checks and
failed 36; its 29.307-second log and all 60 original reports are retained. A
three-adapter corrected smoke and a later three-format color-bias smoke each
passed five Qt checks. No test assertion or skip condition was relaxed.

Release GUI, CLI, Worker and the affected test/probe targets build successfully.
The four regression suites pass with no failures or skips:

| Suite | Qt checks | Qt elapsed seconds |
| --- | ---: | ---: |
| Pixel trace | 24 | 0.455 |
| Pixel inputs | 221 | 93.064 |
| Checkpoint inputs | 635 | 130.176 |
| Geometry replay | 233 | 97.497 |

Together with the new matrix, these suites pass 1,415 Qt checks. The regression
CTest run completes in 322.66 seconds. The new sample-depth validator rejects
4,200 corruptions. The merger validator accepts 102 owned and nine real reports,
rejecting 1,197 corruptions; the shader validator accepts 72 owned and nine real
reports, rejecting 864. These overlapping validations total 6,261 corruption
controls, not additional GPU runs. A separate compatibility run also accepts the
previous immutable merger evidence and rejects its 1,197 controls.

All nine pinned real capture/adapter runs preserve the independent normal GPU
result and restore original storage. All 36 selected private primitive mergers
match their GPU checkpoints exactly. Measured probe durations are:

| Capture | WARP (ms) | NVIDIA (ms) | Intel (ms) |
| --- | ---: | ---: | ---: |
| GF2, event 113, pixel 799/480 | 688 | 1,246 | 1,252 |
| BF1, event 2010, pixel 751/433 | 7,545 | 11,330 | 10,762 |
| Helldivers, event 200, pixel 168/183 | 3,080 | 8,373 | 3,629 |

The added depth observation requires an additional private raster capture per
selected primitive with a bound DSV. The durations include full probe work and
are not isolated overhead measurements or a P8 performance qualification. Peak
memory and transient loaded modules were not audited in this step. The local
evidence ledger includes initial failures, corrected probes, final reports,
build/test logs and stable copies of the final built executables. Raw captures,
binaries and large reports remain outside Git.

## Reproduction

Build `FloraNativePixelSampleDepthTests` in Release, then run the test and the
independent validator with a fresh output directory:

```powershell
$env:FLORA_PIXEL_SAMPLE_DEPTH_EVIDENCE = "$PWD/artifacts/dx11-native/sample-depth-rerun"
ctest --test-dir build/vs2022 -C Release -R '^native_pixel_sample_depth$' --parallel 1 --output-on-failure
python tools/validate_native_pixel_sample_depth.py "$env:FLORA_PIXEL_SAMPLE_DEPTH_EVIDENCE" --out sample-depth-validation.json
```

The matrix contains 300 cases: WARP, NVIDIA and Intel; one and four samples;
every sample index; D32S8, D32, D24S8 and D16; flat depth, a slope, depth bias,
clamped bias, and color output with depth bias. Independent original draws
supply depth storage and selected-target bytes. D32S8 padding retains its
existing invalid-byte classification. Color cases export the original PS
position-z bits to an integer RTV, checking its unchanged interpolation
separately from per-sample depth. Deterministic flat and single-sample slope
values also have exact arithmetic assertions.

The validator requires all 300 reports and the expected 192 sample/center
differences. It rejects 4,200 deliberate corruptions of sample identity, geometry,
depth source, raw bits, format, independent reference bytes and eligibility.
The existing shader/merger validator checks the new observation when present
and retains support for immutable older research reports.
