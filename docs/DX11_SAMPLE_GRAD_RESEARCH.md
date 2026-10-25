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

## Stage-specific follow-up

The first executor comparison found two failures when its PS results were compared
to the compute-only oracle. The standalone probe now also accepts `--grad ps` and
`--grad vs`. It uses a rasterized fullscreen triangle with an integer render
target for PS, and vertex-ID selection plus stream output for VS. Both use
independently compiled HLSL, the same texture/state matrix and debug-layer checks.
There are now 19,278 independent observations across three stages and adapters;
each stage has a separate evidence ledger and explicit stage identity.

Only WARP has cross-stage differences in this matrix: eight anisotropic records
per offset differ between PS and CS/VS. The executor's initially failing PS
values match the independent PS values exactly. For example, at coordinates
`(-1.25, 0.375)` with gradients `(-0.25, 0)` and `(0, -0.25)`, the texture's blue
channel is raw `1040187392` in PS and `1040154624` in CS/VS without an offset.
This is evidence to compare the same native stage, not a reason to weaken the
bit-exact assertions or replace the shader stage with CS. The full initial
154-pass/two-failure executor log and focused mismatch diagnostic are retained.

Run the same validator with `--stage ps` or `--stage vs` and the corresponding
output root. The updated CS, PS and VS ledgers are
`research/dx11-sample-grad-{cs,ps,vs}-evidence.json`; the original compute-only
ledger remains intact as historical evidence. These observations do not resolve
all driver-dependent or nonfinite-input filtering behavior.
