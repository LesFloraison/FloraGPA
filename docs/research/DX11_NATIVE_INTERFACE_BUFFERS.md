# Native class buffer loads and resource queries

This P6 increment adds `bufinfo`, relative raw/structured buffer loads and
class-selected texture `resinfo` to the independent executor. It does not close
class-debug parity, captured Worker qualification, or the RenderDoc removal gate.
No Intel GPA or RenderDoc implementation is linked into these operations.

## Instruction evidence and implementation

The local Microsoft compiler emits `bufinfo_indexable` for class buffer
`GetDimensions`, followed by `ld_raw_indexable`, `ld_structured_indexable` or
typed `ld_indexable` for the corresponding load. Class texture dimensions use
`resinfo_indexable`. The owned HLSL fixtures have two distinct implementations,
two named array elements and an actual interface call. Saved original DXBC and
direct-control DXBC are outside Git and pinned by the evidence ledger.

Microsoft's [bufinfo instruction contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/bufinfo---sm5---asm-)
defines the result as the view's typed element count, raw byte count or structured
element count, replicated to each destination component. The operation requires
shader model 5.0. The [published token format](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/Support/d3d12TokenizedProgramFormat.hpp)
places structured stride in bits 22:11 of the resource-dimension extension.
The observed compiler tokens include:

| Resource | Dimension extension | Return extension | Operation |
| --- | --- | --- | --- |
| Raw | `0x800002c2` | `0x00199983` | 121 / 165 |
| Structured, stride 16 | `0x80008302` | `0x00199983` | 121 / 167 |
| Typed uint buffer | `0x80000042` | `0x00111103` | 121 / 45 |
| Texture2D uint | `0x800000c2` | `0x00111103` | 61 |

Native resource shape resolution now includes stride. Relative SRVs must carry
complete dimension and return-type extensions; they do not borrow a declaration
at the physical slot. Static resources continue to require a matching declaration.
Duplicate and reserved type extensions, invalid structure strides and missing
relative type information fail explicitly. Actual buffer views must agree with
the instruction's structure stride or typed-buffer representation.

`bufinfo` reads the private view range rather than the entire backing buffer.
Missing binding information stays unknown; an explicitly unbound view returns
known zero. Unknown buffer contents do not make a known view size unknown.
Destination masks preserve untouched register components. The snapshot identity
advances from `independent-replay-resource-inputs/9` to `/10`.

## Independent GPU controls and unresolved differences

`FloraDxbcInterfaceBufferTests` covers 96 combinations: WARP/NVIDIA/Intel, two
class implementations, two array indices, named/created instances and four
resource/query modes. Raw views start at 16 or 32 bytes, other buffer views have
nonzero element offsets, and all view ranges are narrower than their resources.
Texture views start at mip 1 and query relative mip 1. Native loads and queries
use the observed class descriptor and the same resource contents and view range.

Every case runs in a bounded child process. Native results are saved first.
A separate directly bound shader then runs on the original device with the same
view; its bytecode must contain the query and must not contain an interface call.
Only then does the original class shader dispatch. This preserves native and
control evidence even when the original class path crashes. Each child's exit
status and completed Qt log are saved; crashes remain test failures.

All **96 native values** and **96 direct GPU controls** match the authored integer
results. Original class execution remains incomplete:

| Original device | Raw | Structured | Typed buffer | Texture dimensions |
| --- | --- | --- | --- | --- |
| WARP | 8 dispatch crashes | 8 dispatch crashes | 8 dispatch crashes | 8 dispatch crashes |
| NVIDIA | 8 matches | 8 matches | 8 matches | 1 match, 7 differences |
| Intel | 1 match, 7 differences | 1 match, 7 differences | 1 match, 7 differences | 1 match, 7 differences |

Across the class matrix, 29 results match, 35 differ and 32 crash with
`0xc0000005`. The parent Qt run reports **31 passed, 67 failed, 0 skipped**,
including setup and cleanup. The native arithmetic and direct GPU controls are
consistent, but they do not prove why the original class path differs. Compiler,
runtime and driver responsibility is unresolved. These are new research cases,
not automatically exempt P0 historical failures. No expected value or assertion
was lowered to accept them.

The debug-layer queue is checked on successful complete comparisons. Failed
value comparisons and crashed dispatches do not reach that final check, so this
report does not claim a clean post-dispatch debug queue for those cases.

## Validation and retained failed attempts

Two new `DxbcMemoryTests` methods cover 72 combinations of shader stage, SRV/UAV,
typed/raw/structured views and known/null/missing binding states, including masked
register writes and unknown content bytes. These are interpreter tests, not a
claim that every stage/UAV combination is supported by every D3D11 device.
Additional cases reject SM4 queries, invalid opcode controls and relative SRVs
without type extensions.

After the final implementation, serial regression results are:

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| `dxbc_resource_info` | 629 | 0 | 0 |
| `dxbc_group` | 182 | 0 | 0 |
| `dxbc_memory` | 248 | 0 | 0 |
| `dxbc_interface_resources` | 91 | 7 | 0 |

The seven resource-suite failures are exactly the previous Intel Gather cases.
The combined CTest run remains failed. GUI, CLI and Worker Release builds pass;
their build graph still contains the legacy Rdc target.

Initial raw-view fixtures incorrectly used a four-byte-aligned start; the D3D11
debug layer required 16-byte alignment. The fixture and its independently
calculated expected value were corrected before qualification. Unisolated WARP
crashes and the first isolated matrix are retained. Early negative-test fixtures
also used an invalid relative token and then an unsupported temporary-register
input; the final test initializes that register through an actual `mov`. Those
failed logs remain outside Git with the successful replacement run. They are not
counted as product passes.

## Reproduction

Build `FloraDxbcInterfaceBufferTests`, `FloraDxbcMemoryTests`,
`FloraDxbcResourceInfoTests`, `FloraDxbcGroupTests` and
`FloraDxbcInterfaceResourceTests` in Release. Place Qt on `PATH`, select a fresh
evidence directory and run GPU tests serially:

```powershell
$env:FLORA_INTERFACE_BUFFER_EVIDENCE = (Join-Path $PWD 'artifacts/dx11-native/class-buffer-rerun')
build/vs2022/Release/FloraDxbcInterfaceBufferTests.exe -o artifacts/dx11-native/class-buffer-rerun.txt,txt
python tools/validate_native_interface_buffers.py artifacts/dx11-native/class-buffer-rerun --log artifacts/dx11-native/class-buffer-rerun.txt --out artifacts/dx11-native/class-buffer-rerun-validation.json
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^(dxbc_memory|dxbc_resource_info|dxbc_group|dxbc_interface_resources)$'
```

The validator verifies all 96 case identities, adapter identities, class resource
bases, opcode presence, direct controls, child outcomes and parent totals. It
rejects 192 altered result records. It writes `evidence_complete: true` and
`parity_passed: false`, then exits nonzero while original differences or crashes
remain. The tracked `dx11-native-interface-buffers-evidence.json` records hashes
of private evidence, tested binaries, source and regression logs.

Captured class-resource workflows, full VS/PS/CS parity and the P1 debugger
engine at host `+0x3c0` remain open. This increment does not change the
Pixel History, dependency deletion or independent release acceptance gates.
