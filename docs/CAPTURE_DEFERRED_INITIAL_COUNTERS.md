# Deferred execution-time counter snapshots

Deferred CS and OM Counter/Append views can now begin with KEEP. Capture reads
the actual native UAV's hidden value immediately before each immediate-context
`ExecuteCommandList`, using its existing public `CopyStructureCount` readback.
It neither infers the value from a descriptor nor reuses a compilation-time
value. Repeated submissions take fresh snapshots.

The CPU lowering model returns unresolved view IDs and first-use command
indices. Nested expansion rebases these indices. Runtime resolves all requested
values, validates the complete set, and attaches each to its first consuming
action. Later actions preserve the established reset/KEEP order. Missing,
conflicting, unbound, incorrectly indexed or unrepresentable values fail before
encoding; numeric `UINT_MAX` remains unsupported. Readback is cancellable and
budgeted, uses reentrancy isolation, and preserves application predication.

Native lifetime entries now retain the actual hidden-counter UAVs as well as
resource storage. Two views of one buffer have distinct hidden counters and
must never be collapsed into the buffer identity or recreated from descriptors.
Merged parent lists retain these leases after child and application wrappers
are released. CPU journals, definitions and frozen frames still own no COM
objects. Encoder consumes the existing capture model and verified normalized
setters; Replay and Format sources are unchanged.

## Evidence

The shared CS/OM fixture compiles with count 9, changes the immediate value to
2 or 4 after compilation, then submits KEEP-first lists. A repeated nested list
starts its second submission at the actual value 2. The independent oracle
checks indices 2,3,5 for the first frame and 1,2,3,4,5 for the repeated frame,
full buffer bytes, final counts, and red/green pixels. Wrapped and unwrapped
WARP/hardware results agree with no Debug Layer warnings or retained objects.
The OM profile also preserves its nonzero CB1 window.

A separate native test uses two Counter UAVs over the same buffer. After
compilation, their counts change from 9/11 to 2/5. Application view wrappers and
the child list are released before recording. Two parent submissions capture
2,5,3,6, leave final native counts 4/7, and preserve the original zero-filled
frame-initial buffer. All native ownership is gone while CPU history remains.

The new CS and OM corpora contain 48 files. Their 968 dual-player checks include
exact output, event counts, all result-buffer bytes, final hidden count, DXBC and
assembly. Replacing ordered KEEP with reset changes the output to blue;
removing only the final visualization Draw produces black with unchanged preview.
Existing explicit-reset CS/OM profiles are regression controls. Eight malformed
snapshot-completion variants per wrapped fixture also verify failure before
partial model mutation, including rebased nested first-use indices.

A development-only observer of the pinned original GPA Draw path independently
checks 108 Draws in the 24 new OM files: 216 exact hidden-counter boundaries and
216 exact full-buffer boundaries. Its final pixels remain unchanged. Research
code and raw observations stay in the ignored artifact directory; production
code neither loads GPA nor uses its private ABI.

Modes 33 and 34 exercise worker compilation through startup injection. They
change the live counter from compilation value 9 to submission value 2, render
from the computed buffer, and test repeated capture, cancellation, refusal and
recovery, disconnect and continued target execution. Package audits use a
system-only child PATH. Exact regression/package totals, binary hashes and the
evidence manifest are in
[capture-deferred-initial-counters-baseline.json](capture-deferred-initial-counters-baseline.json).

Final validation passes 143/143 capture CTests and 45/45 related application
CTests; the latter is not a complete 293-test application sweep. The combined
new and explicit-reset corpora pass 96 files / 1,936 checks. Two independent
package launches add 10 files / 66 checks, with no Qt, Core/Replay, GPA,
RenderDoc or Python loaded by the target. The prior runtime refuses both new
profiles without publishing a file; both targets then exit normally. All GPU
validation is serial.

## Limits

This adds execution-time initial counter evidence, not new GF2 or original-GUI
acceptance. The six accepted Intel GF2 frames retain their historical runtime
identity. Numeric `UINT_MAX` restoration, extended UAV slots, exhaustive shader
stage/slot combinations, and broader predicate/SO/query/synchronization profiles
remain pending. Historical Intel/NVIDIA pixel discrepancies remain open.
Overall capture delivery remains partial.
