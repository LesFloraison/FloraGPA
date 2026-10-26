# Cancellable native debugger operations

## Execution and presentation

`NativeDebugOperationController` now runs native instruction/source movement,
breakpoint searches and session export outside the UI thread. A single operation
owns mutable session access until completion. The view disables conflicting
controls and does not expose its mutable model while an operation is active.
MainWindow includes this activity in its busy/cancel handling.

The Worker/session loader prepares an initial display snapshot. Subsequent
operations return position, instruction state, raw register components and
validity, interpreted register rows, source values, location, call-stack data and
watches. The UI renders these values without seeking the trace. Its source/watch
panel reads only the selected snapshot; it cannot trigger a disk-backed state
walk on the UI thread. Changing event/experiment/adapter invalidates the request
generation, cancels outstanding work and rejects its eventual result. The
operation retains the session until it has stopped.

Configuration and navigation retain the existing model's exact shader/source
identity requirements. Direct synchronous model and current-state JSON APIs
remain available to noninteractive callers. Interactive navigation and the
session-export action use the asynchronous controller.

## Cancellation and commit boundaries

The operation deadline starts at submission, including queue time. It covers
state movement, source searches, watch inspection and archive generation.
Cancellation checks run between complete journal steps and during register,
source, hit-count and archive-chunk loops. No captured resource or GPU state is
changed by these post-analysis operations.

If navigation fails or is cancelled, the displayed position remains unchanged.
The reader may hold a coherent intermediate projection; the next requested
operation resumes from it. Cancellation does not force a potentially long
rewind on the UI thread. The old display snapshot and register interpretation
remain visible. A retry reconstructs the requested boundary normally.

Source hit-count caches now build a local complete encounter vector before
publishing it. Interruption cannot leave a partially populated cache looking
complete on retry. Existing checkpoint/source navigation uses the same cache
implementation; its behavior without cancellation is unchanged.

The shared ZIP writer checks cancellation for file-backed and in-memory payload
chunks and immediately before atomic publication. Cancellation leaves any old
destination unchanged and removes the temporary output. Once publication has
succeeded, a later cancellation cannot turn that export into an alleged failure.
Likewise, navigation that passed its final cancellation check wins over
a later cancellation. A changed view context still rejects the old result.

## Validation

Nine affected suites record **95 passes, zero failures and five existing skips**:
operations 8, native UI 8, sessions 29, native model 10, checkpoint model 15,
checkpoint UI 9 (one skip), legacy replay-debug model 6, legacy replay-debug UI 3
(four skips), and native counter UI 7.

New checks interrupt encounter-cache construction and projection, verify exact
register restoration after retry, exercise operation cancellation/deadlines and
destruction, preserve an existing archive during both payload write modes, and
cancel after archive publication but before result delivery. The queued
cancellation test holds the worker pool until cancellation is submitted, so it
does not depend on winning a timing race. UI tests await asynchronous actions,
invalidate an in-flight session, reject its stale completion and retry with a
new session. Existing MainWindow tests still execute native jobs on NVIDIA,
Intel and WARP, including cancellation/retry. No old golden or assertion was
weakened. The five skips remain missing optional checkpoint/replay-debug corpora.

`FloraNativeDebugOperationProbe <job-directory> <report.json> <archive.zip>`
measures an event-loop heartbeat while running actual saved journals through
the operation controller. BF1 and Helldivers inputs are recorded Worker jobs
from the execution-model checkpoint, not new GPU captures. Measurements on this
host were:

| Journal | Terminal movement | Cancelled rewind | Retry to initial | Export | Largest heartbeat gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| BF1, 14,992 global / 52 selected steps | 584.97 ms | 5.96 ms | 706.53 ms | 46.32 ms | 2.46 ms |
| Helldivers, 384 global / 6 selected steps | 15.40 ms | 5.78 ms | 8.52 ms | 13.75 ms | 1.54 ms |

The rewind cancellation timer was set to 5 ms. Both cancelled requests kept the
terminal display position, and retries restored initial raw registers exactly.
Heartbeat timing describes the controller event loop, not end-to-end rendering
latency for arbitrary captures. No permanent performance threshold or skip was
introduced from these single-run observations.

Independent Python ZIP validation compares original artifact bytes and reloads
the asynchronous exports with the C++ session probe. Both captured archives and
the source fixture pass. The source fixture preserves position 2, configuration
and watch value `total + 1 = 11`. The native view screenshot was inspected after
export. Logs, binary/artifact hashes and measurements are recorded in
`research/dx11-native-debug-operation-evidence.json`; large payloads stay outside
Git.

## Remaining scope

Maximum-size journal/widget population, metadata-copy cost, peak memory and
representative full UI performance still require P8 validation. This change does
not establish that every file-system operation has bounded OS latency. Archive
import UI, assembly selection/run-to, complete call-frame selection and legacy
configuration migration remain open, as do native VS/PS input integration,
remaining CS/source semantics and full Pixel History. Legacy features remain
available. P7 dependency deletion and P8 standalone release gates are not closed.
