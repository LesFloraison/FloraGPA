# Native DXBC hidden UAV counter checkpoint

Date: 2026-10-24. This P6 checkpoint implements counter instructions in the private
DXBC executor and reads counter inputs through native GPA-frame replay. Production
debug actions still use the migration backend; this is not P6 or release acceptance.

## Public semantics and implementation

Hidden counters are keyed by UAV view identity. Aliased slots share a counter;
different views of the same buffer have independent counters. `Counter` preserves
the distinction between an unavailable value and a known zero. A `CounterChange`
records the view, previous state and next state independently of resource bytes.

`imm_atomic_alloc` returns the previous unsigned value and increments it modulo
2^32. `imm_atomic_consume` decrements modulo 2^32 and returns the new value. These
rules follow the public [allocation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-alloc--sm5---asm-)
and [consumption](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-consume--sm5---asm-)
instruction contracts. Tests include exact zero/all-ones wrap behavior on real
D3D11 devices. Missing snapshots fail explicitly; unavailable counter values and
unbound returned values remain unknown rather than becoming guessed zeroes.

The `_opc` declaration flag requires a COUNTER view. Counter instructions without
that flag require an APPEND view. An ordinary structured declaration without either
counter instruction may use a COUNTER view but not an APPEND view. Mixing allocation
and consumption on one view, including through different slot aliases, is rejected.

Append indices belong to the invocation that allocated or consumed them. The
executor tracks this permission separately for each lane and rejects other index
accesses. It invalidates the private tail beyond a known initial Append count, or
the whole view when the count is unknown. Newly allocated structures start with
unknown validity until stores establish their contents. Raw bytes are retained
separately from validity. The public
[D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines these lifetime and storage rules. It also allows a single 32 KiB structured
TGSM allocation; the earlier 2048-byte UAV stride cap no longer incorrectly applies
to g-register storage. A boundary store/load probe verifies that case.

The model retains the group's `serial-round-robin/1` execution identity. It chooses
one private ordering and does not recover the original GPU's atomic schedule,
Append storage reordering or opaque index assignment. Future debugger results must
preserve that distinction. Within an invocation, allocation/consumption, register
results, bytes and validity are committed only after the instruction validates.
Malformed destinations and falling off the program leave those states unchanged.

## Native replay snapshot

`snapshotCounters` requires a successfully completed replay stopped immediately
before the requested draw/dispatch. It resolves effective bindings, enumerates
bound counter views and uses the existing native `CopyStructureCount` readback.
GPA view IDs identify counter records; u-register slots map to those IDs. Compute
and graphics snapshots use the same typed representation.

Event experiment overrides are scoped through `Replay::inspectEventInputs`.
Returned snapshots contain the overridden input while the replay counter and
buffer state are restored after inspection. Missing captured initial values remain
empty with an explicit reason. Cancellation is checked before inspection and around
each readback; cancellation inside the event-edit scope restores state. The existing
native readback includes a driver `Map` call, so Worker process deadlines are still
required for a hung driver. No in-process callback can guarantee cancellation of
such a call.

The executor operates on copied CPU bytes and counters. It never updates the replay
UAV through a simulated instruction. Tests compare original replay bytes/counters
after execution and reverse every private counter, byte and validity change.

## Reproducible evidence

`FloraDxbcCounterTests` generates small GPA frames, compiles HLSL with the installed
Microsoft compiler and submits the original compute shaders through native replay.
It uses explicit WARP, NVIDIA and Intel selection. Thirty GPU cases cover ordered
and Append allocation/consumption with one/four lanes, plus ordered overflow and
underflow. Counter values and deterministic outputs use exact comparisons. For
four-lane ticket/payload order, each execution is checked independently against the
expected permutation. Untouched output bytes remain exact. The tests preserve raw
output/storage hashes and do not equate those hashes with recovered scheduling.
The initial one-lane Consume probes also record the observed tail element on these
three devices; that observation does not establish a universal Append layout.

Additional cases cover view aliases, unknown/unbound values, declaration validation,
rollback, invocation ownership, unknown initial replay values, graphics snapshots,
event overrides and cancellation. D3D11 debug-queue errors fail the hardware cases.
The first run passed 32 cases; the expanded suite passes 40, with no failures/skips.
The final serial regression passes 40 counter, 38 group, 375 execution and 246
buffer cases: 699 total, zero failures/skips, 107.88 seconds wall time.
An intermediate compile error in the new test was fixed before rerunning it; its
build log is retained outside Git. No existing golden or skip was changed.

Reproduce with:

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcCounterTests FloraDxbcGroupTests FloraDxbcExecutionTests FloraDxbcMemoryTests
ctest --test-dir build/vs2022 -C Release -R '^dxbc_(counter|group|execution|memory)$' --output-on-failure -j 1
python tools/summarize_dxbc_counters.py --log build/vs2022/dxbc-counter-Release.txt --source tests/DxbcCounterTests.cpp --out artifacts/dx11-native/dxbc-counter-rerun.json
```

The checked-in ledger is `research/dx11-dxbc-counter-evidence.json`. It includes
test/shader/frame/output identities and source identities; generated captures,
binaries and raw logs stay outside Git. These probes require neither RenderDoc nor
Intel GPA private runtime modules. They do not validate full VS/PS counter parity,
typed resource execution, sampling, quad/helper semantics, source-level UI behavior,
real-capture debugger parity or standalone packaging. The original GPA debugger
engine path remains unresolved as recorded in `DX11_GPA_RESEARCH.md`.
