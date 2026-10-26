# Native compute analysis jobs

The subsequent native debug model checkpoint adds `shader.dxbc` to the
manifested exports, with `shader_file` and `shader_sha256` in the result. See
`DX11_NATIVE_EXECUTION_DEBUG_MODEL.md` for source/model identity and validation.

## Product entry point

The native analysis Worker accepts `debug-thread` under `dx11-native/3`:

```powershell
FloraGPA.Cli analyze frame.gpa_frame debug-thread --gpa-event 893 --group 0 0 0 --thread 0 0 0 --gpu-vendor 0x10de --out analysis
```

`--warp` selects WARP explicitly. An experiment project can be supplied with
`--experiment`. `--max-bytes` controls private input snapshot storage,
`--max-steps` limits cooperative group execution, and `--max-trace-bytes` limits
the serialized journal. The existing Worker timeout, cancellation, process-tree
isolation, artifact manifest and stale-context rejection remain in use. No
RenderDoc fallback is selected. A successful output has `trace.jsonl`,
`debug.json` and the final `result.json` publication marker.

This is an additional native compute path. VS/PS debug actions, debugger UI
switching and complete debugger parity remain separate work. The legacy UI is
not removed by this checkpoint.

## Invocation identity and eligibility

`debugNativeCompute` runs one before-event replay through the existing input
snapshot path. Its boundary check executes inside the selected event's buffer,
texture and counter experiment scope, before allocating shader input snapshots.
The normal analysis pre-run is omitted for this action, avoiding an unnecessary
second prefix replay.

Direct dispatch dimensions come from the effective frame. Indirect dimensions
come from the actual replay argument buffer after preceding GPU work and scoped
buffer modifications. The buffer's indirect-argument flag, aligned offset,
12-byte extent and allocation budget are checked before reading. Dimensions must
fit D3D11's limits, and each selected group coordinate must be strictly below the
effective dimension. A zero-sized dispatch has no selectable invocation. Local
thread coordinates are checked against the actual shader's declared group size.
Disabled events fail explicitly. Deferred/normalized command reconstruction
continues through the existing replay implementation; the debugger does not
invent an immediate event for an unresolved context.

The result records the GPA event/shader identities, actual argument resource and
offset, dispatch dimensions, selected group/thread, effective frame hash and
execution model. The parent result binds these artifacts to the original frame,
effective experiment, actual adapter and Worker executable digest. The journal's
low-level eligibility field still says that the writer itself does not establish
eligibility; `debug.json` supplies the checked event-boundary evidence.

## Predication evidence

`Replay::observeBoundPredicate` checks the actual context predicate against the
replay's tracked binding. An unbound predicate allows execution. An issued,
unhinted predicate is read with a bounded deadline and cancellation checks; its
observed Boolean result is compared against the actual comparison value.
The source identifies a replayed GPU query, native replay initialization, or a
captured normalized-condition carrier. These observations do not claim to
recover the application's original query result.

Microsoft documents that a predicate suppresses commands when its result equals
the comparison value, and that hints may not prevent execution. Dispatch and
DispatchIndirect are among the affected commands. See the
[SetPredication reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication).

A hint predicate currently produces an explicit unknown-eligibility error. It
is neither silently ignored nor converted into a false skip decision. Establishing
hint behavior for the selected dispatch remains an open parity requirement.
Similarly, a confirmed skipped dispatch produces no fabricated execution trace.

## State and failure handling

The eligibility check reads resources and query results without rebinding the
pipeline or changing original UAV counters. Shader execution uses copied private
inputs. Scoped argument edits are restored by the existing experiment lifetime.
Cancelled, failed or oversized execution does not publish a journal; no failed
analysis publishes `result.json`. A fresh job/snapshot is required for retry.

The enclosing Worker bounds the entire operation, including legacy replay
readback calls. This checkpoint does not claim that all existing replay Map
paths have been converted to nonblocking waits. The native DXBC GPU helper
continues to use its previously verified bounded completion implementation.

## Validation

Release GUI, CLI, Worker and affected test targets build. The full rebuild
reports two existing warnings in unchanged `DxbcGpuOperations.cpp` (C4838 and
C4457); the new compute analysis sources introduce neither warning. Seven
affected suites finish with 263 passing tests, zero failures and zero skips:

| Suite | Passed |
| --- | ---: |
| Native compute debugging | 63 |
| Native analysis jobs | 40 |
| Native counter UI | 7 |
| Predication | 43 |
| Normalized predication | 7 |
| DXBC resource inputs | 82 |
| DXBC execution journals | 21 |

The new tests cover WARP, NVIDIA and Intel; direct and indirect dimensions;
preceding GPU copies and scoped argument edits; all unhinted predicate Boolean
combinations; explicit unknown hint behavior; native baseline and normalized
condition provenance; unchanged buffers, hidden counters and predicate bindings;
out-of-range/disabled/empty/invalid dispatches; step/input/trace budgets; native
Worker and CLI jobs; shader edits with a changed thread-group size and a nonzero
selected thread; changed experiment rejection; failed-job recovery; and loaded
module checks. Predication decisions are checked against independently executing
the original dispatch and observing its output and hidden counter increment.

The initial seven-suite run had one skip because the old original-predicate
corpus was absent. That run and its skip remain in the evidence. The unchanged
development probe generated a new corpus: 16 modes, native and original-GPA
execution per mode, with 12 verified frames per run. The complete normalized
predicate suite was then rerun with `FLORA_INITIAL_PREDICATE_CAPTURES` pointing
to the new corpus and passed all seven tests, including the original captures.
No golden, assertion or skip condition was changed. Corpus generation requires
GPA only as a development reference; replaying the saved corpus does not.

The real NVIDIA CLI runs validate BF1 event 209 (14,992 steps, 2,000 GPU helper
operations) and Helldivers event 893 (384 steps, 64 helper operations). The
independent Python projector checks Worker/request/frame/adapter identity,
eligibility metadata, artifact hashes, private storage continuity and full
reverse restoration against the preceding independent probe reports. Eleven
deliberate result corruptions are rejected. These are selected-group checks;
BF1's dispatch contains multiple groups and is not claimed as whole-dispatch
parity.

Observed Worker execution was 8,720 ms for BF1 and 1,096 ms for Helldivers.
Peak working sets were 1,759,690,752 and 1,744,084,992 bytes respectively; journals
were 13,643,506 and 322,755 bytes. These measurements include replay and driver
costs and are not a controlled performance comparison or P8 acceptance.

The compact source/binary/test/capture ledger is
`research/dx11-native-compute-debug-evidence.json`. Raw reports, journals, corpus
captures, frozen development producer and build/test logs remain outside Git.

## Reproduction and remaining work

```powershell
ctest --test-dir build/vs2022 -C Release -R '^(native_compute_debug|native_analysis|native_counter_ui|predication|normalized_predication|dxbc_input_snapshot|dxbc_execution_trace)$' --output-on-failure
python tools/validate_native_compute_debug.py --directory analysis --reference independent-probe.json --dispatch-groups 1 1 1 --worker build/vs2022/Release/FloraGPA.Worker.exe --out validation.json --negative-out negative.json
```

Set `FLORA_INITIAL_PREDICATE_CAPTURES` to a verified original-predicate corpus
before running its optional original-capture test. The validator's reference
report is generated with `FloraDxbcInputProbe --export-trace` on the same unedited
capture, shader, adapter and group; it is not a replacement for the original
independent GPU instruction probes.

The next step is a native journal reader/model and debug UI integration while
preserving forward/reverse stepping, source navigation, watches, breakpoints and
configuration migration. Hint eligibility, remaining instruction/shape coverage,
captured VS/PS invocations, complete Pixel History, legacy backend removal and
standalone acceptance remain open. This checkpoint does not close P6, P7 or P8.
