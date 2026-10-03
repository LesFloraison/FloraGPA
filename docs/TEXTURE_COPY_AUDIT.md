# M2: checked texture copies and ResolveSubresource

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

Offline preflight and production replay now share texture transfer validation for
`CopyResource` (0x3e), `CopySubresourceRegion` (0x40), and `ResolveSubresource`
(0x42). Previously textures received only basic reference/usage checks, while
Resolve only checked wire length and context. Invalid regions could reach a
void-returning native call without a capture-level diagnostic.

## Implemented scope

- Decode Resolve's exact 44-byte record, context, source/destination IDs,
  subresources and typed format; reject linked, truncated or trailing layouts.
- Validate dimensions, nonzero arrays/samples, effective mip count, and array/mip
  subresource indices before computing extents.
- Whole ordinary texture copies require equal dimensions, mip/array counts,
  compatible format families and equal sample counts/quality.
- Ordinary region copies check source coordinates and widened destination ends,
  dimensional restrictions and different source/destination subresources.
  Different layers of the same texture are supported.
- Depth/MSAA regions require a null box, zero destination offsets and equal
  subresource extents. Empty boxes retain their no-write event behavior.
- BC regions use physical block extents, including sub-4x4 mips. Explicit boxes
  and offsets require block alignment; whole-subresource null boxes remain valid.
- Resolve requires an MSAA 2D source, a DEFAULT single-sample 2D destination,
  equal selected subresource dimensions and compatible typed format selection.
  Replay additionally queries the actual device's MULTISAMPLE_RESOLVE support;
  offline success does not certify device support or image correctness.

The contracts are Microsoft's [CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource),
[CopySubresourceRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion),
[ResolveSubresource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-resolvesubresource)
and [BC virtual versus physical storage](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression).

## Original evidence and regression probes

The development-only `FloraTextureCopyProbe` creates its own DX11 workload and
optionally requests an unmodified GPA `CaptureNextFrame`. Every frame explicitly
initializes its inputs/destinations, transfers data, checks every destination
subresource byte against a CPU oracle, and renders a texture-dependent green
image. A failed texture comparison renders magenta. The framebuffer is also read
back and checked; neither byte success nor image success is inferred from the
void copy/resolve return type. Capture files remain outside Git.

| Fixture | Behavior |
|---|---|
| whole_2d | Whole texture including multiple mips and layers |
| array_mip_region | Offset rectangle between selected array/mip subresources |
| same_resource_layers | Copy between different layers of one resource |
| volume_region | Offset 3D region in a selected mip |
| one_d_region | Offset interval in a selected 1D mip |
| bc_mip_edge | 1x1 BC1 mip copied with an explicit 4x4 physical-block box |
| bc_mip_null | The same small BC mip copied with a null box |
| depth_whole | Whole D32-compatible depth subresource using a null box |
| msaa_copy | Four-sample whole copy, followed by Resolve to a shader input |
| resolve_typed | Four-sample typed source to single-sample typed destination |
| resolve_typeless | Typeless source/destination with an explicit UNORM resolve format |

The first BC producer attempt used an explicit 1x1 source box on the 1x1 mip.
Native readback remained zero. A second run with the D3D11 debug layer reported
message 280: BC coordinates must align to a block boundary. The corrected 4x4
box passed the producer with the debug layer, and both corrected box/null-box
forms passed unmodified original capture generation. The validator consequently
rejects the 1x1 explicit box rather than treating it as a legal logical-mip edge.
The failed artifacts remain in `artifacts/m2-texture-copy-originals/5/native`
and `artifacts/m2-texture-bc-debug`; the successful debug control is
`artifacts/m2-texture-bc-block-debug`. These exploratory runs are not silently
substituted for final acceptance.

C++ tests check each original capture's destination bytes immediately after its
transfer and again at frame end, on hardware and WARP. Separate negative tests
cover zero/oversized dimensions, mip/array limits, overflow, wrong dimensional
coordinates, format/sample mismatches, same-subresource copies, depth/BC region
restrictions, Resolve usage/type/format errors and strict wire boundaries.

```powershell
cmake --build --preset release --target FloraTextureCopyProbe FloraTextureCopyTests
python tools/capture_texture_copies.py --producer build/vs2022/Release/FloraTextureCopyProbe.exe --out artifacts/new-texture-originals
$env:FLORA_TEXTURE_COPY_CAPTURES = "$PWD/artifacts/new-texture-originals"
ctest --preset release -R '^texture_copies$' --output-on-failure
```

## Remaining limits

This is not full format/driver conformance. Documented BC/uncompressed
reinterpretation pairs retain their existing execution with an explicit
`texture_copy_validation_partial` finding; their differing coordinate units are
not passed through ordinary texel extent checks. Packed, one-bit and planar
formats also retain partial diagnostics. Format-family checks alone do not
certify every hardware format capability. The new original corpus is not an
exhaustive format, sample-quality, cube or feature-level matrix.

Inspection readbacks are separate from captured command validation. Mip count
zero is normalized for transfer extent checks; this does not complete all
inspection consumers' implicit-mip handling. Frame-initial contents, mapped
lifetimes, deferred resource versions and other interface layouts remain separate
work. No missing bytes/counters are invented, and Helldivers shaders are unchanged.
M2 and M3–M6 remain incomplete; Python migration module counts are unchanged.

## Final acceptance

The originals are registered in [texture-copy-corpus.json](texture-copy-corpus.json).
Source/binary hashes and retained evidence paths are pinned in
[texture-copy-baseline.json](texture-copy-baseline.json). The portable package is
`out/FloraGPA-texture-copy-final-20261003/`. GPU runs were serialized.

| Check | Result |
|---|---|
| Prior 112 registered files | 224 native runs completed; all 111 previous stable image hashes unchanged; Helldivers retains its documented variability |
| Eleven new original files | 22 independent and 22 original-player runs; exact matching images |
| Producer oracle | 22 native/shim runs × 12 frames; 264 full-destination byte and texture-dependent image checks |
| C++ original boundary checks | Eleven files on hardware and WARP; destination bytes checked after the relevant transfer and at frame end |
| All 123 registered files | 246 independent runs; 122 repeat-stable and one repeat-variable file; no failed replay/preflight |
| Default runtime dependencies | All 246 runs pass module audits excluding GPA/Python/RenderDoc |
| Corpus controls | 49 passed, including 22 new transfer-disabled magenta controls |
| Isolated packaged goldens | GF2/BF1 and their draw-disabled negative controls all passed |
| Relevant CTest | Twelve suites passed; seven texture-transfer, 34 texture-edit-replay and 55 main UI Qt cases, no skips in those suites |
| Development validator | Ten CPU tests passed |
| BC malformed-box regression | Prior preflight accepts; new preflight rejects event 24, source 15, destination 5 before GPU execution |

The malformed-box preflight check uses a clearly labeled negative-only copy of
the BC original with right/bottom changed from 4 to 1. Its source hash and exact
mutation are retained in `artifacts/m2-texture-copy-fixtures/bc-unaligned-box.json`.
It is never counted among original acceptance files and was not submitted to the
old replay GPU path. The native debug-layer producer supplies independent evidence
for the underlying invalid operation.

The original player's adapter remains unidentified. Its image agreement is not a
claim of matched-device/driver equivalence or an original-kernel resource dump.
The historical 94 texture transfer records now pass ordinary structural transfer
checks, with no remaining `texture_copy_validation_partial` finding in that
historical corpus. Special format paths outside that observed set remain partial;
zero such findings in this corpus does not close their validation scope.
