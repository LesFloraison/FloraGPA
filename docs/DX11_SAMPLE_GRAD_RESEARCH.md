# Independent explicit-gradient sampling observations

The public D3D11 probe now accepts an optional `--grad` argument. It compiles
`Texture2D.SampleGrad` independently and dispatches it directly, without the
project's replay or executor, RenderDoc, or Intel GPA private modules. Existing
explicit-LOD invocations remain unchanged. The refreshed WARP explicit-LOD
report is byte-for-byte identical to the previous observation.

The gradient matrix contains 63 inputs for each of 34 sampler/view configurations
on WARP, NVIDIA and Intel: 6,426 observations, with no D3D11 debug-layer errors.
Seven coordinates combine with nine gradient pairs: zero, subtexel, one-texel,
two-texel, full-texture, negative, two anisotropic orientations and a skewed pair.
Each configuration runs with and without a constant texel offset. Sampler bias,
clamps, resource MinLOD, view mip ranges, addressing, min/max filters and null
bindings retain the independent explicit-LOD probe's coverage.

The [D3D11 specification, section 22.4.17](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines explicit normalized-coordinate derivatives and per-invocation LOD
calculation. The executor must preserve these operands and execute native
`sample_d`; reducing gradients to a CPU-computed scalar LOD would lose anisotropy
and implementation-specific filtering behavior. This work does not establish
implicit quad derivatives or NaN/Inf behavior.

`tools/summarize_sample_level.py --grad` validates the complete input grid, shader
instruction, requested and observed state, output range, null bindings and
analytical axis-aligned point filtering. Skewed LOD is preserved as observation
rather than forced into a single approximation. The validator also still passes
the original 6,426 explicit-LOD observations.

Fourteen adapter/configuration rows differ from WARP. NVIDIA and Intel each have
29 anisotropic differences per offset and seven linear/null-sampler differences
per offset. Intel additionally has 42 fractional-MinLOD differences per offset.
These are observations, not a claim that a particular driver is the specification
oracle. Exact comparisons for the executor must use the same adapter.

Reproduction:

```powershell
cmake --build build/vs2022 --config Release --target FloraSampleLevelProbe
# Create the parent once; each probe output directory must be new.
foreach ($vendor in @(0, 4318, 32902)) {
    build/vs2022/Release/FloraSampleLevelProbe.exe "artifacts/dx11-native/sample-grad-observed/$vendor" $vendor --grad
}
python tools/summarize_sample_level.py --grad --root artifacts/dx11-native/sample-grad-observed --binary build/vs2022/Release/FloraSampleLevelProbe.exe --out docs/research/dx11-sample-grad-evidence.json
```

The evidence ledger records source, shader, binary and report hashes. Raw reports
and compiler disassembly remain outside Git. A first build failed because the
conditional HLSL string lacked a concatenation operator; the corrected build and
the original diagnostic remain in `artifacts/dx11-native/sample-grad-probe-build*.txt`.
This research checkpoint does not complete shader debugging or release acceptance.
