# Native PS typed buffer execution

Date: 2026-10-24. This P6 checkpoint executes typed-buffer SRV loads in the native
pixel stage. It does not supply captured pixel inputs, quad/helper-lane execution,
derivatives, texture sampling or full fragment debugging.

## Native operation and coverage

The independent executor routes a PS `ld` to a project-generated native PS on its
isolated same-adapter D3D11 device. The helper recreates the typed SRV's original
format and element range over private immutable backing bytes. A generated vertex
shader covers a one-pixel viewport, and the PS writes raw words to a private
R32G32B32A32_UINT render target. No application UAV slot is needed. VS and CS
operations retain their own native stages; PS access does not fall back to them.

An occlusion query verifies exactly one covered sample before a result is accepted.
Readback and query completion share a bounded polling deadline and cancellation
checks. The context is cleared on completion or failure; failed operations poison
the helper until it is recreated. Pixel shader cache entries are bounded to 256,
separately from the existing VS and CS caches. Backend identity is `dxbc-gpu/5`.

The CPU still controls instruction execution, register bits and component validity.
The helper supplies hardware format conversion and load semantics. Unknown byte
validity stays unknown after readback, destination masks retain other components,
and reversible changes support backward stepping. Unknown addresses fail before
submitting a native operation. A verified null binding yields known zero, while
an unavailable binding remains unknown.

This follows the [Microsoft `ld` contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld--sm4---asm-)
for pixel-stage typed reads, defaults, unsigned addresses, swizzles and raw 32-bit
floating-point preservation. It does not infer PS equivalence from earlier VS or
CS measurements. The product path calls native D3D11 and contains no GPA private
entry points or RenderDoc calls; final package/module auditing remains open.

## Independent probe

`FloraDxbcTypedPixelTests` executes an independently compiled `ps_5_0` shader on a
reference device, then executes the independent CPU core with native PS assistance.
It compares all four output words exactly, including subnormal values, negative
zero and a NaN payload. It covers:

- Explicit WARP, NVIDIA and Intel selection at feature level 11.0.
- R32_FLOAT/UINT/SINT, R32G32B32A32_FLOAT, R16G16B16A16_UNORM,
  R8G8B8A8_SNORM/UINT, R8G8_UINT, R8_UINT, R10G10B10A2_UNORM,
  R11G11B10_FLOAT and R32G32B32_FLOAT.
- A three-element view beginning at element one; indices 0, 2, 3 and UINT32_MAX;
  literal versus constant-buffer indices; full swizzles and partial assignments.
- Per-component unknown propagation, cancellation/retry, unavailable addresses,
  null bindings and exact register trace reversal.
- Original source bytes and render-target bytes, plus reference VS/PS, CB/SRV,
  RTV, rasterizer, blend factor/sample mask, depth/stencil reference, topology,
  viewport and scissor bindings. D3D11 error messages fail the probe.

The earlier VS lifecycle test's unsupported-PS assertion is now strengthened to
require successful PS execution, exact subnormal/NaN output words and the correct
unknown-component mask. Its original VS assertions remain intact. This is support
expansion, not a new historical exception or a reduced assertion.

Each PS hardware record includes original shader/input hashes, raw CPU/GPU words,
validity and helper adapter/driver/feature-level identity. The evidence script
retains raw-log SHA-256 values and compact summaries for unchanged regression
suites, avoiding copies of all their successful probe rows. Raw logs, shaders and
other generated artifacts stay outside Git.

The initial WARP smoke and lifecycle run passed five cases. The complete serial
CTest run then passed all eight suites in 431.61 seconds:

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Typed PS buffer | 580 | 0 | 0 |
| Typed VS buffer | 580 | 0 | 0 |
| Typed CS buffer | 473 | 0 | 0 |
| Input snapshot | 42 | 0 | 0 |
| Hidden counter | 40 | 0 | 0 |
| Thread group | 38 | 0 | 0 |
| Raw/structured memory | 246 | 0 | 0 |
| Instruction execution | 375 | 0 | 0 |
| Total | 2374 | 0 | 0 |

The PS suite's 580 cases comprise 576 hardware rows, two validity/lifecycle cases
and QtTest setup/cleanup. Final builds completed without compiler warnings.
This focused checkpoint does not replace the full P0 capability matrix or final
release, performance and module audits.

## Limits and reproduction

The established stage/model is PS SM5 at feature level 11.0 with typed buffer
SRV loads. Pixel UAV execution, textures, sampling, actual captured invocation
selection, quad-dependent behavior, full source/GUI integration and release parity
remain required. A one-operation helper render is not a captured fragment record,
Pixel History implementation or proof of full pixel-shader execution.

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcTypedPixelTests --parallel 4
ctest --test-dir build/vs2022 -C Release -R '^dxbc_typed_pixel$' --output-on-failure -j 1
```

Run GPU suites serially. `tools/summarize_dxbc_typed_pixels.py` accepts repeated
`--log`, `--regression` and `--source` arguments, followed by `--out`. The checked-in
ledger is `research/dx11-dxbc-typed-pixel-evidence.json`.
