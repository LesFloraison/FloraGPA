# Native explicit-LOD sampling observations

This P6 research checkpoint establishes a hardware observation baseline for
`sample_l`. It does not implement texture sampling in the interpreter or close
the shader-debugging acceptance matrix. `FloraSampleLevelProbe` links only the
public D3D11, DXGI and compiler libraries; it does not load GPA or RenderDoc.

## Method

The probe selects WARP or the requested hardware vendor explicitly, enables
D3D11 validation, compiles two compute shaders and saves their DXBC and
disassembly. Each shader samples a four-mip 8x8 RGBA32_FLOAT texture using 63
coordinate/LOD combinations. Texels encode the mip, x and y independently.
One shader has no offset; the other uses the immediate offset (-1, 1).
Seventeen configurations cover point/linear/anisotropic/minimum/maximum
filtering, bias, sampler and resource clamps, restricted view mips, five
address modes, an unbound sampler and an unbound resource. Readback is bounded
to five seconds per dispatch. No adapter substitution is permitted.

The [D3D11 instruction specification, section 22.4.18](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines explicit-LOD sampling, including sampler bias and clamps. The
[SampleLevel assembly reference](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-l--sm4---asm-)
describes operands and offsets. The
[sampler binding reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetsamplers)
specifies the default state used for a null sampler. These references guide
independent analytical checks; one adapter's output is not treated as truth
for another adapter.

## Observed results

All three local devices support all 34 configurations, producing 6,426 raw
float-bit observations with zero D3D11 validation errors. Point sampling and
point sampling with bias +2 pass exact analytical checks for all coordinates,
LODs and offsets. Null resources return zero. Null samplers match the native
linear/clamp configuration throughout this matrix.

Two cross-adapter differences remain explicit:

- NVIDIA anisotropic sampling at LOD 0.25 returns mip-0 values where WARP and
  Intel interpolate between mips 0 and 1. Seven coordinates differ for each
  offset mode. This is a mip-selection difference, not a rounding tolerance.
- With point filtering and sampler MinLOD 1.5, Intel selects mip 1 for requested
  LODs below 1.5, while WARP and NVIDIA select mip 2. At a directly requested
  LOD of 1.5 all three select mip 2. There are 35 differing records per offset.

No vendor-specific correction is inferred. A native helper must retain the
actual adapter, sampler descriptor, SRV mip range and resource clamp. These
observations do not justify replacing filtering with one CPU formula.

`GetDesc` canonicalizes unused sampler fields on all three devices: it returns
zero MaxAnisotropy for non-anisotropic filters and zero border color when no
axis uses border addressing. The ledger retains both requested and observed
descriptors. Validation permits only those zeroed unused fields and rejects
changes to active fields. A snapshot must preserve observed state, rather
than demand byte equality with the creation descriptor.

An initial observation run incorrectly queried `GetResourceMinLOD` on resources
without `D3D11_RESOURCE_MISC_RESOURCE_CLAMP`; the debug layer rejected that
fixture. The corrected probe calls the getter only for clamp-enabled resources
and records null otherwise. The failed console log remains outside Git.

## Reproduction and limits

Build target `FloraSampleLevelProbe` in Release. For each vendor 0 (WARP), 4318
(NVIDIA), and 32902 (Intel), run it serially with a new output directory and the
vendor number. Then run:

```text
python tools/summarize_sample_level.py --root artifacts/dx11-native/sample-level-observed --binary build/vs2022/Release/FloraSampleLevelProbe.exe --out docs/research/dx11-sample-level-evidence.json
```

The committed ledger records source, executable, shader and report hashes,
adapter identities, compact configuration results and differing examples.
Large raw reports and binaries remain outside Git. The validator checks the
complete grid, descriptor semantics, analytical point results and unbound
behavior; deliberately corrupted point output must fail validation.

This matrix covers finite coordinates on RGBA32_FLOAT 2D textures in compute
shaders. It does not establish cube seams, other formats or stages, implicit
derivatives, comparison sampling, sparse validity footprints, helper lanes or
full application parity. Those remain implementation and acceptance work.
