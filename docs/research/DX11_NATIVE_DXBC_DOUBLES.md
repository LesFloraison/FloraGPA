# Native DXBC double instructions

Implementation commit: `e65c7de6535c220067f55c555a1bd4d090b31a7a`.

## Implemented behavior

The owned executor previously parsed double operands but rejected their executable
opcodes. It now executes the 19 SM5 double instructions: `dadd`, `dmax`, `dmin`,
`dmul`, `deq`, `dge`, `dlt`, `dne`, `dmov`, `dmovc`, `dtof`, `ftod`, `ddiv`,
`dfma`, `drcp`, `dtoi`, `dtou`, `itod` and `utod`.

The C++ executor owns instruction sequencing, source selection, register addressing,
paired validity and reversible register changes. Private D3D11 shaders perform the
instruction on the selected adapter, preserving driver arithmetic and conversion
behavior. The assistance identity is `dxbc-gpu/21`. Native analysis jobs also bind
the Worker executable SHA-256, so previously cached jobs cannot silently reuse a
different implementation.

Double values occupy low/high word pairs `xy` and `zw`. All four legal pair
swizzles, sign modifiers, immediate 64-bit operands, constant buffers, temporary
registers, indexable temporaries and VS/PS output registers are handled. Scalar
comparison/conversion results are packed into the selected destination components.
Unknown source words invalidate the complete affected pair. Conditional moves
depend on the selected branch and condition, without taint from an unselected
branch. The raw result words remain available even when their validity is unknown.

SM5 capability declarations and pair masks are checked before execution. The
actual helper device must report basic double support; extended instructions also
require extended double support. Unsupported hardware fails explicitly without
adapter substitution. Malformed indexable component destinations are rejected
before issuing GPU work. Minimum-precision double operands remain explicitly
unsupported; this change does not claim comprehensive malformed-bytecode validation.

The implementation was developed from this project's operand model and public
instruction documentation. No RenderDoc interpreter or Intel private runtime is
used by this execution path.

## Independent observations

The raw matrix contains 684 cases: 19 instructions, three requested adapters,
three stages and four operand/validity forms. Each supported case compares the
native instruction helper with the original operand encoding executed directly
in an independent whole VS using stream output. The oracle does not call the
instruction rewriting or assistance implementation. PS and CS helpers are
therefore compared with a VS instruction oracle for this finite operand matrix;
this is not an independent whole PS/CS shader equivalence claim.

The compiled matrix contains 108 cases: 12 HLSL scenarios, three requested
adapters and three datasets. Scenarios cover arithmetic, min/max, comparisons,
float/integer conversions, division, conditional selection and loops. Datasets
include ordinary finite values, a double subnormal with signed zero, and NaN with
infinity. The original compiled VS runs independently on D3D11. All supported
results match raw words exactly on these local drivers, and every register journal
reverses to its initial state. Observed NaN and conversion bits are local results,
not universal cross-driver rules.

| Requested adapter | Basic/extended support | GPU comparisons | Explicit hardware rejections |
|---|---|---:|---:|
| WARP, vendor 5140, device 140 | Yes / yes | 264 | 0 |
| NVIDIA, vendor 4318, device 11352 | Yes / yes | 264 | 0 |
| Intel, vendor 32902, device 32103 | No / no | 0 | 264 |

Adapter LUIDs and driver identities are retained in the Qt logs and result files.
The validator compares helper identity with the independently created reference
device's logged identity. Intel rejection checks are counted separately from
functional comparisons and are not skipped test cases.

Additional pair-state tests execute 280 supported helper operations across WARP
and NVIDIA. They verify every pair swizzle and source modifier, raw NaN/subnormal
movement, output and indexable destinations, condition validity, and unselected
unknown branches. Intel verifies 140 corresponding capability rejections.
Malformed masks, missing capability declarations, invalid pair swizzles, absent
GPU assistance and indexable component limits have explicit rejection checks.

`validate_dxbc_doubles.py` verifies all 792 retained reports and bytecode identities,
the exact raw instruction programs, validity masks, complete selection matrix and
independent oracle results. It rejects 5,772 deliberately corrupted reports.
The 528 GPU comparisons and 264 hardware rejections are separate counters.

## Regression and retained evidence

Seven CTest entries pass serially with 2,540 Qt checks, zero failures and zero skips:

| Test | Passing checks |
|---|---:|
| `dxbc_execution` | 1175 |
| `dxbc_pixel_quad` | 1022 |
| `dxbc_group` | 182 |
| `dxbc_execution_trace_reader` | 64 |
| `dxbc_execution_trace` | 21 |
| `native_compute_debug` | 63 |
| `native_execution_debug_ui` | 13 |

The first five CTest entries took 139.85 seconds in total, including 109.87 seconds
for `dxbc_execution` within its unchanged 180-second limit. The two workflow entries
took 24.30 seconds. Release GUI, CLI and Worker targets build successfully.
These measurements are test execution times, not a replay performance comparison.

A single in-flight execution-test snapshot records 72 loaded modules, no RenderDoc
module and no module under the original Intel GPA installation. Its observed peak
working set is 85,569,536 bytes. This snapshot does not cover transient modules,
child processes or final process peak, and is not the P8 release audit.

The initial smoke test failed because its fixture attempted to supply a temporary
register as an entry input. Entry inputs intentionally reject that register kind.
The corrected fixture initializes the temporary with an ordinary `mov` instruction.
The seven original failures, corrected smoke run, preliminary matrices, final
builds, regression logs and validation results are retained without rewriting.
No frozen-main golden or existing skip condition changed.

Raw evidence remains under ignored `artifacts/dx11-native/dxbc-double-*` paths.
The adjacent `dx11-native-dxbc-doubles-evidence.json` pins source, tools, binaries,
logs, reports and shader bytes. Historical validators for older helper versions
continue to validate their original evidence; this new validator requires version 21.

## Reproduction

Use a fresh evidence directory because the test writer refuses to overwrite files.
Build the test and application targets with the existing Windows Release toolchain:

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraDxbcExecutionTests FloraDxbcPixelQuadTests FloraDxbcGroupTests FloraDxbcExecutionTraceReaderTests FloraDxbcExecutionTraceTests FloraNativeComputeDebugTests FloraNativeExecutionDebugUiTests FloraGPA FloraGPA.Cli FloraGPA.Worker
$env:FLORA_DXBC_DOUBLE_EVIDENCE = "$PWD/artifacts/dx11-native/dxbc-double-rerun"
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure --timeout 180 -R '^(dxbc_(execution|pixel_quad|group|execution_trace_reader|execution_trace)|native_(compute_debug|execution_debug_ui))$'
python tools/validate_dxbc_doubles.py artifacts/dx11-native/dxbc-double-rerun --test-log build/vs2022/dxbc-execution-Release.txt --out artifacts/dx11-native/dxbc-double-rerun-validation.json
```

The retained validator pins the observed local adapter capability matrix. A machine
with different support requires a separately recorded capability baseline; it must
not relabel unsupported observations as executed comparisons.

## Remaining gates

This is a P6 instruction-set extension. It does not recover the original GPA
debugger engine at host `+0x3c0`, complete source/watch/type parity for double
variables, qualify captured pixel invocations, enable the reserved public
`debug-pixel` action or close full Pixel History. Additional shader and hardware
coverage remains necessary. Existing RenderDoc build/runtime consumers remain;
P7 removal and P8 standalone package acceptance are still open.

## Public instruction references

- [DADD paired operands and destination masks](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dadd---sm5---asm-)
- [DMOVC branch selection and paired movement](https://learn.microsoft.com/en-us/windows/desktop/direct3dhlsl/dmovc--sm5---asm-)
- [DTOF conversion and destination packing](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dtof--sm5---asm-)
- [FTOD conversion and permitted denormal behavior](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ftod--sm5---asm-)
- [DEQ comparison result layout](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/deq--sm5---asm-)
