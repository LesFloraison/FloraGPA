# Capture-owned MSAA sample readback

MSAA work now includes an owned CPU sample model, native readback primitives,
a scoped color initialization encoder and immediate-context recording. The
recording path retains RGBA8/RGBA16 2x/4x quality-zero samples before first use and
passes owned bytes to the encoder. Two native startup-injected color profiles
also pass repeated capture and dual-player execution. Broader formats and
hidden-state combinations remain pending. GF2
formal scene acceptance remains **0/6**. The supported HUD acceptance is separate.

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

## Executable initialization experiment

The next experiment moves the owned native sample planes into an ordinary
single-sample integer texture per plane, draws each plane to its matching layer
and sample of a private MSAA render target, and copies that texture to the
application resource. It uses ordinary DX11 Draw/Copy records and shader DXBC.
No GPA private entry points or Replay helpers are used by the fixture. This is
a **test-only wire writer**, not a production encoder/runtime integration.

The source application produces 13 x 7 RGBA8 planes, two layers, 2x/4x samples,
quality zero, with coordinate/layer/sample-specific values. Capture's native
readback owns the initial planes before the application overwrites the left
region of even samples. A final application shader displays every sample in
separate tiles; no Resolve or averaging can hide sample differences. Source COM
objects are destroyed before encoding. Native initial planes and final output
are checked against independent CPU formulas, with exact bytes and clean Debug
Layer results on WARP and hardware.

The encoded MSAA resources retain their sample counts and array layers. Their
ordinary GenData contains deliberately unrelated bytes, so it cannot supply
correct samples. An explicitly labelled experimental initialization region and
the fixture's event-ID manifest distinguish helper operations from application
Draws. Original GUI grouping and annotation context identity are not certified.
This does not claim the helpers were application calls.

Both players execute complete files and two controls: deleting initialization
Draws leaves the helper's deterministic cleared contents; deleting application
Draws retains the initial sample patterns. Both controls retain the complete
file's captured final image and preview. Four positives and eight controls
(two source devices, two sample counts) match their respective independent
expected bytes. The validator also checks event counts, sample/layer descriptors,
shader DXBC/disassembly and Flora's module inventory.

### Scissor encoding defect found by the experiment

The first original-player run restored all initial planes but omitted the
scissored application changes. The fixture and production encoder inherited a
disabled state-mask bit 672 from the old empty-reference workaround. Flora's
full-state application masked this omission; original GPA respects the bit.
Changing only that bit makes the original output exact. Disabling scissor,
changing sample masks and removing annotations were separate retained diagnostic
controls, not accepted substitutes for the requested behavior.

The production encoder now leaves bit 672 enabled and always emits a valid
scissor array record, including count zero. Four hook-observed cases on each
source device exercise left-to-right rectangle changes, resetting to an empty
array with scissor enabled, disabling scissor after that reset, and an initially
empty scissor array. All eight production-encoded files execute exactly in both
players. This is an actual encoder fix; production MSAA publication remains
explicitly refused.

The combined validator is `tools/validate_capture_msaa_encoding.py`; its first
complete matrix passes 20 files / 412 checks. Raw evidence is retained in
`artifacts/capture-msaa-wire-20261014`, including the initial mismatched original
image and the isolated mask-bit correction. Regression counts, hashes and
subsequent injected checks are recorded separately in the baseline. The earlier
depth readback source-package proof remains revision `a665593`.

Remaining work includes a bounded capture-owned initialization lowering model,
build-time shader generation, broader color/depth/stencil initialization,
production frame snapshot/encoder integration, and GF2's formal six-frame gate.
The successful color wire experiment does not establish those requirements.

## Capture-owned initialization encoder

`FrozenFrame::initial_msaa` now carries the owned sample model independently of
ordinary `initial_resources`. The encoder validates all MSAA inputs before
allocating initialization records or opening a transaction. Map keys, resource
identity, object kind, exact descriptor size, dimensions, format, sample count,
quality and usage must agree. Every layer/sample must appear exactly once in
canonical order with exact row and plane lengths. Total source-byte and plane
budgets apply across the entire frame, including multiple MSAA resources.
Defaults are one GiB and 1,024 planes. Invalid or cancelled inputs publish no file.

MSAA SRVs/RTVs additionally require compatible typed formats, matching MSAA
view dimensions, bounded array ranges and actual resource bind flags. The current
encoder permits the RGBA8 color family, quality zero, 2x/4x samples and ordinary
DEFAULT resources. Depth/stencil initialization, other color families, special
flags, application MSAA copy/update/map commands, predicates and query-containing
MSAA frames explicitly refuse. The dual-player corpus currently exercises
typeless RGBA8 storage, UINT initialization views and UNORM application views;
the table is not a certification of every family member/device combination.

The encoder emits a labelled initialization region, a private compatible MSAA
target, owned ordinary plane textures, per-sample masked Draws and a final
CopyResource. Original resource descriptors remain intact. Empty ordinary
GenData makes clear that the initial sample data comes from the explicit
operations. Helper IDs are allocated separately from application object IDs and
all helpers precede application commands. Each application Draw keeps its full
captured state and sample mask. Original GUI grouping/context association and
arbitrary selected-event replay remain separate acceptance requirements.

Capture-owned HLSL is compiled by `FloraCaptureShaderCompiler` during the build.
Only generated DXBC arrays enter `FloraCaptureEncoder`; its target does not link
D3DCompiler, D3D11, DXGI, Replay, Core, Qt or the format reader. The dependency
check now explicitly forbids a D3DCompiler link on the encoder. No runtime shader
compiler or GPU helper was added to the CPU encoder, and no generated header or
shader binary is committed.

`FloraCaptureMsaaEncoderTests` supplies real native sample snapshots to the
production encoder after source COM objects are destroyed. The native fixture is
shared with the earlier wire experiment without importing replay code. It
checks malformed models, aggregate limits, incompatible views and cancellation.
`tools/validate_capture_msaa_encoder.py` executes the resulting files in both
players and removes initialization Draws only in separate negative-control
copies. Exact sample-tile pixels, application-Draw deletion controls, shader
exports, event counts and independent modules are checked. Unlike the previous
experiment, positive files now come from `encode_frame`.

At this encoder stage, runtime recording did not yet populate `initial_msaa`.
The subsequent immediate recording stage is described below. Broader
initialization formats and GF2's six-frame gate remain incomplete. Evidence and regression
counts are in the baseline's `color_encoder` entry and ignored directory
`artifacts/capture-msaa-encoder-20261014`. No new GF2/GUI/golden result is implied.

The committed CPU encoder revision `a899110` additionally passes the isolated
source-package validator: 93 serial CTests, all six stages, 24 packaged launches
and 96 existing single-sample files. All 24 targets were nonforeground, so
automatic selection correctly refused; explicit selection and recovery passed.
Shader generation succeeds from the
archived source directory with spaces under System32-only PATH. The module
audit finds no Qt, Python, GPA, RenderDoc, FloraCore or FloraReplay. Raw evidence
is retained in `artifacts/capture-msaa-encoder-source-a899110`; this does not
claim live MSAA capture or a new dual-player/GF2 result.

## Immediate-context recording

The runtime now snapshots supported MSAA dependencies before their first
recorded use. Later references keep the original samples, even after an
application Draw changes the resource. Sample bytes contribute to the same
frame byte budget as ordinary resources and payloads; an additional aggregate
limit bounds the number of planes. `freeze_frame` moves this separate owned
map to the encoder and releases all retained COM objects on the application
thread. No Replay or format-reader dependency is introduced.

Live admission is limited to DEFAULT RGBA8 color resources, 2x/4x, quality zero,
ordinary SRV/RTV bindings and no special flags. Unsupported initialization
profiles include resource ID, format and sample count in the error. Deferred
MSAA and direct MSAA copy/update/map commands explicitly refuse. Encoder query
and predicate restrictions remain in force; MSAA swap-chain output is still
unsupported. The broader standalone depth sample reader does not imply depth
capture support.

`FloraCaptureMsaaRecordingTests` observes actual wrapped context calls instead
of constructing command models. WARP and hardware retain all pre-modification
planes, record exactly three application Draws with no internal helper calls,
match independent CPU output and release native owners before encoding.
Each device also refuses four byte/plane-budget cases after verifying that
the application's final pixels are unchanged. The recorded positive files,
application-Draw deletion controls and initialization-Draw deletion controls
pass 12 files / 248 exact dual-player checks using
`tools/validate_capture_msaa_encoder.py --recorded`. No preview is used as the
execution oracle. Evidence is retained in
`artifacts/capture-msaa-recording-20261014`; regression counts and hashes are
recorded separately in the baseline. This is wrapped-context coverage, not
startup-injected MSAA or formal GF2 acceptance.

## Startup-injected color capture

The native live target now has two MSAA profiles without any capture-runtime,
encoder or Replay linkage. A persistent 13x7 two-layer texture contains distinct
coordinate/layer/sample values. Every frame changes only the left part of even
samples, cycling five exact integer colors; the display Draw shows each sample
separately in a single-sample swap-chain output. Unmodified samples therefore
depend on preserved initial data. After disconnect, another native render and
staging readback must match the target's independent CPU oracle.

Mode 24 uses 2x samples, direct device/swap-chain creation and blt `Present`;
mode 25 uses 4x, factory `CreateSwapChainForHwnd`, flip-discard and `Present1`.
`FloraCaptureMsaaControllerTests` exercises actual launch injection, invalid IPC
identities, duplicate requests, armed cancellation, three consecutive captures,
an explicitly rejected update, successful recovery, disconnect and normal exit.
Both targets remain alive after disconnect and preserve backbuffer identity;
the flip path retains its required unbinding behavior.

`tools/validate_capture_msaa_injected.py` checks every captured file against
fixed CPU rules using its own Present ordinal, executes both players, verifies
event counts and shader exports, and audits independent modules. Separate
negative copies remove either initialization Draws or the two application
modification Draws. They keep the original reference image but must produce
respectively zeroed untouched samples or the preceding frame's modified color.
The first run passes eight live files plus four negative files / 280 checks.
This validates these color profiles, not MSAA depth, query/predicate combinations,
deferred execution, multisampled swap-chain output or GF2 scene acceptance.

The source-package validator now includes a seventh `package-msaa-control`
stage. The committed `4330a64` archive passes all seven stages and 96 serial
CTests with System32-only PATH and no Qt discovery. The package produces 120
existing-profile files across 24 launches and ten MSAA files across two launches;
both MSAA sessions also pass unique-foreground automatic selection. Those ten
packaged files plus four removal controls pass 325 actual dual-player checks.
Loaded-module audits exclude Qt, Python, GPA, RenderDoc, FloraCore and FloraReplay.
Raw source-package evidence is retained in
`artifacts/capture-msaa-injected-source-4330a64`. Initial workspace evidence is in
`artifacts/capture-msaa-injected-20261014` and exact hashes/counts in the baseline.

## GF2 format-specific continuation

A new elevated launch using the independently built `4330a64` package reaches
the animated start screen and main character scene. The main-scene request
explicitly refuses at `ID3D11DeviceContext::ClearRenderTargetView` for resource
2981: format 9 (`R16G16B16A16_TYPELESS`), two samples. This identifies the next
required color initialization family; it does not prove the typed application
view, later resources or later command/state requirements are covered.

No `.gpa_frame` or `.partial` is published. Presents advance from 2566 before
the request to 3823 after refusal, with zero interface gaps. The scene remains
visible, disconnect succeeds and Alt+F4 closes the diagnostic process; the CLI
subsequently reports Exited. Graphics settings were unchanged. HUD and capture
hotkeys were disabled for this diagnostic launch. No new accepted GF2 frame or
original-player scene result is claimed: formal acceptance remains **0/6**.
Raw evidence is `artifacts/capture-gf2-msaa-20261014`, with hashes and the exact
error in the baseline's `gf2_rgba16_preflight` entry. The next implementation
step is RGBA16 sample initialization and its independent native/dual-player
checks before repeating this scene.

## RGBA16 initialization and recording

The color initialization admission table now also includes the RGBA16 family.
Runtime and CPU validation share that table. MSAA view validation compares
format families and still requires a typed view, exact format for typed source
textures, valid array bounds and matching bind flags. Cross-family views remain
errors. The existing integer-plane helper already uses the validated storage
format, integer view and actual plane bytes; no new wire layout, runtime shader
compiler dependency or Replay helper is required for this extension.

The native fixture supplies distinct 16-bit values for every component,
coordinate, layer and sample, checks captured initial planes against explicit
little-endian CPU words, then partially writes through FLOAT RTVs. The four
write values are exactly representable binary16 numbers: 1.5, -1, 2^-14 and
1365/4096. A UINT SRV display shader renders separate low-byte and high-byte
tiles for all four components into RGBA8. Therefore exact output comparison
covers all 16 bits, including alpha; it does not rely on tone mapping, reduced
precision or a relaxed floating-point tolerance.

`FloraCaptureMsaaRgba16Tests` covers two layers, 2x/4x quality zero, WARP and
hardware. It records actual context calls, releases native owners before
encoding, and checks four recording-budget refusals plus six encoder refusals
per device, including wrong row width and incompatible views. Twelve positive
and Draw-removal files pass 250 actual dual-player checks through
`tools/validate_capture_msaa_encoder.py --recorded --rgba16`. The exercised
profile is typeless RGBA16 storage, UINT seed/read views and FLOAT application
writes, not a certification of every typed member or state combination.

One new GF2 main-scene preflight with the RGBA16-enabled worktree reports its
first missing profile as format 19 (`R32G8X24_TYPELESS`), two samples, resource
3654, observed at `ClearRenderTargetView`. The bound depth/stencil dependency
still refuses; no file or partial transaction is published. Presents advance
from 2119 to 4365 with zero interface gaps. Disconnect and normal UI close
succeed. The error change identifies the next requirement but does not establish
correct replay of a complete GF2 frame. Formal acceptance remains **0/6**.
Raw evidence is retained in `artifacts/capture-msaa-rgba16-20261014` and
`artifacts/capture-gf2-rgba16-20261014`. Regression counts, hashes and pending
package scope are recorded in the baseline. Depth/stencil initialization,
broader hidden-state/query profiles and formal GF2 acceptance remain incomplete.

The committed RGBA16 revision `a2a3465` subsequently passed all seven isolated
source/package stages and 98 serial CTests. Packaged controls produced 120
existing-profile files in 24 launches and ten RGBA8 MSAA files in two launches.
This does not add RGBA16 startup-injected dual-player or GF2 acceptance.

### D32S8 initialization wire experiment

`FloraCaptureMsaaDepthEncodingTests` is a test-only writer, with no Replay
dependency. It preserves owned D32S8 sample planes through R32G32_UINT uploads,
one depth Draw and eight stencil-bit Draws per plane, then CopyResource. The
native producer instead selects whole stencil values across 256 reference
passes, providing an independent control for the bitwise initializer. Exact CPU
float32 words and stencil bytes verify initial samples and final display tiles;
unused D32S8 padding remains canonical zero.

WARP and hardware, two layers and 2x/4x quality-zero samples produce four positive
files and eight Draw-removal controls. All 306 dual-player checks and both focused
CTests pass, including actual execution, events, shaders, unchanged captured
references in negative files, native owner release and zero Debug Layer warnings.
Raw evidence: `artifacts/capture-msaa-depth-wire-20261014`. The baseline records
hashes and scope. Production depth MSAA capture still refuses; this experiment
does not change the GF2 0/6 status.

State construction follows Microsoft's [depth/stencil descriptor semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_depth_stencil_desc)
and [output-merger depth clamping and sample masks](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage).
Broader depth values and production model validation require separate checks.
