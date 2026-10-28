# Native compute debug sessions and UI

## Scope

The native debugger tab submits `debug-thread` directly through the native
Worker. It does not recapture an RDC or invoke an external debugger. Existing
VS/PS/CS tabs remain available during migration. This checkpoint does not claim
complete shader debugging, Pixel History, or standalone release acceptance.
The tab is now named `Native execution`; its vertex continuation and validation
are documented separately in `DX11_NATIVE_VERTEX_DEBUG.md`. Explicit private
primitive PS sessions are described in `NATIVE_PRIVATE_PIXEL_DEBUG.md`; they do
not qualify original fragments or replace the full pixel debugger during migration.

`NativeAnalysisController` performs hashing, isolated Worker execution, artifact
verification and model construction in its background operation. The same
cancellation token and deadline cover loading. Cancellation before delivery
discards the completed session. The controller also exposes asynchronous loading
of saved job metadata and artifact bytes; this is infrastructure for archive
import, not a GUI import action yet.

## Acceptance and ownership

`NativeDebugSession` validates the request/result context, exact three-artifact
manifest, total payload budget, hashes and debug metadata. It binds the trace to
the effective shader, frame digest, GPA event/shader, selected group/thread,
group size, dispatch bounds, observed predicate eligibility, execution model,
actual adapter LUID and helper GPU identity. It checks trace completion and
record/byte counts. A selected vendor or WARP cannot silently change.

The original frame context and effective-frame provenance remain separate. An
experiment can change the effective shader/frame; the session does not infer
that an edited frame equals the original. Replay-side eligibility remains
`eligible_in_observed_replay`, not recovery of the original GPU schedule.

Verified trace bytes are written into a session-owned temporary directory. The
model keeps the reader's Windows read lock. The Worker directory and transport
byte array can then be released. The reader closes before the session directory
is removed. Model ownership transfers exclusively after asynchronous loading;
concurrent access is not supported. Source-value callbacks hold weak references
and cannot keep discarded sessions alive.

MainWindow checks the current frame/experiment/adapter context before delivery.
The view additionally checks its request generation and invocation selection.
Changing event, experiment, adapter or coordinates invalidates the displayed
session; old results cannot replace the new selection. Existing automatic replay
timers are stopped before native analysis, as with native counter measurement.

## Interaction and export

The view provides group/thread selection, instruction and source navigation,
forward/reverse stepping, source over/out where mappings are available,
instruction/source breakpoints, conditions and hit counts, watches, configuration
import/save, source text, disassembly, raw registers and current call-stack data.
Registers retain raw components and an explicit XYZW validity column. Display
interpretation is selectable. Unknown values and missing source stay unknown.
The final position is the terminal whole-group state, with no invented source
scope. Breakpoint/watch controls use the existing native components.

`Export session` stores `job.json`, `result.json`, the exact `debug.json`,
`shader.dxbc`, `trace.jsonl`, and the current `state.json` in a ZIP. Original
artifact bytes preserve their accepted hashes. State includes the selected
position, raw registers, source values, configuration and watches. Programmatic
current-state JSON export is also available.

`FloraNativeDebugSessionProbe <job-directory> <report.json> <archive.zip>` checks
load/step/rewind/release behavior and, when `state.json` exists, restores its
configuration and position and verifies state/register equality.
`tools/validate_native_debug_session.py` independently verifies ZIP CRCs, exact
members, manifest hashes, job/result identity, and optional byte equality with
the original exports. With `--probe`, it reloads an independently extracted
archive and compares the complete re-exported state.

## Validation

Six affected suites record **65 passes, zero failures and five existing skips**:
session 29, native UI 7, model 10, native counter UI 7, checkpoint UI 9 (one skip),
and legacy replay-debug UI 3 (four skips). The new UI suite covers actual
MainWindow → Worker → session → view execution, cancellation and retry on local
NVIDIA, Intel and WARP. The synthetic source fixture covers raw restoration,
source/watch display, conditional and instruction breakpoints, stale result
rejection, configuration rejection, terminal state and export. Twenty-four
session mutations are rejected, in addition to budget/cancellation checks.

The five skips are unchanged missing optional corpora:
`FLORA_CHECKPOINT_FIXTURES`, `FLORA_REPLAY_DEBUG_REPORTS`, and
`FLORA_DEBUG_SOURCE_CAPTURE`. No old golden, assertion or skip policy changed.
The native UI test permits a hardware-unavailable skip on other machines; all
three adapter cases passed on this host. The view screenshot was inspected after
applying the application's appearance; raw values and validity use separate
readable columns.

Previously saved BF1 and Helldivers Worker results from the execution-model
checkpoint were loaded and archived by the new session probe. These are consumer
checks of recorded jobs, not new captured GPU runs or a claim about current
Worker binary equality. Selected/global steps remain 52/14,992 and 6/384.
Initial register state restores exactly. Session load times were 1,639 ms and
69 ms; total probe times including traversal and export were 2,967 ms and 112 ms.
Both archives pass byte comparison and saved-state reload. Neither shader has
source metadata. The independent compiled source fixture reloads 31 mapped
selected steps and preserves its step-2 watch value `total + 1 = 11`.

Evidence hashes and exact logs are recorded in
`research/dx11-native-debug-session-evidence.json`. Local ZIPs, screenshots and
capture payloads remain outside Git.

## Remaining gates

The subsequent `DX11_NATIVE_DEBUG_OPERATIONS.md` checkpoint moves interactive
navigation/export to a cancellable background controller and records actual
journal response measurements. Maximum-size UI/memory acceptance remains open.
Archive import UI, full call-frame selection,
assembly instruction selection/run-to, legacy configuration migration, remaining
CS source semantics, native VS/PS invocation integration and full shader/fragment
parity remain open. Existing debugger features must remain until their native
equivalents pass the capability matrix. P7 dependency deletion and P8 standalone
packaging/module/performance acceptance have not been reached.
