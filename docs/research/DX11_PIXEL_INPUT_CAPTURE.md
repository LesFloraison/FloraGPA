# Original PS input declaration transport

This checkpoint adds `pixelInputShader` in `DxbcPixelInputs`. It transforms a
selected SM4.0, SM4.1 or SM5 pixel input register into an SM5, resource-free,
four-lane observation shader. The product's native `debug-pixel` action is not
implemented by this checkpoint. Actual GPA fragment selection, private geometry
bridge integration, input provenance and helper validity remain required.

## Input and output contract

The transformer reads ISGN/ISG1 and the executable declarations. It copies the
selected register's semantic names, semantic indices, component types, component
masks, interpolation modes and minimum-precision declarations. Multiple semantics
packed into one register remain separate signature rows. Missing components are
zero-filled transport padding with a clear validity bit; they are not fabricated
input observations. Invalid or absent registers, conflicting chunks, malformed
signatures and reserved minimum-precision values are rejected.

Only the selected register is retained as private v0. Auxiliary position and
sample-index inputs occupy separate registers when necessary. This works even
when the original shader uses all 32 input registers. The returned compact
`inputSignature` is a contract for a private pre-raster geometry bridge. D3D11
stage linkage checks hardware registers as well as semantics: binding the helper
against an unmodified original producer is invalid. Tests use separate, explicitly
ordered producer signatures and check D3D11 debug-layer errors. Integration with
the existing final-geometry facilities is still pending.

The helper retains sample-frequency execution if **any** original declaration
uses sample interpolation or SV_SampleIndex, including a declaration outside the
selected register. Selected input interpolation is not changed to achieve that:
an explicitly used auxiliary SV_SampleIndex forces the required execution rate.
If position is selected but the original program only reads z/w, xy is added for
orientation while its original validity mask remains unchanged.

UINT MRTs 0-3 carry the four row-major quad lanes' uint4 words. MRT 4 contains
signed pixel coordinates in raw uint32 form and measured quad role; MRTs 5-7
contain sample identities, coverage and raw orientation derivatives. Byte-wise
fine-derivative transport follows the preceding
[independent experiment](DX11_PIXEL_QUAD_TRANSPORT_RESEARCH.md). Signed floor
coordinates avoid truncating negative coordinates toward zero; this checkpoint's
GPU validation region remains positive and does not establish negative/helper
boundary behavior. Consumers must validate orientation and retain unknown data.

No original instructions, resources, class instances, UAV access, discard or
depth behavior execute in the helper. Observation of an input is not evidence
that the original fragment passed depth, stencil or discard. Minimum-precision
inputs are captured as the device exposes them through a full-precision move;
this does not redefine the original shader's later precision semantics.

## Independent validation and corrections

GPU tests cover linear, noperspective, centroid, noperspective centroid, sample,
noperspective sample, nointerpolation, uint, sint, sparse x/z, min16float,
external sample-index triggering, position, position-z, sample-index selection,
SM4.0 and SM4.1. Each runs on NVIDIA, Intel and WARP with one and four samples.
Original and compact producers compute the same geometry/attributes. Selected
current-lane components must exactly equal a direct original HLSL output.

A second original-declaration HLSL shader independently gathers each of 32 bit
planes, first horizontally and then vertically. It uses neither byte
decomposition nor a mixed derivative. All four captured lanes must exactly match
this oracle's words. No floating-point tolerance is used. The oracle's sample
index is explicitly written to prevent the compiler removing its execution-rate
trigger. Input-free registers are not silently synthesized.

The tests also create every selected register from full 32-register signatures,
including 64 float2 semantics packed into 32 registers, on all three adapters.
This establishes declaration/creation support, not GPU execution of an arbitrary
32-register application shader. Additional checks cover an unselected sample
interpolation trigger and reserved precision rejection.

The final input suite passes 113 checks with no failures or skips. Existing
`dxbc_pixel_quad` and `dxbc_pixel_sampling` suites pass another 1,022 and 168
checks respectively (1,303 total). GUI, CLI and Worker Release targets build.
See `dx11-pixel-input-capture-evidence.json` for immutable report identities.

Earlier failures are retained under `artifacts/dx11-native/`:

- `pixel-input-first-build.txt`: a C++ auto declaration mixed size_t and uint32.
- `pixel-input-first-test.txt` / `pixel-input-diagnostic-test.txt`: a constant
  interpolation template allowed the compiler to remove derivatives before
  declaration replacement. Current lanes matched while neighbor lanes failed.
  The template now starts with varying float input and bit reinterpretation.
- `pixel-input-varying-tests.txt` / `pixel-input-exception-test.txt`: the debug
  layer exposed incompatible producer/consumer register linkage. The fixture
  now supplies an explicit compact producer and the API exposes that contract.
- `pixel-input-linkage-tests.txt` / `pixel-input-centroid-test.txt`: WARP centroid
  neighbors disagreed with another pixel's same-sample output. The diagnostic
  showed distinct coverage groups (for example mask 3 versus samples 2/3), with
  every directly observed current input correct. A neighboring pixel's separate
  invocation is not an oracle for the current quad's invocation. The new bit-plane
  shader compares within the same quad, with exact assertions. The six min16
  fixture compile errors required an explicit float4 promotion before asuint.
- `pixel-input-oracle-tests.txt`: a fixture variable shadowed an input parameter.
- `pixel-input-oracle-fixed-tests.txt`: Intel exposed an unused sample-index
  parameter optimized out of the oracle. Writing that index preserves its
  original sample-frequency trigger; the exact assertions remain.

The original logs and earlier observations are not overwritten. These are
development corrections, not modifications to old release goldens or skips.

## Reproduce

Build `FloraDxbcPixelInputsTests` in the Release configuration. Set
`FLORA_PIXEL_INPUT_EVIDENCE` to a new directory, then run the test executable or
CTest's `dxbc_pixel_inputs` test. Each GPU case saves the original/compact VS,
original/helper/oracle PS, disassemblies, original/oracle HLSL, direct output,
four oracle buffers, eight helper buffers and adapter/module metadata. Large
observations stay outside Git. GPU tests are serial and readbacks use the existing
five-second bounded completion mechanism.

```powershell
$env:FLORA_PIXEL_INPUT_EVIDENCE = 'D:/CDXrepo/FloraGPA-Cpp/artifacts/dx11-native/pixel-input-new-evidence'
& build/vs2022/Release/FloraDxbcPixelInputsTests.exe -o pixel-input-tests.txt,txt
python tools/validate_pixel_input_capture.py artifacts/dx11-native/pixel-input-new-evidence --out artifacts/dx11-native/pixel-input-new-validation.json
```

The offline validator requires the complete 102-case matrix, compares 129,600
direct and 518,400 quad components exactly, checks orientation/sample/coverage,
audits loaded modules and helper resource declarations, hashes saved artifacts,
and rejects six independent in-memory corruptions. Its evidence does not audit
the complete product/package; legacy RenderDoc consumers still exist.

Microsoft references: [pixel input declarations](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dcl-input),
[fine derivatives](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/deriv-rtx-fine--sm5---asm-),
and [D3D11 rasterization rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules).
The transformation and experiments are independently implemented; they do not
claim recovery of the original GPA debugger engine.
