# Merged deferred command lists

Capture now records ExecuteCommandList on a deferred context as an immutable
edge to the source list's CPU journal. The parent native context/list separately
retains the child's resource leases. Destroying source wrappers cannot remove
the parent's required resource data, and CPU journals never own COM objects.

At actual immediate submission, bounded recursive expansion preserves every
child occurrence in compilation order. Child pipeline snapshots retain their own
context identity until lowering to the executing immediate context. Each emitted
command keeps the outer submission and an outer-to-inner merge provenance path,
including list, context, generation, call position and restore flags. Native
getters continue to sample subsequent parent actions after state restoration;
Capture does not emulate that restoration through Replay helpers.

This implements the merging behavior documented by Microsoft for
[ExecuteCommandList](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).
Only submission on an immediate context executes the merged work on the GPU.
Encoding continues to emit the existing flattened action representation; no
new GPA wire layout, Core or Replay dependency is introduced.

## Bounds and refusal behavior

Compilation charges each referenced child journal against the parent's existing
64 MiB journal budget and permits at most 16 merge edges in a path. Expansion
also detects cycles, limits total visited calls to 4096 (including bindings and
empty-list merges), and shares the remaining frame command/payload limits across
children. The visit limit prevents an empty, repeatedly shared graph from causing
unbounded traversal. Native resource leases retain their existing 8192-object cap.

Child gaps, missing history, mismatched devices and invalid arguments reject the
frame. A merge while the parent has live mappings remains unverified and refuses.
Child Map/Copy/Update writes invalidate a matching parent NO_OVERWRITE baseline;
a later parent discard can establish a fresh baseline. No hidden resource version
or counter is inferred from the child descriptor. Existing unsupported query and
hidden-state checks still apply inside every child.

## Native and player evidence

The native/wrapped fixture constructs two mapped constant-buffer Dispatch lists
inside the captured frame. A pair list merges A/B, then a root merges that pair
and A again; the next frame uses B/A/B. Source lists and contexts are released
before root execution. One root execution must produce all three dispatches,
exact integer results 22/37 and red/green pixels. Independent staging readbacks
check every result and constant-buffer byte. The prior immediate upload (17, 99)
remains frame-initial data. Both TRUE/FALSE merge restoration and immediate
restoration are checked, as are source object destruction and Debug Layer output.

Nine malformed-model/budget cases run for each captured frame on WARP and the
default hardware device: invalid arguments, missing history, wrong device, a
child gap, a cycle, depth overflow, exponential empty-list traversal, command
budget and payload budget. Existing merged Clear tests now require the retained
immutable child edge and an actual expanded command instead of expecting refusal.

Eight files pass **132** dual-player checks, including real execution, exact CPU
pixels and Flora event/resource/shader inspection. Deleting the final
Map/Unmap changes the result to the preceding version; deleting Draw leaves black.
Both controls retain the original preview. The original GPA development runner
uses its default adapter; no new original GUI or adapter-equivalence claim is made.

Independent-package mode 30 creates a mapped Draw list on a worker every frame,
merges it through two additional contexts, joins and submits it. **Five frames
pass 33 checks** against per-Present CPU colors in both players, with cancellation,
busy handling, failure/recovery, disconnect and continued native execution.
System-only child PATH and actual modules exclude Qt, Core/Replay, GPA, RenderDoc
and Python. The identical target under the previous Runtime refuses the nested
ExecuteCommandList, publishes no frame and exits normally.

Both configurations build. The complete capture-only run passes **129/129**.
After strengthening the test-only ancestry checks to verify leaf IDs, contexts,
generations, call positions and restore flags, both capture native suites pass
again (**2/2**), the application's related suites pass **31/31**, and the final
eight-file corpus passes the dual-player checks above. GPU jobs run serially.
This is a focused application regression, not a complete 279-test application run.

[capture-deferred-nested-baseline.json](capture-deferred-nested-baseline.json)
records regression totals, binary/capture hashes and raw evidence. Outputs remain
ignored under `artifacts/capture-deferred-nested-20261014`. No new GF2 or golden-frame
run is claimed. Open mappings across merges, unverified NO_OVERWRITE dependencies,
hidden/query states and broader synchronization remain unfinished; historical
Intel/NVIDIA discrepancies remain open. Overall independent capture is partial.
