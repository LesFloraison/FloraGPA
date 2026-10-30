# Pixel ordinal queries with sparse render-target bindings

Implementation: `4a64accb953e4941ea0dcd4690c772e3edb2935c`.
Previous branch head: `76a21de97826ed0b19dbae2bdc66f820d1d06c4e`.

## Finding and scope

The owned D3D11 probe isolates an unresolved Intel occlusion-query discrepancy
from GPA replay and from changes to shader bytecode. Keeping eight declared PS
outputs and the same shaders, but binding one, two, four or eight RTVs, changes
the measured query multiplier. Repeated observations also expose different
counts for identical draw inputs and bindings. Selected pixel values remain
correct in these fixtures. Neither a fixed divisor nor a successful single-RT
run establishes a reliable correction.

This is P5 research. Product replay, history results and public backend selection
are unchanged. `original_fragment_eligibility` remains unknown. The native backend
and executor identities remain `dx11-native/12` and `dxbc-gpu/31`. The experiment
runs an owned D3D11 probe without invoking RenderDoc or Intel GPA tools. This
increment does not remove the project's remaining RenderDoc dependencies.

[D3D11 occlusion queries](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
count samples passing depth/stencil tests; the documented result is not a count
of color exports. [OMSetRenderTargets](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets)
permits arbitrary occupied and empty RTV slots. These contracts and the probe's
closed-form geometry establish the expected counts independently of either
analysis tool.

## Controlled experiment

The optional `FLORA_PIXEL_ORDINAL_TARGET_MASK` selects occupied RTV slots while
retaining the final slot used for readback. The shader still declares and writes
all eight outputs. Masks 255, 128, 192 and 240 bind slots 0–7, 7, 6–7 and 4–7,
respectively. `OMSetRenderTargets` receives eight entries in every experiment;
`OMGetRenderTargets` verifies every returned pointer and releases its reference.
All created targets are cleared, including unbound targets.

`FLORA_PIXEL_ORDINAL_REPETITIONS=4` repeats each count/rank pair. Every observation
clears context bindings, color and stencil, reinstalls the pipeline, issues its
own occlusion query, waits for bounded GPU completion and reads back the selected
pixel. No observation is discarded as warmup. The query includes only the draw;
the compute readback happens afterward.

The matrix uses explicit WARP, NVIDIA and Intel selection, direct/instanced/GS
draws, one/four samples, a 4x4 viewport and integer outputs. Each of 72 binding
configurations contains 88 observations: counts 8 and 260, eleven stencil ranks
and four repetitions. This binding experiment uses the plain shader only; the
unchanged 144-configuration default matrix separately covers discard, early
depth/stencil and coverage modes. Default bindings and one repetition remain the
probe's normal behavior.

Sparse bindings deliberately produce debug-layer messages for unbound PS
outputs. Only `DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET` (3146081), warning severity,
and a slot actually absent from the mask qualify as expected. Raw messages remain
in the evidence. All other diagnostics fail; the all-bound default still requires
an empty diagnostic list. Query and output assertions are unchanged.

## Results

The authoritative repeatable run is `verified/`. GPU processes ran serially.

| Run | Qt passes | Strict failures | Skips |
| --- | ---: | ---: | ---: |
| Eight bound RTVs | 14 | 6 | 0 |
| One bound RTV | 20 | 0 | 0 |
| Two bound RTVs | 14 | 6 | 0 |
| Four bound RTVs | 14 | 6 | 0 |
| Original default matrix | 140 | 6 | 0 |
| Total | 202 | 24 | 0 |

All 23,760 selected sample vectors (95,040 raw components) match the independent
model. The 9,504 observation rows retain 1,326 query discrepancies: 1,224 in the
binding experiment and 102 in the default matrix. The 24 failing groups are Intel
query assertions. WARP and NVIDIA match both output and query expectations.
Default failures reproduce the existing six Intel plain/eight-MRT groups; they
are research failures, not newly permitted P0 exceptions.

The earlier complete matrix in `final/` retains 1,233 binding-query discrepancies,
including nine with only slot 7 bound. Eighteen count/rank groups produced different
query values across their four repetitions. Relative to `verified/`, 43 query
observations differ despite identical shader bytes, selected adapter and binding
identity. The selected vectors agree in both runs. In the later run, the sparse
Intel queries consistently show multipliers of one, two or four for the occupied
RTV count; in the earlier run, some initial queries still show eight. The eight-RTV
case consistently shows eight in both runs. The internal driver/runtime cause
has not been identified; asynchronous compilation or caching remains a hypothesis,
not a recovered implementation fact.

Adapters were WARP 5140:140, NVIDIA 4318:11352 and Intel 32902:32103, with LUIDs
recorded in every report. Local driver inventory records NVIDIA 32.0.16.1742 and
Intel 32.0.101.8826. This finding is not generalized to other drivers or hardware.
The five authoritative processes took approximately 81 seconds in total; these
are probe durations, not product latency or baseline-performance measurements.

The verifier checks ordered matrix completeness, bindings, adapter identity,
eight PS output signatures, byte-identical VS/GS/PS/readback shaders across masks,
independent selected vectors, terminal Qt results and preserved process failures.
It rejects 504 corruptions of bindings, identity claims, output bits, repetition
order and warning attribution. Four invalid-input probes reject empty masks,
missing readback slots, out-of-range slots and excessive repetition before device
creation. Both the runner and verifier return nonzero for the retained query
failures; evidence validation is not reported as semantic success.

## Retained intermediate failures

The initial sparse-binding probe passed null entries to the pre-existing color
clear loop and terminated with exit 2173. The fix clears the owned views rather
than the sparse binding array. Its original terminal log remains in the evidence.
The next focused sparse run retained expected missing-RTV warnings and therefore
failed the original empty-diagnostics assertion. The final implementation checks
those specific intentional warnings while preserving all default assertions.
Neither intermediate failure was deleted or reclassified as a successful run.

## Relation to GPA research and remaining work

The prior [GPA counter-state study](DX11_PIXEL_ORDINAL_RESEARCH.md) identified the
player's additive RGB blend descriptor at RVAs `0x4e0a2..0x4e0cc`, creation through
device slot `+0xa0` at `0x4e21c..0x4e22a`, and its color-disabled variant. Those
pinned Ghidra findings motivate a separate additive-count observation, but do not
prove this MRT query discrepancy has been solved or establish original fragment
eligibility. This increment performs no new GPA decompilation and does not close
the debugger host `+0x3c0` implementation chain.

A replacement must independently validate count acquisition while preserving the
original shader's discard, depth, sample, UAV and blend behavior. Detaching other
application targets, dividing queries by RTV count or dropping early observations
would require additional proof and is not applied here. Complete Pixel History,
full VS/PS/CS parity, dependency removal and independent package acceptance remain
open gates.

## Reproduction and evidence

Build `FloraNativePixelOrdinalTests` in Release, then use fresh output directories:

```text
python tools/run_pixel_ordinal_bindings.py --binary <Release/FloraNativePixelOrdinalTests.exe> --qt-bin <Qt/bin> --out <new-directory>
python tools/validate_pixel_ordinal_bindings.py --root <new-directory> --out <verification.json>
python tools/validate_pixel_ordinal_bindings.py --root <later-directory> --prior-root <earlier-binding-directory> --out <comparison.json>
```

The runner resets inherited ordinal-probe overrides, retains all terminal exits,
copies the probe executable and hashes input binaries. The optional prior directory
contains the four `mask-*` directories. The second and third commands return 1
when query discrepancies remain, while retaining the complete verification JSON.

Raw evidence is under `artifacts/dx11-native/ordinal-target-bindings/`, outside Git.
The [manifest](dx11-pixel-ordinal-bindings-evidence.json) pins sources, raw reports,
shader bytes, executable, driver inventory and intermediate logs. Original GPA
files, Ghidra databases and large captures remain outside the repository.
