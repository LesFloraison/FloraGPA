# Minimum-precision native texture UAV loads

## Implementation scope

Implementation: `f4fdf3f5ffb912f06744611865f243e1b0c3e860`.

The independent DXBC executor accepts minimum-precision `ld_uav_typed` texture
loads in PS and CS execution. Five texture dimensions are covered: 1D, 2D, 3D,
1DArray and 2DArray. Destination precision supports float16, float2.8, signed16
and unsigned16 annotations; integer coordinates retain full, signed16 or
unsigned16 precision. PS output destinations keep their annotation and the
matching float/signed/unsigned render-target transport. CS continues to use
temporary destinations. The helper identity advances to `dxbc-gpu/31`.

The request carries the destination/coordinate precision, literal-coordinate
form, output-register selection and refactoring flag through the executor,
texture snapshot and GPU helper. Generated shaders retain the original precision
annotations. CS results are materialized through an annotated source before
full-width storage, matching the existing typed-buffer approach.

Resource validity follows the effective coordinates, including hardware precision
conversion. The original coordinates still feed the actual annotated load.
The existing 3D footprint probe handles locations outside the directly resolved
view range. Unknown source components remain unknown; output values do not serve
as a substitute for resource validity. The helper uses private resource copies,
the explicitly selected adapter and existing bounded completion/cancellation.

[Microsoft's minimum-precision guidance](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/using-hlsl-minimum-precision)
allows implementations to provide more precision than requested. Accordingly,
the probe compares each native result with execution of the original instruction
on the same selected adapter; it does not impose one CPU quantization model across
different GPUs.

## Owned reference and coverage

The existing independent texture reference now has an explicit texture-UAV mode.
It creates the original resource and view, binds input UAV slot 1, and executes
the original PS or CS. This is separate from the product's generated helper.
The old SRV mode retains its bindings and shader inputs.

The extended matrix has 300 configurations: the existing 180 SRV configurations
and 120 texture-UAV configurations on WARP, NVIDIA and Intel. Each covers three
coordinate precisions, four coordinate/mip cases, constant-buffer and immediate
operands, full and partial destination masks, and temporary or applicable PS
output destinations. Source formats are RGBA32_FLOAT, RGBA32_SINT and RGBA32_UINT;
this matrix does not claim every typed storage format or refactoring setting.

The cases include a smaller selected UAV mip, positive coordinates above 65535,
negative coordinates, array layers and 3D depth slices. Each observation compares
the original GPU result with interpreter raw bits and validity, then repeats with
all source bytes unknown. Each UAV configuration also checks cancellation and
recovery once per coordinate precision, preserving the pre-load register and
resource bytes.

Six pre-change PS/CS cases on the three adapters reached original shader execution
and then failed in the interpreter with its opcode-163 minimum-precision refusal.
The retained engine baseline is `69b016fd2781745f9e5b412748b9462d11d1de8f`.
The baseline fixture, reference implementation and executable remain available.

An intermediate expanded run had 1,730 passes and 24 failures, all from the new
3D fixture's resource creation. Its constructor wrote the UAV bind flag to the
Usage word instead of the 3D descriptor's BindFlags word. The fixture correction
changes that descriptor index; no product assertion or skip policy is relaxed.
The six focused 3D PS/CS float16 configurations then passed. That failed run and
its 276 completed configuration reports are retained separately from final data.

## Final validation

| Release suite | Passed | Failed | Skipped | Elapsed ms |
| --- | ---: | ---: | ---: | ---: |
| DxbcTextureLoad | 1754 | 0 | 0 | 383460 |
| DxbcTextureUav | 765 | 0 | 0 | 108727 |
| DxbcPixelMemory | 650 | 0 | 0 | 10798 |
| DxbcExecution | 1232 | 0 | 0 | 109824 |
| Total | 4401 | 0 | 0 | 612809 |

All four suites ran serially. GUI, CLI, Worker and test targets build in Release.
The independent verifier accepts all 300 configurations and 11,520 original-GPU
comparisons, including 4,320 new UAV observations. It checks 46,080 raw result
components, all corresponding unknown-storage observations and 360 UAV
cancellation/recovery controls. It rejects 102 corrupted reports, including
altered subresource and cancellation coverage. The original historical verifier
mode still accepts 180 configurations and rejects its 54 corruptions.

Every field in all 7,200 historical SRV records matches the final run: original
DXBC, coordinates, mask, output selection, reference/interpreter raw bits and
known/unknown validity. The old records are not rewritten. This comparison does
not imply complete hardware, shader or debugger coverage beyond the matrix.

The [evidence ledger](dx11-texture-uav-precision-evidence.json) pins Git source
content, executable snapshots, terminal logs and raw reports through an artifact
index. It distinguishes the six baseline product refusals, the later fixture
construction failures and final successful results. Suite times measure this
run; they do not establish sustained performance or bounded memory acceptance
for the final portable product. No standalone package or clean-host module audit
is claimed by these instruction tests.

## Reproduction

Build `FloraDxbcTextureLoadTests`, `FloraDxbcTextureUavTests`,
`FloraDxbcPixelMemoryTests`, `FloraDxbcExecutionTests`, `FloraGPA`,
`FloraGPA.Cli` and `FloraGPA.Worker` in Release. Put Qt on `PATH`, use a fresh
evidence directory, and execute GPU suites serially. Wait for each process to
terminate before reading its Qt log.

```powershell
$env:FLORA_TEXTURE_PRECISION_EVIDENCE = '<new-root>/cases'
build/vs2022/Release/FloraDxbcTextureLoadTests.exe -maxwarnings 0 -o <new-root>/texture-load.txt,txt
build/vs2022/Release/FloraDxbcTextureUavTests.exe -maxwarnings 0 -o <new-root>/texture-uav.txt,txt
build/vs2022/Release/FloraDxbcPixelMemoryTests.exe -maxwarnings 0 -o <new-root>/pixel-memory.txt,txt
build/vs2022/Release/FloraDxbcExecutionTests.exe -maxwarnings 0 -o <new-root>/execution.txt,txt
python tools/validate_texture_precision.py <new-root>/cases --texture-uav --backend dxbc-gpu/31 --test-log <new-root>/texture-load.txt --out <new-root>/validation.json
```

`--compare-srv <historical-SRV-directory>` additionally requires every historical
SRV record field to match, including original DXBC, selection, GPU/interpreter
bits and validity. Omitting the new options retains the earlier matrix and
backend requirements for historical evidence. Raw reports, binaries and failed
attempts remain outside Git in `artifacts/dx11-native/texture-uav-precision/`.

## Remaining migration gates

This increment expands native instruction execution; it does not recover the
original GPA debugger engine or qualify original PS fragment selection. It adds
no new VS UAV execution, texture-store precision or minimum-precision feedback
instruction support. Full P6 parity still requires the remaining instruction,
input, format and source/debugger matrix. Full Pixel History, default backend
cutover, RenderDoc removal and independent package acceptance remain unfinished.
