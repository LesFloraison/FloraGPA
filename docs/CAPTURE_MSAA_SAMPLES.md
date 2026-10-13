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
storage, then reads each sample through an integer view. A private compute shader
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
components where the device supports multisample integer loads. Depth/stencil,
BGRA, packed formats and special resource flags remain explicit gaps. The table
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

Depth/stencil and the formats required by GF2 must retain their individual
sample values too. Next, independently encode a verified initialization sequence
and prove that original GPA and Flora both reconstruct retained samples before
application commands. Ordinary legacy MSAA GenData is insufficient: the retained
sample experiment in `D:/CDXrepo/FloraGPA/analysis/ACTIVE_MSAA.md` demonstrates that
both players can agree on an incorrect image when initial samples are missing.
Helper initialization must remain distinguishable from application events.
No new original-player, GUI or GF2 acceptance is claimed by this readback step.
