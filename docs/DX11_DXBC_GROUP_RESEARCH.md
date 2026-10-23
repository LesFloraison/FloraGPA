# Native DXBC thread group execution checkpoint

Date: 2026-10-24. This P6 checkpoint adds cooperative compute-thread execution,
private raw/structured groupshared storage, synchronization and 32-bit memory
atomics. It does not replace the production debugger or close P6 acceptance.

## Execution model and identity

`Group` constructs every lane declared by `dcl_thread_group`, generating
SV_DispatchThreadID, SV_GroupID, SV_GroupThreadID and SV_GroupIndex from explicit
group coordinates and the declared dimensions. The scheduler advances one
instruction from each executable lane in round-robin order. Its explicit model
identifier is `serial-round-robin/1`.

This is a simulated instruction serialization. It is not a recovered hardware
wave layout, lane schedule or atomic ordering. Register validity describes what
is known within that execution, not an assertion that the captured GPU used the
same schedule. Production results must carry this distinction when integrated.
Missing inputs, initial shared bytes and undefined accesses remain unknown;
unknown control or address inputs fail instead of selecting a guessed branch.

The scheduler creates a separate shared-memory snapshot for each group. Resource
snapshots supplied through `Inputs.memory` are caller-owned private UAV/SRV data
and can be shared between simulated groups. Shared changes have their own trace
field and identity namespace; a g-register index is never presented as a GPA
resource handle. Each `GroupStep` identifies the lane, global sequence, barrier
epoch and whether that step released a rendezvous.

## Synchronization and memory rules

A thread-synchronizing instruction blocks its lane until every group lane reaches
the same dynamic barrier. Identity includes instruction offset, flags, call stack
and active loop iterations. Reaching the same static instruction in a different
loop iteration cannot release the group. A terminated participant or a different
barrier causes explicit failure. Standalone `Lane` users cannot step through a
pending thread rendezvous without the group scheduler.

Memory-only fences are recorded separately from thread rendezvous. The simulated
memory model commits each instruction in order; it does not model hardware cache
reordering or cross-group global visibility. The distinction between memory fences
and thread synchronization follows the public
[sync contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sync--sm5---asm-).

Group storage is initially unknown. Loads preserve per-component validity;
out-of-range shared components do not acquire the UAV/SRV zero-read behavior.
An out-of-range shared store invalidates all of that group's shared storage,
including other g-register allocations, rather than only its selected view.
See the public [raw load](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-raw--sm5---asm-),
[structured load](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-structured--sm5---asm-),
[raw store](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-raw--sm5---asm-) and
[structured store](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-structured--sm5---asm-) rules.
Declaration sizes and strides must agree with the private shared snapshot.

The core implements atomic and/or/xor, integer addition, signed and unsigned
min/max, exchange, compare-store and compare-exchange on raw/structured g/UAV
storage. Returning forms preserve the old raw word and validity in the selected
destination component. Comparison failure records no write; exchange can establish
a known new word while its returned old word remains unknown. Out-of-range atomic
results remain unknown. No-return and returning forms have separate opcode
coverage. See [atomic addition](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-iadd--sm5---asm-)
and [compare-exchange](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-cmp-exch--sm5---asm-).
For out-of-range shared atomics, all shared allocations become undefined, as
specified in the atomic instruction sections of the
[D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

This implementation uses project-owned DXBC parsing and CPU operations. It neither
loads GPA private modules nor copies an external debugger implementation. It does
not close the still-unresolved original GPA debugger engine path documented in
`DX11_GPA_RESEARCH.md`.

## Bounds and recovery

Declarations enforce nonzero thread dimensions, the SM5 1024-thread limit and
32 KiB aggregate shared-memory limit. Replicated input values and the total number
of simulated instructions are bounded. Cancellation is checked between group
steps and during lane construction. A failed group rejects further execution.
The existing 32-frame subroutine budget now raises an error instead of silently
skipping a call at the limit.

Individual instructions validate before committing register or memory changes.
Already completed group steps remain in the private snapshot after a later
failure or cancellation. Retry requires a new group and a fresh or restored
private UAV snapshot; constructing another group resets shared storage only.
Recorded byte/validity changes can be reversed without executing the shader again.
No replay-device resource, predicate, hidden counter or SO cursor is touched by
this CPU group executor.

## Independent probes

The hardware oracle compiles complete HLSL compute shaders and executes actual
dispatches on explicitly selected WARP, NVIDIA and Intel devices. The CPU path
interprets the original compiled bytecode with private snapshots. The oracle uses
the D3D11 debug layer; validation errors fail the case.

| Probe | Evidence checked |
| --- | --- |
| Reduction | Shared writes, divergent arithmetic, reconvergence and repeated uniform barriers |
| Structured neighbors | Field stride, neighboring indices and component selection |
| Ping-pong | Five iterations with alternating shared regions and barrier epochs |
| Atomic tickets | Counter total and unique return-ticket permutation within each group |
| Unsigned atomics | Returning and non-returning bitwise/add/min/max plus exchange/compare forms |
| Signed atomics | Negative values and signed min/max |
| UAV atomics | Raw resource addresses, old values, comparison and memory fences |
| UAV neighbors | Within-group UAV visibility across device/all-memory barriers |
| System and local values | Three-dimensional dispatch/local coordinates and flattened lane identity |

All deterministic output bytes are compared exactly, including untouched backing
bytes. The ticket probe checks that each CPU and GPU group independently returns
every ticket from zero through fifteen exactly once, then compares all remaining
bytes exactly. Raw CPU and GPU hashes differ for that probe and are retained.
The test never asserts an invented association between a ticket and the actual
GPU lane that acquired it. This constraint was part of the probe from its first
run, not a relaxation after a failed exact comparison.

Every hardware case reverses all shared and UAV byte/validity changes back to its
input snapshot. Additional CPU cases check standalone barrier blocking,
termination/dynamic-barrier divergence, unknown shared memory, invalidation across
multiple shared allocations, atomic rollback, null/out-of-range atomic results,
partial-component validity, declaration mismatch, cancellation after an executed
step and group/call instruction budgets.

`research/dx11-dxbc-group-evidence.json` records completed run totals, adapter/driver
identities, shader hashes, opcode coverage, output hashes and test pass markers.
It explicitly records that the original GPU schedule was not recovered. The
earlier 31-case and expanded 35-case runs are retained alongside final validation.
Raw logs, executables and shader artifacts stay outside Git.

Final serial CTest results:

| Suite | Passed | Failed | Skipped | CTest duration |
| --- | ---: | ---: | ---: | ---: |
| dxbc_group | 38 | 0 | 0 | 4.42 s |
| dxbc_execution | 375 | 0 | 0 | 51.89 s |
| dxbc_memory | 246 | 0 | 0 | 44.64 s |

The combined run passed in 101.00 seconds. The group suite contains 30 hardware
cases, six CPU cases and setup/cleanup. Its hardware shaders cover both shared
declaration layouts, all 19 implemented memory-atomic opcodes and sync. These are
small correctness probes, not real-capture latency or peak-memory benchmarks.

## Remaining acceptance

This validates SM5 compute probes, not all SM4.x limits or feature levels. Typed
resources, hidden UAV allocate/consume counters, texture operations, cross-group
communication, data-race diagnosis, hardware wave behavior and broader atomic
boundary cases remain open. The group executor is not a global-dispatch memory
model and does not prove a unique result for racy shaders. Sensitive scalar GPU
assistance remains VS-only; it cannot silently serve as a CS substitute.

Actual GPA frame input/resource extraction, PS quad/helper/sample execution,
source/debug UI integration and complete debugger parity remain required. P5
fragment history, remaining P1 evidence, P7 dependency removal and P8 standalone
release validation are unchanged gates. No old functionality is removed here.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcGroupTests FloraDxbcExecutionTests FloraDxbcMemoryTests
ctest --test-dir build/vs2022 -C Release -j 1 `
  -R '^dxbc_(group|execution|memory)$' --output-on-failure
python tools/summarize_dxbc_groups.py `
  --log build/vs2022/dxbc-group-Release.txt `
  --log build/vs2022/dxbc-execution-Release.txt `
  --log build/vs2022/dxbc-memory-Release.txt `
  --source src/application/DxbcGroup.cpp `
  --source src/application/DxbcExecution.cpp `
  --source src/application/DxbcMemory.cpp `
  --source tests/DxbcGroupTests.cpp `
  --out artifacts/dx11-native/dxbc-group-current.json
```
