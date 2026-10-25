# Independent implicit pixel sampling observations

This probe establishes native `sample`, `sample_b` and `sample_c` behavior for
synthetic pixel quads before executor integration. It uses public D3D11, the
system HLSL compiler and explicit WARP/NVIDIA/Intel selection. It neither loads
GPA/RenderDoc nor infers captured quad inputs.

## Matrix

The R32_FLOAT Texture2D has four mips, starting at 8x8. A texel at `(x,y,mip)`
contains `mip + 0.25 + x/8 + y/64`. The probe renders a 2x2 integer target and
preserves the raw float result bits. It initializes untouched pixels to
`0xdeadbeef`; results use a replicated red channel to share ordinary and
comparison sampling observations.

There are 14 ordinary sampler configurations: point, linear, anisotropic, sampler
bias, minimum/maximum LOD, resource minimum LOD, a restricted mip view, four address
modes, null resource and null sampler. Comparison sampling adds seven configurations
to cover all eight comparison functions. Each instruction uses zero and `(-1,+1)`
offsets, eight coordinate/control-flow scenarios and four coverage masks. The
scenarios include zero/small/large/non-affine gradients, two discard masks followed
by coordinate arithmetic, reconverged branches and unequal loops. Bias is supplied
per pixel; comparison references include NaN and infinity in the branch scenario.

The matrix has 3,136 cases per adapter, 9,408 total. It records 17,856 defined active
vectors, 372 undefined missing-comparison-sampler observations, and 19,404 untouched
vectors. The validator checks 540 point-filtered vectors with independent texel
arithmetic, including all comparison functions, and 1,116 null-resource vectors.
All undefined observations require the specific D3D11 sampler mismatch diagnostic
390 and are excluded from numerical comparisons. No unexpected validation errors
occurred. All 24 compiled shader binaries match across adapters.

For every defined surviving pixel, partial scissor coverage produces exactly the
same bits as full-quad coverage with the same shader, inputs and discard mask.
There are zero coverage differences in this matrix. This supports a full-quad
GPU operation for these synthetic inputs. It does not prove that all formats,
shapes, primitive boundaries, sample-frequency invocations or drivers behave the
same way. Captured invocation provenance and broader resource coverage remain
required work.

## Specification and implementation consequences

Microsoft permits [sample LOD calculation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample--sm4---asm-)
to be shared within a quad or performed per pixel. The executor should run the
actual implicit instruction on the selected adapter; reconstructing LOD from a
single CPU-selected derivative pair is not equivalent evidence.
[Sample bias](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-b--sm4---asm-)
is applied per pixel together with the sampler bias before LOD clamps; out-of-range
bias has undefined results. [Comparison sampling](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-c--sm4---asm-)
compares the reference against red texels before filtering and replicates the
scalar result. It must not be replaced by a CPU comparison of the filtered texel.

## Reproduction

Build `FloraPixelSamplingProbe` in Release. Run it serially with a new output
directory and vendor `0`, `4318`, or `32902`. Output directory reuse and unavailable
requested adapters are fatal. Readback has a five-second deadline. The executable
writes `results.json`, DXBC binaries and disassembly; keep these outside Git.

Run `tools/summarize_pixel_sampling.py --root <parent> --binary
build/vs2022/Release/FloraPixelSamplingProbe.exe --out <report.json>`. The committed
`dx11-pixel-sampling-observations.json` records source/binary/report identities,
shader hashes and comparison counts. Five negative mutations (missing case,
altered sampler, incorrect point output, inactive write and undefined result
acceptance) are rejected. This research checkpoint does not complete P6 or release
acceptance.
