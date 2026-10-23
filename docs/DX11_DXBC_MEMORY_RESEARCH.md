# Native DXBC buffer execution checkpoint

Date: 2026-10-24. P6 remains unfinished. At this CPU-only checkpoint the buffer suite reported
119 passing, 6 failing and zero skipped cases. The pre-existing scalar/control
suite still reports 357 passing, 17 failing and zero skipped cases. Both strict
CTest entries remain enabled. No production debugger has switched to this core.

Follow-up: `DX11_DXBC_GPU_MEMORY_RESEARCH.md` records the same-adapter private GPU
assist that resolves these six buffer differences without relaxing assertions.
Its expanded suite passes 246 cases. The CPU-only checkpoint and evidence below
remain preserved; the separate scalar/control failures and full P6 gate are open.

## Private memory model

`DxbcMemory.cpp` adds independent raw and structured SRV/UAV execution for
`ld_raw`, `store_raw`, `ld_structured` and `store_structured`. Each private resource
snapshot owns its bytes and per-byte validity. Views reference a resource identity,
byte range and structure stride; overlapping views use the same underlying bytes.
No captured D3D11 resource, context, binding or hidden counter is modified.

A missing binding is unknown, while an explicit null binding is represented
separately. Loads from an unknown binding produce unknown components; a store
through an unknown binding fails before changing state. Unbound loads return zero
and unbound stores have no effect. In-range components are known only when all
four source bytes are known. A store of an unknown component clears the written
bytes' validity. Stale raw bits may remain present but cannot establish a value.

The executor validates resource declarations against the captured view kind and
stride. Sources and dynamic addresses are resolved before preparing a change.
Each successful store records resource-relative offset, before/after bytes and
before/after validity; mutations are committed only after the instruction and its
continuation validate. This supports reversible trace storage without touching
replay memory. The tests restore the original storage from recorded changes.

Shared memory objects are intended for a serial diagnostic scheduler, not concurrent
host threads. They do not themselves implement CS scheduling, barriers, races or
atomic operations. The default snapshot budget is 256 MiB of resource bytes and
the per-instruction changed-range budget is 16 MiB. Validity and trace copies are
additional allocations; these limits are not claims about total peak memory.
Unknown addressing, malformed views, unaligned accesses, stride mismatches and
budget overruns fail explicitly. Typed buffers, texture operations, resource
capture integration and GPU-assisted boundary semantics remain pending.

## Addressing references and independently tested behavior

The implementation was written using public instruction contracts:

- [ld_raw](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-raw--sm5---asm-):
  resource swizzle selects components from the addressed window; an out-of-range
  SRV/UAV component has a zero result.
- [store_raw](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-raw--sm5---asm-):
  contiguous x/xy/xyz/xyzw masks select the write width, with only in-range data
  written.
- [ld_structured](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-structured--sm5---asm-):
  an element index and field byte offset form the address; an invalid field access
  is distinguished from an out-of-range element.
- [store_structured](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-structured--sm5---asm-):
  an invalid field access makes UAV contents undefined. The private model clears
  validity over the view rather than manufacturing a definite result.

CPU tests cover overlapping view identity, resource swizzles, partial raw stores,
unknown bytes propagated through loads/stores, missing bindings, invalid field
reads, view invalidation, unknown addresses, invalid masks, declaration mismatch,
snapshot/change budgets and rollback when a store would fall off a program without
ret. The undefined-field tests assert validity, not arbitrary hardware byte values.

GPU probes compile ordinary HLSL CS 5.0 shaders and run one thread on explicitly
selected WARP, NVIDIA and Intel devices. They use raw and 24-byte structured
buffers with nonzero view offsets, compare the complete backing output buffer,
and include bound/null SRV and UAV combinations. Whole-buffer comparison also
detects writes outside the view. No adapter fallback or external analysis tool is
used. The debug layer reports no errors or warnings in the recorded run.

## Open boundary differences

The first run passed 75 cases and failed one Intel raw-buffer case. The expanded
run retains that case and separates extreme reads from extreme writes. All ordinary
in-range, partial-range and ordinary out-of-range cases pass. The remaining six
rows use raw byte offset 0xfffffffc or structured element index 0xffffffff:

| Device | Case | Observation |
| --- | --- | --- |
| NVIDIA | Raw extreme write, both views bound | Output storage differs from the unchanged private model |
| NVIDIA | Structured extreme write, bound source | Output storage differs from the unchanged private model |
| NVIDIA | Structured extreme write, null source | Output storage differs from the unchanged private model |
| Intel | Combined raw extreme read/write | Output storage differs from the private model |
| Intel | Raw extreme read, ordinary destination | Loaded values differ and affect ordinary output storage |
| Intel | Raw extreme write, ordinary source | Output storage differs from the unchanged private model |

WARP agrees with the model for all tested buffer cases. These observations do not
prove a universal wrapping rule or identify the responsible driver lowering. The
CPU model uses widened arithmetic to avoid accidental host-address wrap. Actual
device behavior at these boundaries needs a separate native GPU operation probe
and, where required, a private GPU assist tied to the analysis adapter. No vendor
lookup table, changed golden, enlarged skip or relaxed comparison was introduced.
This checkpoint does not claim hardware parity for extreme addresses.
The structured NVIDIA differences affect backing bytes 16-23, before the selected
view begins at byte 24. A future assist must preserve the whole resource and the
original view offset, not just copy the view's bytes into a new zero-based buffer.
These comparisons establish final storage values, not exact write footprints;
unchanged bytes alone never prove that a write did not execute.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcMemoryTests FloraDxbcExecutionTests
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(dxbc_execution|dxbc_memory)$' --output-on-failure
python tools/summarize_dxbc_execution.py --scope native_dxbc_buffer_execution `
  --log build/vs2022/dxbc-memory-Release.txt `
  --source src/application/DxbcMemory.cpp `
  --source tests/DxbcMemoryTests.cpp `
  --out artifacts/dx11-native/dxbc-memory-current.json
```

CTest and the summarizer currently return failure. The evidence ledger
`research/dx11-dxbc-memory-evidence.json` retains original and expanded totals,
exact CPU/GPU byte arrays for failed comparisons, adapter/driver identity and
source hashes. The original scalar/control ledger remains unchanged; the
post-memory regression run is retained under ignored artifacts as
`dxbc-execution-after-memory.txt`. Ghidra's unresolved GPA debugger engine and all
remaining P5-P8 gates remain open.
