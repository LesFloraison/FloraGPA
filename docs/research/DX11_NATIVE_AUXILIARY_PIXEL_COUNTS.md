# Preserve integer RT0 coverage with an auxiliary count target

## Result and remaining boundary

`native-pixel-return-count/3` keeps the original private RT0 binding when an
integer PS output uses alpha-to-coverage without an explicit coverage output.
It accumulates the diagnostic count in RT1. This closes the coverage differences
and broad rejection recorded in the [previous increment](DX11_NATIVE_PIXEL_INTEGER_COUNTS.md)
for the tested configurations with an available RT1 binding.

The operation checks the actual UAV binding at output-merger slot 1. An occupied
slot produces an explicit error before the diagnostic draw. It does not unbind,
relocate or replace an application UAV to obtain space. The saturated binding
case remains open, as do complete original fragment qualification, debugger
parity and the final RenderDoc removal/package gates. This report is not full
P5, P6 or standalone release acceptance.

## Implementation

The count request still selects a GPA event, original target and RTV-relative
pixel/layer/sample. `count_transport.source_slot` records that selected target;
`count_transport.slot` records the actual float counter binding. The
`auxiliary_target` flag distinguishes the route. Other count requests retain
their existing transport. No query normalization or vendor-specific alpha
conversion is introduced.

When the auxiliary route is needed, RT0 retains its private cloned resource,
view format, original blend behavior and original shader output. RT1 receives
an RGBA32F target with dimensions/layers/samples derived from the selected RTV.
The original draw runs once, with additive RGB counting at RT1. All application
resources remain subject to the established private-output snapshot/restoration
scope. The original RT0 clone can receive its normal shader writes during that
scope; it is not mislabeled as an unchanged diagnostic buffer.

`instrumentAuxiliaryPixelReturnCount` uses an existing nonzero output or adds an
RGB output declaration and signature entry. It inserts full-precision unit
markers before main-entry returns. Original instructions, RT0 output precision,
other declarations, depth/coverage exports, calls and resource operations are
retained. Added signature rows preserve the order and contents of the original
rows, adjusting their string offsets. The prior depth/coverage-only helper keeps
its public restrictions. Its existing tests continue to apply.

The shader report pins the original input hash and final transformed hash and
records `output_declaration_added` and `original_rt0_instructions_preserved`.
An added RGB output has no defined alpha export; diagnostic alpha words are
not original RT0 alpha and are not used to decode the count. Original fragment
eligibility remains unknown. Raw occlusion and pipeline queries remain their
own observations, including previously documented adapter discrepancies.

## Verification

The owned matrix retains uint/int/min16uint/min16int shaders, one/four samples,
one/eight targets, discard, alpha bit patterns and explicit coverage. Eight-target
cases now select both RT7 and RT0. All original frame files and reference-phase
values are compared with the retained prior experiment. This avoids replacing
the failed reference with a new golden or treating float-target coverage as truth.

The suite passes 734 Qt checks with no failures or skips in 264.5 seconds. It
covers 720 count configurations and 1,800 per-sample observations, including 720
auxiliary observations. Every count is compared with two ordinary unmodified
shader reference phases. During auxiliary draws, the test reads the actual RT0
view format before/after execution and retains its post-draw integer words;
those words must equal the ordinary reference. After each diagnostic request,
every originally bound color resource is compared with its original bytes.

Twelve additional cases cover append/counter UAVs on all three adapters. Slot 2
remains bound while RT1 accumulates counts; private UAV bytes and hidden counter
values equal ordinary replay. Slot 1 is rejected explicitly. Both paths verify
original resource/counter restoration and a subsequent ordinary private draw.
The C++ checks derive the expected count from the actual original GPU output;
the verifier also pins the observed one-pixel fixture results for this evidence
set. These observations are not product vendor rules.

Thirty CPU transform cases cover float/uint/int/minimum-precision outputs with
an existing RT1, absent RT1, sparse RT0/RT7 outputs, depth and explicit coverage.
They compare every original instruction/declaration, permitting only the added
RGB declaration/return markers or the established selected RGB mask expansion.
Invalid targets, payload budgets and cancellation are checked as well. The
separate evidence run passes 32 Qt checks including setup/cleanup; these cases
are also part of the completion regression suite and are not counted twice.

Independent validation checks original/reference frame identity, original GPU
identities, source/diagnostic slot separation, output provenance, exact counts,
RT0 raw storage and the UAV controls. It rejects 4,320 corrupted observations.
The original 24 failed NVIDIA groups and 58 sample discrepancies remain retained
in the prior increment; their frames and original values were not rewritten.

The first build failed because the new internal helper call omitted its fifth
argument. The corrected build and focused GPU/CPU runs are retained separately.
No unsuccessful GPU run occurred in this increment's focused or complete matrix.
The final full-suite executable adds raw RT0 evidence retention after the focused
build and is copied by the runner. Focused raw reports and build logs are retained;
the earlier focused executable itself was not copied.

Five regression suites pass 921 checks with no failures or skips: return count
189, ordinal replay 164, presence 53, DXBC completion 258 and geometry replay 257.
Together with the integer suite this is 1,655 checks. The regression verifier
validates 450 owned observations and rejects another 1,800 corruptions. Nine
real-capture controls retain selected counts one/two/one for GF2/BF1/Helldivers
on WARP, NVIDIA and Intel. GUI, CLI and Worker Release builds pass. The integer
RT0 route is exercised by the owned fixtures; the three real captures do not
by themselves establish integer RT0 coverage equivalence.

## Reproduction

```text
cmake --build build/vs2022 --config Release --target FloraNativePixelIntegerCountTests FloraNativePixelReturnCountTests FloraNativePixelOrdinalReplayTests FloraNativePixelPresenceTests FloraDxbcPixelCompletionTests FloraNativePixelGeometryReplayTests FloraNativePixelCompletionProbe FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
python tools/run_pixel_integer_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --out <new-directory>
python tools/validate_pixel_auxiliary_count.py --root <new-directory> --prior artifacts/dx11-native/pixel-integer-count/guarded --out <verification.json>
python tools/run_pixel_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-regression-directory>
python tools/validate_pixel_return_count.py --root <new-regression-directory> --out <regression-verification.json>
```

GPU processes run serially with existing per-operation and process deadlines.
The prior artifact directory supplies immutable original-reference comparisons;
it does not supply a runtime backend. Raw frames, binaries and logs stay outside
Git under `artifacts/dx11-native/pixel-count-auxiliary/`. The
[manifest](dx11-native-pixel-count-auxiliary-evidence.json) pins implementation
source blobs and retained evidence. Full baseline performance, peak-memory and
standalone module/package audits remain necessary.

The manifest audit verifies 3,775 retained artifacts, 14 source blobs, 30 original
and transformed shader pairs, and 2,160 unchanged prior frame identities.
