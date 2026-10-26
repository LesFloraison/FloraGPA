# Lossless native pixel quad transport

This experiment establishes a way to export all four lanes' arbitrary 32-bit
register values through a covered pixel without a pixel-shader UAV. It is a
prerequisite for native PS input acquisition. It does not capture actual GPA
shader input signatures, select an original fragment, recover the Intel GPA
debugger engine, or expose a completed native pixel debugger.

## Why the existing counter is insufficient

The existing native Quad diagnostic measures coverage and serialized geometry.
Its reports do not provide the four initial register maps required by the
independent `PixelQuad` executor. Nonwriting lanes cannot simply export their
values to a render target or UAV. Reconstructing arbitrary floats by subtracting
their original floating-point values would lose raw bits, especially for NaNs,
infinities, denormals, signed zero and values with widely differing magnitudes.

Microsoft's [fine derivative contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/deriv-rtx-fine--sm5---asm-)
identifies row/column differences within the current quad but does not promise
an even screen-coordinate quad origin. The
[rasterization rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules)
describe extra pixel invocations needed for derivatives. Neither document alone
proves that a particular captured neighboring input is valid or that a
nonwriting lane has a particular helper classification.

## Independently derived transport

Each uint32 component is split into four bytes. A byte converted to float32 is
exact, as are the small integer differences and sums used here. For one byte
vector `b`, compute uniformly before any original shader control flow:

```text
dx  = ddx_fine(b)
dy  = ddy_fine(b)
dxy = ddx_fine(dy)
target = b + deltaX * dx + deltaY * dy + deltaX * deltaY * dxy
```

`deltaX` and `deltaY` are the target lane's row/column coordinates minus the
current lane's coordinates, each in {-1, 0, 1}. Convert the reconstructed byte
back to uint and shift/OR the four bytes. All intermediate magnitudes are small
enough for exact float32 integer arithmetic. Original bit patterns never enter
floating-point arithmetic directly.

The probe measures current lane direction rather than deriving it solely from
screen parity. Let `p` be the pixel-coordinate parity in one axis and `d` its
native fine derivative. Then `d * (1 - 2*p)` is +1 for the left/top lane and -1
for the right/bottom lane. This handles either parity of the quad origin. The
probe exports the measured direction and role, checks unit direction and zero
cross-axis derivatives, and reconstructs neighboring coordinates from the
measured role. The tested coordinates are positive and inside the fixture
domain; negative/outside-viewport helper positions remain an integration concern.
All observed native origins happened to be even on these three adapters; that
observation is not promoted to an API guarantee or an assumed input identity.

Four UINT render targets carry the neighboring uint4 values. Four additional
diagnostic targets retain location/role, sample IDs, coverage masks and direction
derivatives. The capture shader has no UAV declaration. A later private compute
pass reads MSAA storage into a CPU-readable buffer; that UAV is not used by the
pixel capture or drawn from an application's available slots.

## Probe and independent validation

`tools/native/pixel_quad_transport.hlsl` and
`tools/native/pixel_quad_transport_probe.cpp` use public D3D11 interfaces only.
The program creates its own device on the requested adapter and enables D3D11
validation. GPU completion has a five-second deadline per readback. Output
directories must be new. No original capture or installed GPA file is changed.

The 128 cases per adapter cover 1/2/4/8 samples, pixel/sample-frequency execution,
two raw-bit datasets, full and one-pixel scissors, four current lane roles,
small/edge triangles, sample masking and a fractional viewport origin. Source
register values are defined by a constant-buffer fixture indexed by observed
pixel position and sample ID; they are not original interpolated GPA inputs.
Datasets include signed zero, denormals, infinities, NaN payloads, integer patterns
and deterministic random uint32 values.

An independent reference shader writes the current value directly without
derivatives. Capture and reference write footprints must match. The current
lane must equal that direct reference exactly. All four reconstructed neighbors
must equal independently computed fixture words, including neighbors that do
not write a target sample. Actual sample identity, sample-mask permission,
coverage of writing samples, measured roles and native diagnostic errors are
checked separately. A nonwriting neighbor is recorded as such, not automatically
labeled an original captured helper lane.

The probe saves constant buffers, all nine targets' raw sample storage, HLSL,
DXBC, disassembly, adapter/driver identity, diagnostic counts and loaded modules.
`tools/validate_pixel_quad_transport.py` independently checks the complete case
matrix and original bytes, regenerates the source fixture, verifies every
neighbor word and current reference, audits pixel UAV declarations and modules,
and hashes the artifacts. Its negative controls alter neighbor bits, quad role,
orientation, sample identity, direct reference and the input fixture in memory;
the original evidence files remain unchanged.

```powershell
cmake --build build/vs2022 --config Release --target FloraPixelQuadTransportProbe
foreach ($vendor in @(4318, 32902, 0)) {
  build/vs2022/Release/FloraPixelQuadTransportProbe.exe `
    "artifacts/dx11-native/quad-transport-audited-$vendor" $vendor `
    tools/native/pixel_quad_transport.hlsl
  if ($LASTEXITCODE -ne 0) { throw "Quad transport probe failed" }
}
python tools/validate_pixel_quad_transport.py `
  artifacts/dx11-native/quad-transport-audited-4318 `
  artifacts/dx11-native/quad-transport-audited-32902 `
  artifacts/dx11-native/quad-transport-audited-0 `
  --out artifacts/dx11-native/quad-transport-audited-validation.json
```

All GPU work runs serially. NVIDIA, Intel and WARP each pass 128 cases with no
unsupported sample counts: **384 cases**, **297,024 exact neighbor components**,
**12,492 nonwriting-neighbor observations**, and **18 rejected corruptions**.
Runtime module and executable import audits find no RenderDoc or private GPA
dependency in this independent probe. This is not a product/package module audit.

The first WARP run reported 96 failed cases because the orientation diagnostic
required positive-zero bits for cross-axis derivatives. Native results also
contained negative zero. All raw neighbor words already agreed. The corrected
diagnostic requires numeric zero for that computed float quantity and retains
its original raw bits; arbitrary input/output words still compare exactly.
No prior golden, product assertion or skip rule changed. The initial report and
representative raw records are retained in `dx11-pixel-quad-transport-evidence.json`.

## Next implementation gates

- Transform effective original PS declarations/signatures into input-copy passes,
  retaining interpolation, minimum precision, system inputs and sample-frequency
  triggers. Cover full signatures without a spare application UAV requirement.
- Select and isolate the required original primitive/fragment with private
  resources, correct depth/stencil/discard/sample behavior and restored state.
- Preserve observed quad origin, input validity and coverage provenance when
  populating `PixelQuad`; unknown data must not be synthesized as valid input.
- Validate actual GPA draws and original native shader output, including source
  debugging, divergent flow and helper memory behavior, then connect journals,
  Worker jobs and GUI. Full Pixel History and P7/P8 gates remain open.
