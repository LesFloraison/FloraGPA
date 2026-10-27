# Private multisample pixel output merger

This extends the single-sample building block documented in
`DX11_NATIVE_PIXEL_MERGER.md`. It advances P5 without changing public history or
pixel-debug routing. Complete fragment qualification/order, original early-depth
semantics, public PS integration and the P7/P8 removal gates remain open.

## Storage and execution

`native-pixel-merger/2` accepts a sample description and sample-major native
color/depth texels. Each sample is uploaded and read separately through the
project's shared native MSAA transfer routines. Initialization must round-trip
exactly before the private draw executes. Reads select samples explicitly and
never substitute a resolve. The operation uses a separate device on the verified
adapter and retains bounded completion and cancellation checks.

The supplied raster coverage mask is intersected with the original output-merger
sample mask. Shader `SV_Coverage`, alpha-to-coverage, dual-source blending, color
write masks, depth/stencil operations and read-only DSV flags are then applied
by D3D11. The result records both masks, effective mask, sample count/quality,
storage order and private pixel location. Occlusion remains a sample count,
not a fragment identity. The private location is `(0, 0)`; spatially dependent
alpha-to-coverage behavior is not evidence about the original frame position.

The caller still supplies observed post-shader outputs and raster depth where
needed. This API does not recover original coverage, sample-frequency invocation
identity, conservative/early-depth optimization, primitive order or cross-quad
side effects. Original fragment eligibility remains null. It does not allocate
a spare original-shader UAV slot.

D32S8 X24 padding is explicitly invalid and canonically zero in sample storage.
The result's invalid-byte offsets apply within each texel. Existing MSAA transfer
restrictions, including unsupported packed-format NaN payload uploads, remain
explicit errors. The format extension does not claim those unknown bytes as
recovered data or narrow existing public backend support.

## Repeated-copy failure and allocation fix

Development testing exposed device removal on the local Intel adapter when
repeatedly inspecting typed MSAA color resources through typeless integer views.
The initial 138-case run passed all 92 WARP/NVIDIA cases and failed its Intel
cases; later Intel adapter-enumeration failures followed device removal in that
same process. Those are retained as failed development results, not unsupported
hardware skips. Subsequent phase annotations identified failures when reading
the reference resource again after the private diagnostic.

Independent D3D11 controls isolated repeated typed-to-typeless copies. Changing
copy binding flags, using immediate copies, unbinding the source, using an
intermediate typed clone or using whole-subresource region copies did not provide
a complete fix. Some first reads passed; that was insufficient for repeated
inspection. Typeless source allocation passed repeated clears and inspections.

Replay-owned MSAA resources in the existing integer-access families now allocate
their typeless family format from creation. The captured resource descriptor and
typed views retain their interpretation; views inheriting `DXGI_FORMAT_UNKNOWN`
use the recorded typed format. Experiment clones follow the same rule. Formats
outside those families retain their allocation format. Native transfer command
lists still restore context state, and replay retains its predicate guards.
The production fix does not change devices or use a typed conversion as a
substitute for raw storage.

This is an observed driver-path failure, not a claim that cross-family-compatible
MSAA copies violate D3D11. Microsoft's
[CopyResource contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
permits compatible format groups with matching sample count and quality. The
standalone control identifies the tested adapter; the evidence manifest records
the local driver version. No conclusion about every Intel driver is inferred.

## Validation and reproduction

The merger matrix adds 144 cases across NVIDIA, Intel and WARP at 2x/4x, quality
zero. Cases cover sample masks, every selected sample, unchanged writes,
dual-source blending, alpha-to-coverage, shader coverage/discard, sequential
sample updates, depth failure, stencil, read-only views, D16/D24S8/D32/D32S8,
half-float, sRGB/BGRA and inherited view formats. Reference storage is re-read
after the diagnostic, then compared against a separate original-shader replay.
The existing 546 single-sample cases retain their assertions.

`validate_native_pixel_merger.py --msaa` verifies matrix completeness, provenance,
storage lengths, explicit unknown bytes, mask identities, untouched samples and
raw reference comparisons. Independent CPU checks calculate float blending,
write masks, depth/stencil changes and deterministic sample counts. It rejects
1,584 corrupted reports. The unchanged single-sample checks reject 3,276 more.
Alpha-to-coverage checks retain its adapter-dependent mask rather than inventing
a portable per-sample coverage pattern.

The old blend test oracle needed an explicit captured typed SRV once backing
storage became typeless. Its shader, expected values and numeric assertions are
unchanged. Its initial default-SRV failure is retained with the development
logs. No golden values, assertions or skip conditions were weakened.

The final eleven distinct CTest suites pass. Their eleven Qt suites contain
1,055 passing checks, with zero failures or final skips. The 195.25-second batch
initially skipped 50 original-corpus cases because their environment paths were
unset. A 10.25-second supplemental run closes those cases using the existing
predicate corpus and newly generated texture corpora. The 24 creation and 11
copy captures verify 840 producer frames across native and original-GPA runs.
The original missing-corpus run remains in the evidence. Merger CTest time is
125.73 seconds for 690 GPU matrix cases plus boundary/lifecycle checks; it is not
a per-request performance benchmark.

The standalone typeless path passes 216 readbacks across the three adapters;
typed controls pass another 144 on WARP/NVIDIA. The final Intel typed control
returns four zero half words on its first read and fails the exact comparison
immediately. Earlier exploratory runs continued through device removal; the
final probe stops at the first mismatch. Intel driver version is 32.0.101.8826,
NVIDIA is 32.0.16.1742. These outcomes are recorded separately from required
success tests. Static imports of the probe contain only Windows graphics/runtime
libraries; this is not a dynamic product module audit.

Build the normal test targets and the independent `FloraMsaaStorageProbe` target.
The probe links only Windows D3D11/compiler/DXGI libraries. Each successful run
checks 72 raw half-float sample readbacks across 2x/4x, quality zero/standard,
three different clears and two inspections per clear. For example, from the
repository root in PowerShell, using new output paths:

```powershell
build/vs2022/Release/FloraMsaaStorageProbe.exe artifacts/msaa-warp warp typeless deferred srv
build/vs2022/Release/FloraMsaaStorageProbe.exe artifacts/msaa-nvidia hardware typeless deferred srv
build/vs2022/Release/FloraMsaaStorageProbe.exe artifacts/msaa-intel intel typeless deferred srv
```

The `typed` probe mode is an opt-in failure reproducer and can remove the device
on affected drivers; it is not a required success test. `immediate` and `srv-rt`
select the independent copy/binding controls. GPU probes and tests run serially.

```powershell
$env:FLORA_PIXEL_MERGER_EVIDENCE = 'D:/new-merger-evidence'
ctest --test-dir build/vs2022 -C Release -R '^native_pixel_merger$' -j 1 --output-on-failure
python tools/validate_native_pixel_merger.py D:/new-merger-evidence --out D:/single-validation.json
python tools/validate_native_pixel_merger.py D:/new-merger-evidence/msaa --msaa --out D:/msaa-validation.json
```

To regenerate the optional original texture regression corpora during research,
build `FloraTextureCreationProbe` and `FloraTextureCopyProbe`, then use new paths:

```powershell
python tools/capture_texture_creation.py --producer build/vs2022/Release/FloraTextureCreationProbe.exe --out artifacts/new-creation-corpus
python tools/capture_texture_copies.py --producer build/vs2022/Release/FloraTextureCopyProbe.exe --out artifacts/new-copy-corpus
$env:FLORA_TEXTURE_CREATION_CAPTURES = "$PWD/artifacts/new-creation-corpus"
$env:FLORA_TEXTURE_DIMENSION_CAPTURES = $env:FLORA_TEXTURE_CREATION_CAPTURES
$env:FLORA_TEXTURE_COPY_CAPTURES = "$PWD/artifacts/new-copy-corpus"
$env:FLORA_INITIAL_PREDICATE_CAPTURES = "$PWD/artifacts/dx11-native/native-compute-predicate-corpus"
ctest --test-dir build/vs2022 -C Release -R '^(texture_creation|texture_copies|normalized_predication)$' -j 1 --output-on-failure
```

Generation invokes the local GPA shim as a development reference; replaying the
saved captures does not. The existing predicate corpus and its generation are
documented in `../DX11_NATIVE_COMPUTE_DEBUG.md`. Required tests must not silently
treat an unset corpus path as an exercised original-capture case.

`dx11-native-pixel-merger-msaa-evidence.json` records final builds, tests, probes,
source/binary identities and artifact hashes. These measurements validate this
internal extension; they are not the P8 standalone product, latency, memory or
dynamic module audit. Original GPA, RenderDoc and large binary evidence are not
required by these owned-fixture tests and are not committed with this work.
The manifest pins implementation `66b386fd20d8fbb33c31e7317c5d1db3a75247c9`
and 3,173 raw artifacts. GUI, CLI and Worker build successfully in Release.
