# Capture-owned MSAA sample readback

The first MSAA implementation step adds an owned CPU sample model and a private
native readback primitive. It does **not** enable MSAA frame publication. The
ordinary resource snapshot and encoder still reject MSAA, so GF2 formal scene
acceptance remains **0/6**. The latest supported HUD acceptance is separate.

`MsaaSamples` retains resource identity, original dimensions/format/sample count,
quality, and a tightly packed plane for each array layer and sample. It cannot
be mistaken for an ordinary single-sample GenData initializer. The model has no
COM owners, Replay objects or wire records. The runtime depends only on native
DX11/D3DCompiler and capture-owned metadata; no replay helper was imported.

`snapshot_msaa_samples` copies the source once into compatible private typeless
storage, then reads each color sample through an integer view. A private compute shader
uses [Texture2DMSArray.Load](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-texture2dmsarray-load)
and writes uint4 values into a staging-bound buffer. CPU packing retains the
source component widths. There is no Resolve, averaging, float conversion, sRGB
conversion or replication of sample zero. Each plane has explicit layer/sample
identity. The source is never written.

The source's sample count and quality are used unchanged for the private copy.
Dimensions, sample count, total CPU payload and the private buffer size are
checked before GPU work. Device feature level and integer multisample-load
support are checked independently. Cancellation, timeout and device removal
checks surround submission, asynchronous mapping and row copying. Failed work
returns no partial model. Temporary maps are always released.

Private commands run in a native deferred context, with
[`ExecuteCommandList(..., TRUE)`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist)
restoring immediate bindings. A reentrancy scope excludes helper calls from
application events and object registration. The caller must own an exclusive,
synchronized application-context boundary. Active queries, predicates, prior
interface gaps, unknown native objects, foreign devices and mapped source
metadata explicitly refuse. This is not support for arbitrary concurrent or
escaped native context access.

Integer-view families cover R/RG/RGBA color storage with 8-, 16- or 32-bit
components where the device supports multisample integer loads. The subsequent
depth/stencil extension is described below. BGRA, packed color formats and special
resource flags remain explicit gaps. The table
of compatible families is broader than the concrete test matrix below; it does
not certify every member on every device.

## Independent GPU checks

The native fixture writes sample-frequency integer patterns into a compatible
typeless render target, then copies those bits into the application's actual
format. A CPU formula independently generates expected packed bytes. Cases use
7 x 5 pixels, two array layers, 2x and 4x sampling, quality zero, and ten formats:
RGBA8 UNORM/SRGB, RGBA16 FLOAT, RGBA32 FLOAT, RG32 UINT, RG16 SINT, R32 FLOAT,
RG8 SNORM, R16 FLOAT and R8 UNORM. Patterns distinguish coordinates, layers,
components and samples and include NaN payloads, infinities, negative zero and
subnormals in floating formats. Comparison is exact bytes, with no tolerance.

Both WARP and hardware tests check repeated readback, CPU data after source COM
destruction, bounded allocation and cancellation recovery. An active recording
sees no helper events or initial-resource entries. Object counts, shader/constant
buffer identities, topology, viewport and LastError stay unchanged. A separate
fixture checks four sequential stream-output appends across successful and
cancelled readback, preserves CS/OM hidden UAV counts 37/53 and observes a later
application increment to 38. Active-query refusal leaves CS invocations zero;
predicate refusal preserves its identity and BOOL. Unknown native resources are
not implicitly registered. Debug Layer warnings/errors fail the tests.

The initial compile log retains a test variable collision with Windows' `small`
macro; it was renamed without changing production behavior. Final build/test
counts and input hashes are in `capture-msaa-samples-baseline.json`; raw logs
remain ignored under `artifacts/capture-msaa-readback-20261013`.

Reproduce with the capture-only `FloraCaptureMsaaReadbackTests` build target and
`ctest --preset capture-only-release -R '^capture_msaa_readback'`. GPU tests run
serially. No Python, Qt, GPA or RenderDoc is needed by this native fixture.

Committed revision `1891553` also passes the clean source/archive workflow:
six configure/build/test/package/control steps, all 85 serial CTests, and 24
packaged launches publishing 120 existing-profile frames. The isolated build
uses a System32-only PATH and does not discover Qt. Runtime module inventories
contain no Qt, Python, RenderDoc, GPA, FloraReplay or FloraCore. These packaged
frames validate existing single-sample delivery/control; they are not MSAA files
or a new dual-player pixel run. A preceding archive accidentally started before
the whitespace commit gate completed was cancelled and excluded. Both raw runs
remain intact, with the successful revision and hashes pinned in the baseline.

## Remaining capture work

Remaining formats required by GF2 must retain their individual sample values
too. Next, independently encode a verified initialization sequence
and prove that original GPA and Flora both reconstruct retained samples before
application commands. Ordinary legacy MSAA GenData is insufficient: the retained
sample experiment in `D:/CDXrepo/FloraGPA/analysis/ACTIVE_MSAA.md` demonstrates that
both players can agree on an incorrect image when initial samples are missing.
Helper initialization must remain distinguishable from application events.
No new original-player, GUI or GF2 acceptance is claimed by this readback step.

## Depth/stencil sample readback

The next native primitive stage covers D16, D32, D24S8 and D32S8, including their
typeless depth-bound variants. D16/D32 use compatible integer views. Combined
formats use separate depth and stencil aspect views: D24 converts the loaded
UNORM value back to its 24-bit integer and packs the stencil byte; D32S8 retains
the loaded depth bits and stencil value. `MsaaSampleEncoding` makes these CPU
layouts explicit. D32S8 uses eight bytes per pixel, with the unused 24 bits
canonicalized to zero; those unused bits are not claimed as source data.
The [DXGI format definition](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
distinguishes the depth, stencil and unused portions.

The independent application fixture renders coordinate/layer/sample-specific
depth and stencil into both an MSAA texture and separate single-sample array
layers. The reference uses ordinary native Copy/Map, without the MSAA readback
shader, resolve or capture helper. Every captured plane must match those native
storage bytes exactly. Floating depth and stencil also match independent CPU
expectations. D32S8 padding is normalized by the fixed layout rule on both sides.
No tolerance is used for sample comparison.

The matrix uses 67 x 19 pixels, two layers, 2x/4x samples and quality zero. Depth
patterns include zero, one, one half, 1/2^24, (2^24-1)/2^24 and varied binary
fractions. Each WARP/hardware run passes 16 cases and 92,938 checks, with zero
Debug Layer warnings/errors. Checks cover cancellation/recovery, source lifetime,
DSV/state/stencil reference/sample mask/LastError preservation, helper exclusion
from the application stream and continued production snapshot refusal.

An initial CPU-only oracle assumed one universal UNORM round-up rule. Hardware
D16 at one half disproved that assumption. The corrected oracle compares exact
independently produced native storage, retaining the failed run. The expanded
matrix records 3,554 WARP and 1,294 hardware native-versus-CPU quantization
differences as diagnostics, counted once per case; none is a tolerated mismatch
between the native oracle and readback. Initial fixture lifetime-order and
variable-name failures are also retained. This matrix does not establish every
depth bit pattern, device, sample quality or count.

Reproduce with target `FloraCaptureMsaaDepthTests` and
`ctest --preset capture-only-release -R '^capture_msaa_depth'`. Raw logs remain
under `artifacts/capture-msaa-depth-20261013`; hashes and regression results are
in the baseline's `depth_stencil_readback` entry. Committed depth revision
`a665593` also passes all six clean source/build/test/package/control steps and
87 serial CTests. With a System32-only PATH and no Qt discovery, 24 packaged
launches publish 120 existing-profile files; runtime inventories contain no Qt,
Python, RenderDoc, GPA, FloraReplay or FloraCore. These remain single-sample
delivery/control checks, not MSAA encoded frames. The raw source-package run is
`artifacts/capture-msaa-depth-source-a665593`, with hashes in the baseline.
Production
snapshot/encoding, Replay and shared format are unchanged; formal GF2 remains
**0/6**, with no new dual-player or golden-frame result.
