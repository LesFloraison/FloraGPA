# Captured DX11 multisample resolves

The capture runtime records immediate-context `ResolveSubresource` independently
of Replay. The CPU encoder emits the verified legacy category 7 / type `0x42`
record after validating the captured resource graph and subresource dimensions.
No Replay/Core helper, state object or GPU executor enters the capture model.

The admitted profile is 2x/4x quality-zero RGBA8_UNORM and RGBA16_FLOAT, with
typed or corresponding typeless source and destination storage. Destination
storage is single-sample DEFAULT; source storage has one mip. Matching dimensions
are checked for the selected subresources, including a destination mip below its
base level. Predicates, deferred Resolve, additional format families, nonzero
quality, layout/misc flags and unverified bind profiles explicitly refuse.
These constraints follow the [Microsoft Resolve contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-resolvesubresource).

Only Resolve operands are dependencies. Other bound, CPU-mapped pipeline resources
do not prevent an unrelated Resolve. Operand mappings still refuse on every use.
The source retains its first-use sample planes; later Draws and repeated resolves
stay in application order, rather than replacing the source with its final data.

`FloraCaptureResolveTests` records a Resolve, a partial sample-masked Draw, a
second Resolve to the same destination and a Resolve from the other array layer,
followed by a display Draw. An independent CPU oracle checks all destination
bytes, including untouched mip levels. The RGBA16 display exposes high and low
half-float bytes. WARP/hardware each exercise eight profiles across both formats,
both sample counts and all typed/typeless operand pairings. Native owner release,
zero Debug Layer warnings/errors and 72 invalid-model refusals pass per device.

`tools/validate_capture_resolve.py` executes 48 positive and deletion-control files
in FloraGPA and the pinned original GPA player. All 1110 checks pass, including
exact output, events, shader DXBC/disassembly and module audit. Removing the
resolves or the application Draw changes actual output while retaining the
captured reference image. This evidence does not establish original GUI event
selection or a new startup-injected Resolve fixture. Detailed test counts and
hashes are in [the baseline](capture-resolve-baseline.json).

The subsequent elevated GF2 main-scene preflight reaches
`ID3D11DeviceContext::Unmap: Unmap has no recorded successful Map`.
It publishes neither a frame nor a partial file. Presents advance from 2868 to
5285 with zero interface gaps; disconnect and normal UI close succeed. Formal
GF2 acceptance remains **0/6**. Cross-frame mapping ownership and data, broader
state coverage and the six dual-replayed scene captures remain required.

Raw evidence is retained under `artifacts/capture-resolve-20261014` and
`artifacts/capture-gf2-resolve-20261014`. The initial test compilation failure
(missing local byte-buffer alias) remains in `build.log`; corrected builds are
separately recorded. Replay/shared format sources are unchanged, so no new golden
run is claimed. The latest independent committed-source package is `a2a3465`,
before D32S8 and Resolve integration; a current package proof remains pending.
