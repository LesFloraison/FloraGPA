# Private shader and output-merger joins

## Execution boundary

`collectNativePixelDrawHistory` can now request `evaluateOutputMerger` together
with `evaluateShaderOutputs`. At each candidate's before-primitive checkpoint,
the implementation observes the live output bindings and storage once, then
evaluates each captured private PS invocation and applies its selected lane to
the selected storage sample. The application sample mask is retained separately
from the mask restricted to that sample. The resource/view, subresource, frame,
adapter, geometry ordinal, lane, raster observations and shader-output digest
remain available for independent verification.

The original private GPU geometry replay still executes the complete original
PS with its output state. At the next checkpoint, its selected texel is compared
with the independently executed shader/merger result. Reports retain both the
comparison of observed values and, only for exact transfers, the comparison of
storage bits. A mismatch is reported explicitly; it is not silently accepted as
equivalent or used to overwrite the checkpoint. D32S8 padding remains excluded
using the existing byte-validity metadata.

The output merger uses the same overall cancellation deadline as geometry and
shader evaluation. Its additional output reports count toward retained shader
payload, and its readback bytes are reported separately. GPU target allocation
uses the existing private merger payload limit. These logical limits are not a
claim about total driver memory allocation.

## Scope and remaining work

This joins independently implemented components on an actual captured resource
boundary. It does not establish original-fragment eligibility. The underlying
geometry checkpoint path still substitutes the pre-raster stages, and the
isolated merger uses an already evaluated shader lane with a helper PS. Original
early-depth execution, ordering of side effects across invocations and complete
sample-depth semantics remain qualification work. A null PS produces no shader
invocation or shader/merger join; its GPU storage checkpoint is still retained.

The new records remain `private_output_mergers` alongside
`private_shader_outputs`. The existing original `shader_output`, write-executed
and failure-reason fields remain unknown. Public full Pixel History and pixel
debugging are not enabled by this change. P7/P8 removal and release gates remain
open.

## Validation

The final geometry suite passes 233 Qt checks with no failures or skips in
85.367 seconds. The checkpoint-input suite passes 635 checks with no failures or
skips in 139.401 seconds. These are 868 passing checks across the two suites.
Release GUI, CLI and Worker builds also pass.

The integration validator accepts 102 owned reports and nine real capture and
adapter combinations, rejecting 1,197 corrupted reports. It verifies the
shader-result digest, observed state identity, before-primitive storage,
measured raster fields, selected sample mask, readback accounting and the
checkpoint comparison independently. The existing shader/history validator also
passes its 72 owned and nine real reports, rejecting 864 corruptions. These
validators have overlapping inputs; their combined 2,061 corruptions are not
additional GPU cases.

Thirty new two-primitive fixtures exercise ordinary and dual-source blending,
alpha-to-coverage, color write masks and same-value writes at one and four
samples on all three adapters. Independent original draws stopped at each
primitive prefix supply the comparison bytes. The validator additionally checks
exact arithmetic for the deterministic blend/mask modes. Same-value writes must
retain equal storage and a passing-sample count of one at both prefixes.

Nine real runs preserve the pinned selected events and coordinates. All 36
selected primitive predictions match private checkpoints exactly, and all nine
final private outputs match an independent normal GPU replay. Elapsed probe
measurements, including capture loading, geometry/input collection, shader and
merger execution, storage restoration and reference replay, are:

| Capture | WARP (ms) | NVIDIA (ms) | Intel (ms) | Joins per adapter |
| --- | ---: | ---: | ---: | ---: |
| GF2 event 113 | 702 | 1,061 | 1,047 | 1 |
| BF1 event 2010 | 6,988 | 10,494 | 9,112 | 10 |
| Helldivers event 200 | 3,985 | 8,528 | 3,202 | 1 |

These measurements do not establish a P8 performance baseline or peak-memory
bound. No new transient module audit was performed. An initial five-case smoke
passed seven Qt checks. The first complete pair of suites passed 203 and 635
checks before the 30 additional blending fixtures were added. A validator was
invoked before the checkpoint suite had produced its shader reports and stopped
on a missing file; after that suite completed, its full matrix validated without
changing assertions. No functional test or numerical comparison failed.

## Reproduction

Build `FloraNativePixelGeometryReplayTests`,
`FloraNativePixelCheckpointInputsTests` and `FloraNativePixelDrawHistoryProbe` in
Release. Run GPU work serially with fresh evidence directories:

```powershell
$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE = "$PWD/artifacts/dx11-native/merger-join-geometry-rerun"
$env:FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE = "$PWD/artifacts/dx11-native/merger-join-checkpoint-rerun"
ctest --test-dir build/vs2022 -C Release -R '^(native_pixel_geometry_replay|native_pixel_checkpoint_inputs)$' --parallel 1 --output-on-failure
python tools/validate_native_pixel_history_merger.py "$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE" "$env:FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE" --out merger-joins.json
```

The existing real-capture probe accepts `merger` instead of `outputs` to enable
the complete join. Its ordinary independent after-event replay and restored
storage checks remain unchanged. The validator accepts `--real-prefix` to add
the three pinned GF2/BF1/Helldivers selections on WARP, NVIDIA and Intel. Original
captures, binaries and raw reports stay outside Git; the evidence ledger pins
their local identities.
