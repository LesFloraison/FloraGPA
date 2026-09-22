# Complete-command metric ranges

The native `MdFrameRanges` component migrates `md_interval.py` and
`md_frame_ranges.py`. It selects inclusive API intervals or category-2 FrameFile
ranges and brackets complete replay commands with synchronous or callback-based
counter operations. The higher-level profiling command and its Qt controls
remain separate, unfinished migration work.

## Selection and execution

Interval endpoints must be existing category-7 API IDs in ascending order.
Selection retains all intervening commands, including non-draw commands, their
names, experiment enablement and the enabled Draw/Dispatch roster. Frame range
selection uses the recovered internal API table and endpoint mapping, rejects
empty/invalid/duplicate indices, sorts by physical replay order without
renumbering and rejects overlapping results. Default selection includes every
captured category-2 range.

`Replay::run` now accepts an optional synchronous command scope. It wraps the
entire `command` call and its command observers, so counters include bindings,
experiment resource preparation and command submission. Restart and final
output readback remain outside the scope. Existing callers that omit the scope
retain their original execution and observer order.

`MetricIntervalCounter` checks each expected command, publishes only after its
last command completes and verifies the complete boundary sequence.
`FrameRangeCounter` also enforces globally increasing command IDs, queues
disjoint callback samples without waiting after every End and retains the
original range identity in delayed callbacks. Callbacks own their range data;
they do not retain pointers into the counter's transient current-range state.

Both scope implementations preserve the Python failure-state transitions,
including exceptions from commands, Begin, End, Submit and report consumers.
They leave native resource cleanup to the acquisition owner. Verification
distinguishes completed command boundaries from the later delivery of queued
reports; the owner must drain reports and validate their count separately.

## Verification

`tools/validate_md_frame_ranges.py` compares **769 scenarios** with the unchanged
Python reference: **257 selection checks** and **4,912 scope observations**.
Inputs include synthetic captures, GF2/BF1, invalid endpoint types, empty
captures, reversed endpoints, disabled draws, unsorted/duplicate range indices,
unknown command names, missing/reordered/duplicate commands, delayed reports,
empty ranges and injected failures. Every step compares state, native activity,
pending reports, method-call order, returned audits and exception messages.

`tests/MdFrameRangeTests.cpp` also exercises both real captures on the Intel
adapter through the native replay engine and Metrics Discovery:

- Three selected ranges run through `MdIterationTransport`; callback identity
  and ordering match the sorted original range indices.
- The synchronous range path and both synchronous and callback interval paths
  produce their expected report counts and complete boundary audits.
- Each measured replay produces exactly the baseline dimensions and RGBA
  bytes. This establishes presentation equality, not equality of every
  intermediate resource or measured performance value.
- A replay command exception crosses the new scope, marks that scope failed,
  prevents partial-report publication and makes verification reject the run.
- Native samples/cache and the private OA arbitration table are released.

The expanded hardware test initially raised an unreported C++ exception while
a full build was running. The original harness did not retain its message, so
the cause remains unknown. The harness now reports the capture, acquisition
phase and exception text. A diagnostic rerun and three consecutive isolated
package runs passed all four test cases without skips. The original failure is
retained in `artifacts/md-frame-ranges-native-all-paths.txt`; these reruns do not
establish a root cause or prove the failure cannot recur.

The full Release build, 17 related CTest suites and GF2/BF1 golden images plus
both disabled-draw negative controls pass. The packaged tests run with only
Windows system directories on PATH; test helpers are removed from the delivery
directory after verification.

The application remains native C++; Python is used only by development
comparisons. These range primitives are connected to real frame replay in the
integration tests. `md_iterations.py`, other profiling owners, worker artifact
orchestration and the Intel metric Qt workflow remain pending. No new UI
controls or claims of a complete profiling workflow are introduced here.

Local evidence (ignored by Git):

- `artifacts/build-md-frame-ranges-release.log`
- `artifacts/ctest-md-frame-ranges-release.log`
- `artifacts/md-frame-ranges-package-parity/validation.json`
- `artifacts/md-frame-ranges-package-tests.txt`
- `artifacts/md-frame-ranges-package-golden/validation.json`
- `artifacts/md-frame-ranges-delivery-audit.json`

Package: `out/FloraGPA-md-frame-ranges/FloraGPA.exe`; distribute its directory.
