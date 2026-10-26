# Native DXBC GPU completion

## Execution model

`GpuCompletion` owns synchronization for one private immediate D3D11 context.
It replaces six independent staging Map loops and the pixel coverage query loop
in `GpuOperations`. Each submission establishes one deadline shared by fence
completion, Map retries, all texture subresources and subsequent queries.
The existing default operation timeout remains 5,000 milliseconds. Cancellation
checks and bounded waits continue after submission. An exception poisons the
completion object and the enclosing GPU helper; a fresh analysis job must create
a new helper.

Automatic mode queries Device5 and Context4 on the existing private device,
creates an unshared fence and owns one manual-reset event. It signals after the
operation's copies and queries, registers the event, and flushes. A successful
fence wait observes both the submitted value and event notification before event
reuse. Wait slices are at most ten milliseconds, constrained by the remaining
deadline. Scheduler latency may exceed the requested interval.

Fence completion does not replace nonblocking Map or query readiness checks.
The independent NVIDIA observations demonstrate why those checks are necessary.
The existing short yield allowance for texture UAV readback remains bounded to
500 microseconds from submission; other unsuccessful readbacks retain a
one-millisecond sleep. No global timer resolution or power setting is changed.

An unavailable interface, `E_NOTIMPL` or `DXGI_ERROR_UNSUPPORTED` retains native
polling on the same device. Other creation/signal/registration failures propagate
as errors. The internal `Poll` selection permits exercising that path explicitly;
it does not select another GPU or an external backend. Actual older Windows and
unsupported-driver execution remains a separate environment validation gate.

The helper identity is `dxbc-gpu/16`, with `completion_wait` set to either
`fence-event/1` or `native-poll/1`. Raw shader semantics and pixel/compute cohort
models are unchanged. These operations remain private to the diagnostic task.

## Lifetime and tests

Cancellation does not add a destructor wait on outstanding GPU work. The
completion interfaces are released before closing the owned event handle. Tests
exercise cancellation after submission, poisoned-instance rejection, fresh
completion instances, handle value reuse and stable handle counts. Worker-level
process termination remains responsible for recovering an unresponsive driver;
these tests do not simulate physical GPU removal or a hung kernel.

`FloraDxbcGpuCompletionTests` runs automatic and explicit polling modes on WARP,
NVIDIA and Intel. The matrix includes 32 exact buffer copies and preceding query
checks per mode/adapter, four texture subresources sharing one submission, 64
cancelled submissions followed by fresh retries, deadline expiry, and actual
VS/PS/CS typed reads and CS writes. Original private input bytes remain unchanged.
Timeout checks require the specific timeout error, including an expired deadline
after an earlier successful readback. Invalid devices, deferred contexts, missing
resources and overlapping submissions are rejected.

The first test draft exposed two test assumptions, retained in the raw logs:

- A cold WARP process gained handles during its first operations. A separate
  completed-operation control also rises to a fixed plateau. After 32 completed
  control operations, the original cancellation handle bound is retained; the
  observed 64-cancellation control run remains exactly at 490 handles. Counts for
  every control and cancelled iteration are recorded, rather than suppressing
  the initial failure or raising the allowed delta.
- A 200 ms synthetic shared-deadline budget intermittently expired before its
  first Intel readback. The final test uses the existing product's 5,000 ms budget,
  verifies the first readback, then waits beyond that same deadline and requires
  the next readback to fail. Product timeouts and historical golden data are
  unchanged. The injected one-millisecond deadline test remains separate.

The initial build also exposed Windows min/max macros leaking through the new
header. The public header now forward-declares D3D interfaces and mapping data;
platform headers remain in the implementation.

## Verification checkpoint

Release GUI, CLI, Worker and all 22 affected DXBC test targets build. The final
matrix passes 7,958 tests with zero failures or skips. A conversation interruption
stopped the first CTest invocation after five completed suites; its partial
gradient log is retained. After verifying that its process was gone, the remaining
17 suites were run serially. The ledger retains both original CTest logs and
rejects duplicate completed suites. The sum of completed-suite times is 1,132.02
seconds, excluding the interrupted gradient attempt and build time.

Ten strict evidence gates pass, including explicit and implicit sampling,
derivatives, resource queries, expanded texture formats, texture UAVs and
completion recovery. Ten negative evidence mutations reject missing checks,
changed execution identity, excessive retained handles, false captured comparison
success and duplicate CTest evidence. The ledger is
`research/dx11-dxbc-gpu-completion-evidence.json`.

On the selected NVIDIA adapter, captured BF1 event 209 executes group `(0,0,0)`
with 14,992 CPU steps, 2,000 GPU operations and 1,488 texture changes in 2,380 ms
of execution time. This is a selected-group observation, not whole-dispatch parity.
Helldivers event 893 executes 384 steps and 64 GPU operations; its single direct
dispatch comparison preserves observed inputs and matches resource 886's 256
output bytes exactly. The prior captured evidence is not rewritten.

For the nine suites shared with the preceding checkpoint, completed suite times
sum to 480.26 seconds, versus 1,343.00 seconds previously. This is not a controlled
comparison across power state, driver scheduling or host load. It does not close
the release performance gate.

## Remaining scope

The independent research is recorded in
`research/DX11_GPU_COMPLETION_RESEARCH.md`. This change does not close captured
quad provenance, helper memory semantics, full Pixel History, debugger/source/UI
integration, RenderDoc dependency deletion or standalone release acceptance.
Suite wall times are observations; a controlled application performance and
memory comparison is still required for release.
