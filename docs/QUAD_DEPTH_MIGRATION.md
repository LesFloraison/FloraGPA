# Native private depth preparation for Quad

`application/QuadDepth` implements the original `quad_depth.prepare` helper.
It operates inside the event's existing replay/input-edit scope; the caller
provides the original geometry submission callback. This is not yet the complete
Quad executor or a new UI feature. The delivered package remains the Coverage
package until the counter, reports and Qt controls are connected.

## Preserved behavior

Preparation requires an actually bound pixel shader, including its dynamic
class instances. An existing DSV is resolved to its actual native resource
owner, whose complete descriptor and storage are copied. The selected extent
must match the diagnostic target. Read-only DSV flags are removed on the private
view. Clearing depth to 1 retains the copied stencil and untouched subresources.

Without an original DSV, the helper allocates local D24S8, initializes stencil
to zero, then clears depth. It preserves 1D/2D array dimensions and MSAA layout;
buffer and 3D targets receive the original depth-compatible 2D adapter. These
allocations retain the Python research implementation's explicit local choices
and do not claim to recover GPA's allocation policy.

The original depth/stencil state, stencil reference, PS and pipeline edits are
rebound inside fresh private output copies. RTVs and UAVs are removed before
the callback; the eventual counter callback may rebind private graphics UAVs
for pre-raster writers. The original helper metadata remains unchanged, and
that caller must update its UAV scope fields as in Python.

`QuadHighUavs` ports `quad_counter.preserve_high_uavs`: on feature level 11.1,
it retains slots 8–63 and restores them alongside the restored low slots,
starting after the RTV range. RAII covers successful and exceptional exits.
Private outputs reuse existing predicate, counter, alias and SO isolation.
The helper can retain SO instead of cloning it when its caller redirects or
unbinds SO before submission. Safe unbinding uses Replay's existing full private
sink buffers before a zero-count hardware unbind; directly setting zero targets
can leave stale driver destinations. It never increments original replay counts.

The returned object owns the prepared resource/view and provides a repeatable
SHA-256 digest over all subresources and all MSAA samples. Complete report
fields preserve the cleared/prepared hashes, descriptors, stencil basis,
submission count and original scope text.

## Validation

`tools/validate_quad_depth.py` compares the unmodified Python helper against
native hardware and WARP executions. It compares full metadata, cleared and
prepared storage hashes, repeated digests, all original output fingerprints,
native output/PS/class/depth/SO binding restoration and unchanged execution counts.
Expected failures include null PS, mismatched extents and deliberately throwing
submission callbacks. The latter also exercise high UAV and SO restoration.

The matrix includes all original depth/stencil fixture modes, PS discard and
depth output, color/depth-only targets, arrays/mips, 1/2/4/8x MSAA, typeless 1D
depth and read-only DSVs, 3D/buffer adapters, targetless viewports, all pre-raster
stages with UAV counters in slots 1/8/63, dynamic classes in all graphics stages,
four SO streams and passthrough providers, GPU-generated indirect arguments,
predication, disabled draws and experiment-replaced PS bytecode.

The final matrix passes **192 hardware/WARP cases**, including **10 expected
rejections**. Complete metadata and storage hashes match exactly; no error
message or metadata normalization is applied. Evidence:
`artifacts/quad-depth-final-v2/validation.json`, adjacent raw reference/native
JSON and probe SHA-256
`e6b632a6f61d472dbbdc7a78935e3fbb6f5119980d0f68e842eb143b08f1ebf4`.

The initial 182-case comparison passed after rerunning only the native probe
because its first launch preceded completion of the build. Evidence remains in
`artifacts/quad-depth-v1/comparison.json`. An added retained-SO test initially
called a bare zero-target unbind in its callback; the reference invariant caught
stale hardware writes. The final test uses the original safe unbind helper and
the native callback uses the existing Replay implementation. The failed log is
retained at `artifacts/quad-depth-final.log`; the final matrix includes all five
rasterized-stream choices on both drivers with unchanged original storage.

The full Release build passes (`artifacts/build-quad-depth-final.log`).
Thirteen related CTest suites pass, including depth/stencil, class linkage,
predication, SO, frame output, Coverage UI and all four Quad helper suites
(`artifacts/ctest-quad-depth-final.log`). GF2/BF1 golden hashes, original execution
counts, suppress-draw negative controls and isolated runtime module audits pass
(`artifacts/quad-depth-golden/validation.json`).

The ledger marks `quad_depth.py` ported and retains `quad_counter.py` as partial.
Totals are 25 ported, 117 partial and 62 pending; module counts are not workload
completion percentages. The overall Python-to-C++/Qt migration remains unfinished.

## Remaining integration

Private graphics UAV relocation and its depth callback are now implemented and
validated in [QUAD_UAV_MIGRATION.md](QUAD_UAV_MIGRATION.md). The full native
counter still needs cross-view compatibility checks, counter/reference passes,
original HLSL and uint32 artifacts, accounting/report/preview generation, single
original-event execution and compact Qt controls/export. Passing this helper's
tests does not establish full Quad parity or full Python migration.
