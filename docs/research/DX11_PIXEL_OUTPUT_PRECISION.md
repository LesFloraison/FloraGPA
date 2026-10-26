# Native pixel output precision

The native DXBC executor can now evaluate a single minimum-precision PS output
destination without replacing that destination with an ordinary temporary.
This addresses the output restriction reached after the typed-buffer precision
work in `DX11_TYPED_LOAD_PRECISION.md`. It is a P6 implementation checkpoint,
not completion of the public pixel debugger or RenderDoc removal.

## Execution contract

`dxbc-gpu/20` preserves the output declaration, operand precision and signature
type in the private PS helper. Float16 and float2.8 outputs use a private
R32G32B32A32_FLOAT target; signed16 and unsigned16 outputs use the corresponding
R32 integer targets. The result is read as four raw 32-bit words. An arbitrary
application output slot is mapped to private slot zero, and only the original
instruction's written components are committed to the native register state.
Previous components and unknown validity remain intact.

The helper uses the existing isolated device, bounded completion handling and
cancellation checks. It does not require an unused application UAV slot. It also
retains the shader's actual refactoring permission for assisted instructions;
the instruction's control bits and operand precision remain separate inputs.
This permission is not enabled unconditionally.

The [minimum-precision documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/using-hlsl-minimum-precision)
permits hardware to use higher precision than the declared minimum. The
[D3D11.3 specification, section 7.20.3](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
distinguishes output storage, operation, source and constant precision. These are
why the helper retains the output path and observes the selected GPU instead
of imposing CPU half-float rounding.

These observations describe the isolated instruction's output through a matching
32-bit target. They are not a dump of the original application's physical GPU
registers, nor do they include its render-target conversion, blending or depth
and stencil qualification. Minimum-precision multi-destination instructions and
sampling directly into a minimum-precision output remain guarded. Their parity
requirements are not removed by this checkpoint.

## Independent fixtures

`FloraDxbcPixelOutputTests` constructs explicit owned DXBC fixtures with FXC output
signatures. The reference creates the actual PS, binds the requested typed target
at output slot 0, 3 or 7, and executes a public D3D11 draw/readback. A separate
native lane executes the same fixture through the product interpreter and GPU
assist. The reference does not call that interpreter, GPA or RenderDoc.

The 2,592 regular fixtures combine three adapters, four output precisions,
floating or integer arithmetic/data-move operations, three slots, three write
masks, two source precisions and both refactoring permissions. Another 432
fixtures exercise signed zero, small normal values, NaNs, infinities, denormals
and integer boundaries through `mov`. Out-of-range minimum-precision constants
are retained as same-adapter observations, not portable overflow guarantees.

Each result saves its shader, constants, expected/observed bits, validity and
both adapter identities. The independent Python validator checks exact fixture
declarations, instructions, operand precision, slot/mask, constant values and
refactoring flags in addition to output equality. It rejects an incomplete
matrix or altered metadata. Tests also check the reference context's output
binding, reversible register deltas, cancellation/retry and unknown components.

## Validation checkpoint

The Release build and all selected checks completed on NVIDIA, Intel and WARP.
The retained evidence ledger is `dx11-pixel-output-precision-evidence.json`.

| Check | Passed | Scope |
| --- | ---: | --- |
| Pixel output suite | 3,027 | 3,024 GPU cases, validity/cancellation and lifecycle |
| Execution suite | 445 | Existing VS/PS/CS execution checks |
| Compute group suite | 182 | Existing group checks and 144 minimum-precision/refactoring cases |
| Typed pixel precision selection | 1,155 | Minimum-precision and validity/cancellation slots, not the entire typed suite |
| Pixel sampling precision selection | 20 | Minimum-precision slot and lifecycle, not the entire sampling suite |

There were no failures or skips in these runs. Independent validators checked
12,096 PS output components, 23,040 typed-load components and 80,352 sampling
components. They rejected 108, 72 and 126 evidence corruptions respectively.
The compute additions compare an original public D3D11 dispatch with native
group execution for four precision types, six operations and both refactoring
permissions on each adapter, including exact storage and validity checks.

All nine GF2 event 113, BF1 event 2010 and Helldivers event 200 capture/adapter
probes now complete their selected private quad journals. They contain 1,368
selected steps and 5,472 global steps, with C++ reader/model checks, full rewind
and 180 rejected corruptions. Each Helldivers run has 329 selected steps, 1,316
global steps, eight sampling batches, four derivative batches and final active
mask 13. This supersedes the earlier output restriction without altering the
historical failure evidence.

These real-frame checks establish input provenance, serialization and reversible
projection. They do not establish original fragment eligibility or independently
compare the complete original PS output. The Helldivers shader has no available
source symbols in this checkpoint; zero mapped source steps must not be reported
as source-debugging parity.

The Helldivers journal is approximately 248.2 MiB, close to the 256 MiB budget.
On NVIDIA, initial reader construction/validation/rewind takes 4,229 ms; the full
probe takes 272,631 ms because it additionally hashes roughly 64 MB of initial
resource data and its validity at every selected boundary and checks the debug
model. These are distinct measurements, neither a P8 performance acceptance nor
evidence of interactive stepping latency. Resource snapshot size and journal
loading remain performance work; no oracle assertions were relaxed.

## Reproduction

Build `FloraDxbcPixelOutputTests` and `FloraDxbcExecutionTests` in Release. Run
GPU tests serially with Qt on `PATH`:

```powershell
$env:FLORA_KEEP_PIXEL_OUTPUT_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcPixelOutputTests.exe -o pixel-output.txt,txt
build/vs2022/Release/FloraDxbcExecutionTests.exe -o execution.txt,txt
python tools/validate_pixel_output_precision.py <saved-evidence-directory> --out <new-report.json>
```

The output log identifies the retained evidence directory. Existing typed-load
and sampling precision validators accept `--backend dxbc-gpu/20` for new runs;
their historical defaults remain unchanged. Real-frame validation uses the
existing pixel geometry probe with `centroid trace`, the C++ trace reader with
the original saved shader, and `validate_pixel_execution_trace.py`.

Raw shaders, trace files, capture data and large validation manifests remain
outside Git. Full fragment eligibility/history, remaining instruction semantics,
source/UI parity, public PS sessions, dependency deletion and standalone release
acceptance remain open.
