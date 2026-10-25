# Independent pixel derivative observations

This checkpoint establishes synthetic four-pixel derivative inputs for the native
DXBC executor. It does not establish captured primitive quad alignment, sample
frequency execution, helper UAV side effects, implicit sampling, or complete pixel
debugging. The independent probe uses public D3D11 and the system shader compiler;
it does not load GPA or RenderDoc.

## Reproduction

Build the `FloraPixelDerivativeProbe` Release target. Create an empty parent folder,
then run the probe serially with a new child directory and vendor `0` (explicit
WARP), `4318` (NVIDIA), and `32902` (Intel). A missing requested adapter is fatal.
Run `tools/summarize_pixel_derivatives.py --root <parent> --binary
build/vs2022/Release/FloraPixelDerivativeProbe.exe --out <report.json>` from the
repository root. The committed ledger records the binary, source, raw reports,
and shader hashes. Raw reports and shader exports remain outside Git.

## Matrix and observations

Each adapter executes 336 cases: legacy X/Y, fine X/Y, coarse X/Y derivatives;
uniform, reconverged branch, unequal iteration count, and discard flows; finite
and special raw floating point inputs; full, top-row, left-column, and top-left
scissor coverage. Discard masks select one, two, or all four lanes. Post-discard
arithmetic changes the derivative inputs. Legacy shaders retain shader model 4
derivative opcodes; explicit fine/coarse shaders use model 5.

All 1,008 cases completed with zero D3D11 validation errors. The validator checks
2,520 untouched vectors against the original integer sentinel, 1,008 finite fine
components against independent arithmetic, and 2,016 finite coarse components
against the possible adjacent pairs. It retains 756 active special-value vectors
as raw observations without imposing a NaN payload or denormal arithmetic oracle.
The shaders are identical across adapters. All six divergent gradient shader
variants per adapter are rejected with compiler diagnostic X4014; these 18
rejections are not successful execution cases.

For these shaders and this synthetic 2x2 rasterization, all observed coarse and
legacy derivatives use the upper horizontal pair or left vertical pair. This is
an observation, not an architectural guarantee. The implementation must retain
the selected adapter and use hardware evaluation rather than hardcode that pair.
Scissored helpers and lanes after discard supply values to surviving pixels;
uncovered and discarded pixels do not update the target. No UAV operations were
included, so these results make no claim about UAV writes or atomic return values.

## Specification and remaining evidence

Microsoft documents [fine derivatives](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/deriv-rtx-fine--sm5---asm-)
using the adjacent pair containing the current pixel and
[coarse derivatives](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/deriv-rtx-coarse--sm5---asm-)
with hardware-dependent calculation. Quad alignment is not specified. The
[discard instruction](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/discard--sm4---asm-)
permits continued execution to support derivatives. These rules motivate the
probe; the observations are not substitutes for captured invocation evidence.

Remaining work includes proven captured quad inputs, mixed primitive coverage,
sample frequency execution, helper memory effects, divergent derivative validity,
CPU quad scheduling, implicit sampling, trace integration, and source navigation.
This checkpoint does not close P6 or the release acceptance gate.
