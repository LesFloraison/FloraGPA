# Independent D3D11 multisample stencil observations

`FloraMsaaStencilProbe` resolves a discrepancy encountered while validating native
DXBC multisample loads. It links only Windows D3D11, DXGI and D3DCompiler libraries.
It has no GPA, RenderDoc, capture-reader or project replay dependency.

The probe creates a device on an explicitly selected adapter, creates a four-sample
depth/stencil texture, clears depth to 0.25 and stencil to 90, and reads a typed
stencil SRV in a pixel shader. It writes the complete uint4 result into a one-pixel
RGBA32_UINT render target and reads that output back. There is no source copy or
compute shader. The output includes actual adapter vendor/device/LUID, address,
raw bits, validation status and the compiled pixel shaders. Staging polls have a
five-second deadline. The test runner additionally bounds each process to 30 seconds.

Both `X32_TYPELESS_G8X24_UINT` and `X24_TYPELESS_G8_UINT` are exercised as 2D and
array views. Samples 0 and 3 are read at an interior coordinate, positive/negative
X overflow, Y overflow and array overflow. Unused Z in a non-array view remains
ignored. Each adapter produces 40 records, with exact checks on the stencil value
and unused X/Z components.

On the local NVIDIA adapter and WARP, an interior result is `[0,90,0,1]` and a
spatial-outside result is `[0,0,0,1]`. On the local Intel adapter the corresponding
results are `[0,90,0,0]` and `[0,0,0,0]`. This difference occurs on the original
resource with an independent pixel shader; it is not introduced by the project's
private texture copy, compute observation, or GPA decoding. It is an observation
of these installed drivers, not an assumption about every device from a vendor.

An optional third argument enables `RESOURCE_CLAMP` and sets the resource MinLOD.
Runs with 0, 0.25 and 1 show that positive values empty the one-mip MSAA view on all
three local adapters, while retaining the same adapter-specific missing alpha.
The twelve runs contain 480 observations in total. A private copy used for shader
analysis must preserve both the flag and the current MinLOD; copying sample data
alone is insufficient to reproduce an SRV read.

The generic missing-component expectation in
[D3D11 instruction specification section 22.4.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
was insufficient to predict the observed Intel alpha component. A native analyzer
should retain the measured bits and record their origin. The product must not
rewrite alpha from a vendor ID, nor silently treat either comparison tool as truth.
Out-of-range sample indices are a separate undefined case and are not used as an
oracle here.

Reproduce with a newly named output directory for each run:

```powershell
cmake --build build/vs2022 --config Release --target FloraMsaaStencilProbe
build/vs2022/Release/FloraMsaaStencilProbe.exe artifacts/probe-warp 0
build/vs2022/Release/FloraMsaaStencilProbe.exe artifacts/probe-nvidia 4318
build/vs2022/Release/FloraMsaaStencilProbe.exe artifacts/probe-intel 32902
build/vs2022/Release/FloraMsaaStencilProbe.exe artifacts/probe-intel-clamp 32902 0.25
```

Run adapters serially. Missing adapters and D3D11 validation errors fail explicitly.
Raw outputs and shader binaries stay outside Git. Their identities and the probe
source identity are retained in `research/dx11-msaa-stencil-evidence.json`.
This probe establishes a narrow hardware observation only; it does not establish
full shader-debugger or release parity.
