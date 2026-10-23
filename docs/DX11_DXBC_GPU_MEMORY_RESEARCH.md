# Native GPU assistance for DXBC buffer operations

Subsequent checkpoint: `DX11_DXBC_GPU_INSTRUCTION_RESEARCH.md` records the VS
instruction extension and renames the shared helper to `GpuOperations`. The
scalar/control failure counts below describe the earlier buffer checkpoint.

Date: 2026-10-24. The hybrid buffer suite now passes 246 cases with zero failures
and zero skips. The six earlier buffer differences are reproduced and resolved
by actual GPU operations, not vendor-specific guessed values. The separate
scalar/control suite still has 357 passes and 17 failures. P6 and release acceptance
remain open.

## Implementation and provenance

`GpuMemory` creates a private D3D11 device from the reference analysis device's
adapter and exact feature level. It verifies the resulting adapter LUID; WARP is
an explicit construction option. There is no alternate-adapter or legacy-backend
fallback. Only the private context receives bindings, dispatches and copies. The
reference device is used to obtain identity and creation settings, never to obtain
or execute its immediate context.

Each assist copies the full backing buffer and recreates the original view offset,
size and stride. This matters for the observed NVIDIA structured-index boundary:
the original operation changed backing bytes before the view. Copying only the
view into a new zero-based resource would have produced a different result.

The helper emits a small project-owned DXBC compute program containing the single
raw/structured operation. It preserves the load swizzle, destination mask and
whether address operands were literals. Other resolved values are supplied through
a private constant buffer. Loads return raw bits through a separate private UAV;
stores return the complete backing buffer. The helper does not load or copy any
Intel GPA or RenderDoc implementation. Its shader framing uses the independently
documented public DXBC format already used by the CPU executor.

Default hybrid execution retains CPU operations for ordinary addresses and uses
the helper where address arithmetic can cross the 32-bit boundary. An explicit
all-accesses mode exercises the helper even for ordinary accesses. Undefined
structured field accesses continue to produce unknown validity; they are not
converted into a hardware-dependent definite value.

Steps expose `memoryGpuAssisted`. The helper exposes actual LUID, driver identity,
feature level, WARP selection and backend version. Store readbacks become reversible
observed byte differences, including differences outside the selected view. They
do not establish the complete write footprint: an unchanged byte may still have
been written. Null and unavailable bindings retain their distinct CPU contracts.

## Isolation and bounded failure

GPU completion uses nonblocking staging Map attempts, cancellation checks and a
bounded deadline (five seconds by default). Errors poison the helper so callers
must recreate it before retrying. Failed or cancelled operations never commit
private CPU memory changes. This is designed to run within the existing Worker
process boundary; its integration into production debug requests is still pending.

The helper caps each backing snapshot at 256 MiB and its shader cache at 256 entries.
Memory change byte budgets remain active, with an additional 4096-range limit for
GPU differences. Unknown store data uses one bounded conservative validity snapshot
instead of millions of tiny ranges. GPU loads with unknown snapshot bytes retain
unknown validity. These choices do not prove complete precision for partially
known memory; that remains a debugger-semantic validation item.

The deadline covers GPU completion polling, not an operating-system guarantee that
every driver API call is interruptible. Worker process timeout/recovery remains
necessary. Full-resource transfers at large sizes, peak memory and real-capture
latency remain performance gates; the small probes are not release benchmarks.

## Independent validation

The reference path compiles normal HLSL and executes the complete original CS
program. The implementation path executes CPU control/arithmetic with independently
assembled single-operation assists on a different device. Both use the same
explicitly selected adapter. Original shader sources, input bytes, views, address
cases and exact full-buffer assertions are retained from the earlier failed suite.
No expected failure, tolerance adjustment or skip was added.

The test runs each original case in both boundary-only and all-accesses modes.
Both modes pass all six previously failed case identities on their original
adapters, giving twelve recorded resolved comparisons. The final run records
48 boundary-only and 240 all-accesses GPU operations across hardware cases. Each
step's assistance flag is checked against the helper's successful operation count.
These counts exclude the separate cancellation/recovery unit test.

Every hardware case checks that a sentinel constant-buffer binding on the reference
context survives helper execution, and reverses recorded memory changes back to
the initial bytes and validity. A cancellation probe interrupts completion polling,
checks that CPU memory remains unchanged, verifies that the poisoned helper rejects
reuse, and succeeds after constructing a fresh helper. This validates a specific
binding and lifecycle path, not all replay-state invariants required by P8.

Final serial CTest results:

| Suite | Passed | Failed | Skipped | CTest duration |
| --- | ---: | ---: | ---: | ---: |
| dxbc_memory | 246 | 0 | 0 | 42.98 s |
| dxbc_execution | 357 | 17 | 0 | 45.14 s |

The memory suite passes; the combined CTest command still fails because the
pre-existing scalar/control comparisons remain unresolved. The GPU helper was
validated on feature level 11.0 CS paths. Other stages, lower feature levels,
typed resources, textures, thread groups and end-to-end debugger workflows still
require implementation or validation. This does not authorize a production cutover.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcMemoryTests FloraDxbcExecutionTests
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(dxbc_memory|dxbc_execution)$' --output-on-failure
python tools/summarize_dxbc_execution.py --scope native_dxbc_gpu_buffer_assistance `
  --log artifacts/dx11-native/dxbc-memory-expanded.txt `
  --log build/vs2022/dxbc-memory-Release.txt `
  --source src/application/DxbcGpuMemory.cpp `
  --source tests/DxbcMemoryTests.cpp `
  --out artifacts/dx11-native/dxbc-gpu-memory-current.json
```

Omit the first log when reproducing without the earlier local artifacts. The
checked-in `research/dx11-dxbc-gpu-memory-evidence.json` preserves old failures,
new totals, the twelve resolved case identities, helper identities, operation
counts and exact source/log hashes. Earlier CPU-only evidence is not overwritten.
