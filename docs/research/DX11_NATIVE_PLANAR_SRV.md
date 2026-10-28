# Native planar shader-resource views

The native DXBC executor accepts NV12, P010 and P016 Y/UV shader-resource
views for texture loads, explicit sampling, gathers and resource information.
This closes the previous nonzero-plane rejection in the private GPU helper.
It does not qualify planar UAV operations, incomplete legacy capture data,
original pixel invocations or the full shader-debugging parity gate.

## Storage and execution contract

The implementation resolves explicit `PlaneSlice` and legacy typed-format
selection to the same validated plane. NV12 uses R8/R8G8 UNORM or UINT views;
P010/P016 use R16/R16G16 UNORM or UINT views. Chroma dimensions are half the
resource width and height, with the original row pitch and an offset after Y.
Invalid format/plane pairs, odd dimensions, multisampling, multiple mip levels
and non-2D planar descriptors fail explicitly.

The private GPU creates a resource with the original planar storage format and
dimensions on the selected adapter. Its legacy SRV creation selects the plane by validated typed format.
The independent oracle uses `CreateShaderResourceView1` and checks `GetDesc1`
for explicit selections; legacy inferred selections use `CreateShaderResourceView`.
There is no conversion to RGBA, substitute GPU or fallback backend.

Load validity now uses the selected plane's byte offset and spatial bounds.
Sampling conservatively requires all bytes of the selected plane in the view's
layers to be known. Missing bytes in an unrelated plane do not taint the result.
Dimensions report the selected plane, including array view ranges. GPU-assisted
dimension queries declare the actual typed return class instead of always UINT;
the old declaration was rejected by the D3D11 validation layer for UNORM views.
The helper identity advances to `dxbc-gpu/25`.

Microsoft documents the typed plane mappings and half-sized chroma views in
[DXGI_FORMAT](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format),
and explicit selection in
[D3D11_TEX2D_SRV1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/ns-d3d11_3-d3d11_tex2d_srv1).
P010's low six bits are not forced to zero by this implementation; the authored
fixtures intentionally retain nonzero low bits and compare actual GPU results.

## Independent evidence

`DxbcPlanarTests` runs 216 configurations: WARP/NVIDIA/Intel, VS/PS/CS,
NV12/P010/P016, Y/UV, UNORM/UINT and single/array views. Four modes exercise
full/partial destination writes, explicit/inferred plane selection, spatial
or mip out-of-bounds queries, reciprocal/float/uint dimensions, source swizzles,
gather components and shifted array views. UINT views use load and dimension
instructions; UNORM additionally uses `sample_l` and `gather4`.

The original authored DXBC runs directly on D3D11, independently of the CPU
executor and private helper. Results use raw VS stream output, a PS UINT target
or a CS structured UAV. Each result checks unknown bytes in both the selected
and unrelated planes and preserves the source byte vector.

On the tested adapters, 192 configurations execute 2,304 original-shader
comparisons. The remaining 24 are NVIDIA P016 rejection contracts: format
support lacks `D3D11_FORMAT_SUPPORT_TEXTURE2D`, `CreateTexture2D` returns
`E_INVALIDARG`, diagnostic 92 reports an unsupported format, and the native
helper rejects the same snapshot without completing a GPU operation. These
are device capability observations, not shader comparisons or skipped tests.

Twenty-four NV12 compute configurations also save and reload a GPA frame,
observe its actual bound SRV and resource bytes, execute a separate full GPU
replay, and compare native group execution. GPA event 100, shader 15, resource 4,
view 6 and observed plane identity are retained, with the frame SHA-256. Source
texture bytes remain unchanged. These are synthetic frames, not qualification
of GF2, BF1 or Helldivers video resources.

The final standalone suite reports 219 passed, zero failed and zero skipped.
`validate_native_planar.py` checks all 216 configurations, original bytecode,
operands and declarations, raw values, validity, device rejection evidence and
saved frame hashes. It rejects 632 deliberate evidence corruptions.

Twelve existing suites pass 4,831 checks with no failures or skips in 1,245.17
seconds: pixel sampling, comparison sampling, sampling groups, gradient and LOD
sampling, gather, texture UAV, resource information, texture load/input, input
snapshot and execution trace. Together with the new suite, this is 5,050 passed
checks. The rebuilt GUI, CLI, Worker and thirteen test targets are recorded in
`dx11-native-planar-srv-evidence.json`. Ordinary and minimum-precision gather
evidence independently revalidates 2,808 and 2,880 comparisons, respectively,
and rejects 99 and 198 corruptions. These focused runs are not P8 performance
or standalone packaging acceptance.

## Retained development failures

The first matrix had 120 creation failures: 108 used a three-layer planar array
without `D3D11_BIND_DECODER`; 12 used a single-layer P016 resource on NVIDIA.
The D3D11 layer specifically reports that arrays larger than two require the
decoder bind flag. The corrected authored fixture uses two layers, still tests
relative layer selection, and restricts a later view to the second layer.
No existing golden, assertion or skip list was relaxed.

The initial GPU dimension queries failed on UNORM because their resource
declaration used UINT. That product defect was fixed before the successful
matrix. The first GPA round-trip fixtures also retained an empty template
shader's stale RDEF, so replay correctly omitted the supposedly unused SRV.
The authored DXBC now strips that stale reflection chunk and exercises the
existing stripped-shader replay path. The original logs remain in the evidence
ledger alongside the successful results.

## Reproduction and limits

Build `FloraDxbcPlanarTests` in Release. Set `FLORA_PLANAR_EVIDENCE` to a fresh
directory, run the test executable with `-o planar.txt,txt`, then run:

```powershell
python tools/validate_native_planar.py <evidence-directory> --test-log planar.txt --out validation.json
```

Run GPU tests serially. Do not read Qt logs until their process has exited.
The matrix requires the named adapters and the D3D11 debug layer. Evidence
records capability rejection when independently proven; unexplained creation
failures remain test failures.

Legacy GPA P010/P016 GenData does not reliably retain complete standard chroma
storage. Its existing replay guard remains active; these direct-device P010/P016
fixtures do not establish recovery of missing bytes. Planar UAV access, comparison
sampling, feedback/status instructions, all filtering modes, minimum precision
on planar operands and original-capture pixel qualification are not established
by this matrix. The unresolved P1 debugger object, full P5/P6 parity, RenderDoc
removal and standalone release acceptance remain open.
