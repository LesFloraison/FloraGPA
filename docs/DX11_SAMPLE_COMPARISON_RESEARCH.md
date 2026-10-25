# Native comparison sampling research

This P6 checkpoint investigates `sample_c_lz` using only public D3D11 calls and
independently compiled HLSL. It does not accept complete shader debugging or
standalone release parity.

The instruction compares a reference against each sampled red component before
filtering the comparison outcomes. Its scalar output is replicated to masked
destination components. It starts with zero LOD, leaving sampler/view/resource
LOD controls to the native implementation. Implicit-LOD `sample_c` still needs
pixel-quad context and is a separate implementation requirement.
References: [Microsoft sample_c_lz](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-c-lz--sm4---asm-)
and [Microsoft sample_c](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-c--sm4---asm-).

`FloraSampleCompareProbe` takes a new output directory, an explicit adapter
vendor (`0` for WARP, `4318` for NVIDIA, `32902` for Intel), and a stage (`cs`,
`ps`, `vs`, or a stage followed by `-single`). It never falls back to another
adapter. It saves DXBC, disassembly, actual adapter identity, requested and
observed sampler descriptors, exact input/output bits and diagnostics. Each
readback has a five-second deadline. No RenderDoc, replay library or GPA private
module participates.

The texture is an 8-by-8 R32_FLOAT image with four mips. A texel is
`((x + 2*y + 3*mip) % 8) / 8`. The 63 input records combine seven horizontal
coordinates with nine references, including positive infinity and a quiet NaN
whose exact payload is retained. Twenty-eight configurations cover all eight
comparison functions under point and linear filtering, bias, sampler LOD limits,
resource MinLOD, a restricted view, four additional addressing modes, anisotropy,
an unbound sampler and an unbound resource. Each has zero and `(-1,1)` offsets.

Both invocation modes run in all three stages on all three adapters. The ledger
records 61,236 defined result vectors and 2,268 undefined-result observations.
Every defined result is a replicated scalar in [0,1]. All 18,144 base point-filter
results match analytical texel selection and IEEE comparison, including NaN and
infinity inputs. Batch and individual invocation results agree for every defined
case. Unbound resource results are all zero with a valid comparison sampler.

An unbound sampler is different: D3D11 reports
`D3D11_MESSAGE_ID_DEVICE_DRAW_SAMPLER_MISMATCH` (390), because the default sampler
does not enable comparison filtering. The probe records those outputs as
undefined and accepts only that exact expected diagnostic for those cases.
They are not numerical oracles. All defined cases must have zero error
diagnostics. The executor must reject a missing or incompatible comparison
sampler explicitly, without treating incidental driver results as valid data.

Reproduce the shader target and run GPU commands serially. For example:

```powershell
cmake --build build/vs2022 --config Release --target FloraSampleCompareProbe
& build/vs2022/Release/FloraSampleCompareProbe.exe new-output-directory 0 cs
```

The full run uses `artifacts/dx11-native/sample-compare-final-{batch,single}` with
`stage/vendor/results.json` below each root. Validate with:

```powershell
python tools/summarize_sample_comparison.py `
  --batch artifacts/dx11-native/sample-compare-final-batch `
  --single artifacts/dx11-native/sample-compare-final-single `
  --binary build/vs2022/Release/FloraSampleCompareProbe.exe `
  --out artifacts/dx11-native/sample-comparison-summary.json
```

The checked-in observation ledger is
`research/dx11-sample-comparison-observations.json`. Raw outputs and binaries stay
outside Git. Exact execution comparisons, source modifiers, destination masks,
unknown input validity, format/dimension constraints and cancellation are the
next implementation gates. These observations do not establish those gates.
