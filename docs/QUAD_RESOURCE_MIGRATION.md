# Native resources for Quad diagnostics

`application/QuadResources` migrates the remaining target/dummy/producer/storage
responsibilities of `quad_targets.py`, alongside the previously migrated array
shader filter. It also implements `quad_counter.fingerprint_outputs`. This is
an internal building block; the native depth prepass, counter executor and Qt
Quad panel are still pending. The delivered package remains
`out/FloraGPA-coverage-ui/FloraGPA.exe`.

## Preserved behavior

Target selection retains `auto`, explicit RTV slots and depth selection, checks
the actual native RTV/DSV binding and reuses the existing checked
`outputSubresource` parser. Selected mip, absolute/relative layer, first/count,
buffer element range, dimension, sample count/quality and target provenance
match Python. Targetless draws retain finite native viewport bounds, negative
origins and the D3D11 texture extent limit. The later counter executor still
owns its original targetless WARP sample-mask rejection and cross-view checks.

Dummy RTVs match buffer, 1D, 1D array, 2D, 2D array, multisample and 3D view
dimensions. They have RGBA8 storage, view-bounded layer/slice counts and the
original sample layout. Prepared-depth requests use the original single-sample
2D adapter for buffer/3D outputs. This allocation does not perform the depth
prepass or change the original pipeline. Texture creation reuses Replay's
existing native resource allocator.

Array-index producer discovery checks GS, DS then VS, skips SO passthrough
signature providers and follows the rasterized GS stream. It retains complete
original output-signature metadata and experiment-replaced bytecode. The
existing `filterQuadArrayIndex` remains responsible for filtering before UAV
side effects.

Storage reads all original texture subresources and individual MSAA samples in
sample-major order. Buffer fingerprints include complete native storage.
Inspection reads suspend predication using the existing replay isolation logic.
Fingerprints resolve each actual RTV/DSV/UAV resource owner, retain aliases by
view ID and include append/counter UAV values, high OM slots and SO resources.
They neither submit nor count original frame work.

## Validation design

`tools/validate_quad_resources.py` runs unmodified Python target allocation and
fingerprinting against the native probe on hardware and WARP. It compares full
selection/provenance JSON, producer signatures, actual view descriptors,
resource descriptors and cleared storage hashes for both ordinary and
prepared-depth dummy allocations. Native tests additionally query the created
resource descriptor and require it to equal the storage descriptor. Original
output fingerprints must remain identical before and after all requests.

Cases cover 1/2/4/8x MSAA, color/depth-only targets, arrays/mips, 1D depth including
read-only DSVs, 3D slices including all-remaining-slices views, six buffer formats,
sparse RTV slots, targetless/negative-origin viewports, routed arrays, high-slot
pre-raster UAV counters, all four rasterized streams and SO passthrough providers.
Each fixture checks invalid targets and out-of-view layers. A deliberate native
RTV unbind proves selection checks actual bindings. All other default `auto`
requests must succeed, so matching unexpected failures cannot mask a broken
allocation path. Rejection presence and partial-result boundaries are compared;
original error strings are retained in evidence but are not required to match
the shared native output parser's wording.

The initial matrix passes **112 GPU cases / 1,232 selections** with 796 rejected
selections and 872 dummy allocations. It fingerprints 275,456 bytes of original
storage per sweep and exercises non-null array producers in 38 cases. The only
default-auto failures are the two deliberately unbound native-output cases.
Evidence: `artifacts/quad-resources-v1/validation.json` and adjacent raw JSON.
That run preceded the additional native descriptor query and is not counted
again in the final matrix.

The final matrix repeats all 112 cases after adding native `GetDesc` verification
and passes all 1,232 selections (796 expected rejections and 872 allocations).
Evidence: `artifacts/quad-resources-final/validation.json`; probe SHA-256:
`1cd6b076ca6b56dde72a64d1ce158499f3995b6edec63849dea3487b803eb37b`.
The full Release build and ten related CTest suites pass. GF2/BF1 golden pixels,
execution counts, suppress-draw negative controls and runtime module audits
pass (`artifacts/quad-resources-golden/validation.json`).

The ledger marks `quad_targets.py` ported and `quad_counter.py` partial, with
only its output fingerprint helper migrated. Totals are 24 ported, 117 partial
and 63 pending; these are module counts, not workload percentages.

## Remaining integration

`quad_depth.py` still needs the PS gate, original-DSV copy/stencil preservation or
local D24S8 allocation, clear-depth pass, original draw with private outputs and
prepared-depth integrity digest. The complete counter requires cross-view
compatibility, high-UAV restoration, isolated counter/reference passes, original
HLSL, uint32 storage/report/preview, single original-event submission and Qt
controls/export. The existing experimental measurement limits remain in force.
