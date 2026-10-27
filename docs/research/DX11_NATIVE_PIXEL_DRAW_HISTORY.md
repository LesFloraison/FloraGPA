# Selected-pixel private primitive storage

This P5 increment connects raster candidates to original-PS geometry checkpoints.
It records selected-texel storage immediately before and after each candidate in
a full private draw. The records are explicitly `private_primitive_storage`, not
original fragments or event snapshots. Full Pixel History remains incomplete.

## Implementation

`replayNativePixelGeometryCheckpoints` invokes a read-only planner on the exact
captured geometry that will be replayed. The planner chooses checkpoint boundaries;
the replay still draws the complete geometry, including gaps between candidates.
This avoids capturing geometry again for the join and retains private resource
writes from intervening primitives. Existing fixed-range callers keep their
previous behavior and validation.

`collectNativePixelDrawHistory` resolves the selected GPA resource to its actual
bound RTV or DSV, including the effective descriptor, typed interpretation, mip,
absolute array layer or 3D slice. Ambiguous or missing output bindings fail
explicitly. A view-relative layer is used only for raster candidate discovery;
storage readback continues to use absolute resource coordinates. The selected
view's dimensions and sample description are retained for candidate rasterization.

Candidate discovery removes the application sample mask, while original-PS
replay retains it. A masked storage sample therefore keeps its candidate even
when its bytes do not change. ForcedSampleCount uses the union of observations
from every forced raster sample, without equating those indices with target
storage samples. Candidate order is unspecified; checkpoint order is the observed
geometry storage order. Neither becomes an asserted original GPU execution order.

Every candidate contributes boundaries immediately before and after its primitive.
The complete draw's final boundary is also read. Changes in noncandidate ranges
are retained separately as `unattributed_changes`, rather than assigned to a nearby
candidate. Raw observed primitive IDs are joined from the existing sidecar;
missing GS IDs and instance identities remain null. Each row includes exact
references to its raster observations, before/after bytes, byte validity, typed
decoding and an observed-value-change flag. Identical before/after values do not
remove a row or prove that no write executed.

Single ordinary texels use narrow readback. Depth and MSAA retain the established
bounded complete-storage paths. Unused D32S8 and BGRX bytes do not establish a
change. MSAA transfer results distinguish bit-preserving storage observations
from converted sample values; `storage_changed` and bitwise restoration claims
remain null when exact storage bits are not established. Definedness remains
unknown. Original fragment eligibility, write execution, shader output and failure
reasons remain null, and `fragment_history_available` remains false.

Limits cover candidate observations, primitive records, logical payload and total
cooperative time. The selected history request's `maxRecords` and
`maxReadbackBytes` are honored separately. Temporary candidate resources are
released before original-PS replay. Readback bytes are reported, but this is not
peak process memory or a count of all GPU resource copies. Driver calls still
require the Worker's external timeout. The diagnostic validates initial private
storage against the bound original and verifies restored observations afterward.

## Validation

The existing 78 geometry-replay data cases remain unchanged in scope. This adds
66 data cases across explicit WARP, NVIDIA and Intel selection:

| Cases | Evidence |
| --- | --- |
| 36 draw histories | Overlap, blend, depth/stencil rejection, discard, early depth, sample mask, scissor, cull, clip, unchanged output and DSV selection; each candidate's bytes match independent original-draw prefixes |
| 12 MSAA selections | All four storage samples, including masked samples, match original sample-frequency PS output |
| 12 mip/layer/slice selections | Array views starting at a nonzero layer and 3D RTV slices at mip 1; relative raster layers map back to absolute storage; original GS IDs remain unknown when not emitted |
| 3 forced-sample cases | Four raster-sample observations join into one storage record and match normal drawing |
| 3 recovery cases | Invalid subresource, primitive-record limit, candidate-observation overflow and cancellation reject safely, followed by successful retry |

The final geometry/history suite passes 146 Qt checks with no failures or skips
in 32.012 seconds. The existing event-history suite passes 91 checks. Together,
237 checks pass. GUI, CLI and Worker Release targets build successfully.

The first forced-sample fixture omitted explicit depth disablement and was
correctly rejected by replay validation. The fixture was corrected to establish
valid D3D11 state, without weakening its assertions. The first CPU validator also
assumed interpolated position would produce UNORM byte 128 on every adapter.
NVIDIA produced 127 for those two components in both the unmodified original draw
and the private replay. The test shader and exact comparisons were retained;
original prefix bytes are now exported as evidence. Constant components still
have independent CPU checks. No portable explanation of the interpolation
difference is asserted, and no tolerance or golden was changed to hide it.

`validate_native_pixel_draw_history.py` verifies all 66 new records, selection and
geometry identity, sample/layer joins, prefix continuity, original reference
bytes, restoration and unknown-field boundaries. It rejects 501 intentionally
corrupted reports. Earlier failed/partial runs remain in the raw evidence ledger.

## Real capture observations

All nine fresh-process probes match normal replay's final selected pixel exactly,
restore original storage and report no unattributed changes. Capture hashes are
pinned by the validator. Each row below applies to all three tested adapters.

| Capture / event / pixel | Candidate records | Records with changed storage | Pixel readbacks / bytes | Observed duration |
| --- | ---: | ---: | --- | --- |
| GF2 / 113 / (799, 480) | 1 | 0 | 5 / 20 | 250–563 ms |
| BF1 / 2010 / (751, 433) | 10 | 2 | 23 / 92 | 1,270–1,769 ms |
| Helldivers / 200 / (168, 183) | 1 | 1 | 5 / 20 | 1,251–1,688 ms |

Unchanged records remain in both GF2 and BF1. These observations do not assign
failure reasons to them. Durations include frame replay, candidate discovery,
checkpoint replay and an independent original reference replay. They are not a
controlled regression benchmark. Private output cloning still processes complete
bound resources; the small readback totals do not describe total GPU traffic.

The recorded platform is Windows `10.0.26200.0`, NVIDIA driver `32.0.16.1742` and
Intel driver `32.0.101.8826`. The probe imports Windows graphics/runtime libraries
and Qt Core, with no GPA or RenderDoc import. This static import check is not the
product's required dynamic module or standalone packaging audit.

## Reproduction and remaining gates

Use fresh output paths and serialize GPU work. Vendor zero requests WARP
explicitly; unavailable hardware requests fail without fallback.

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelGeometryReplayTests FloraNativePixelDrawHistoryProbe --parallel 4
$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE = '<new-owned-directory>'
build/vs2022/Release/FloraNativePixelGeometryReplayTests.exe -o '<new-test-log>,txt'
build/vs2022/Release/FloraNativePixelDrawHistoryProbe.exe <frame> <event> <resource-or-zero-for-RTV0> <x> <y> <mip> <layer> <slice> <sample> <vendor> <new-directory>
python tools/validate_native_pixel_draw_history.py <new-owned-directory> --log <new-test-log> --real-prefix <new-real-prefix> --out <new-validation.json>
```

The optional real validator expects `<prefix>-gf2-<vendor>`,
`<prefix>-bf1-<vendor>` and `<prefix>-hd-<vendor>` for `0`, `4318`, `32902`, using
the selections above with mip/layer/slice/sample zero. The real probe has a
120-second cooperative deadline, a 60-second diagnostic limit and a recorded
150-second subprocess watchdog. It exports both private and reference pixel bytes.
The ledger `dx11-native-pixel-draw-history-evidence.json` pins source, binary and
raw artifact identities; captures and large research artifacts remain outside Git.

This is an internal diagnostic, not a new public history mode. Qualification,
complete failure attribution, shader-output and source-debug joins, pre-raster
and shared-UAV side effects, full formats/experiments and public UI parity still
need closure. UAV-only targets and storage layouts not covered by compatible RTV
or DSV interpretation are not established by this increment. The GPA debugger
host `+0x3c0` implementation remains unresolved; no new Ghidra conclusion is
claimed here. P5/P6 completion, RenderDoc deletion and P8 standalone release
acceptance remain required.
