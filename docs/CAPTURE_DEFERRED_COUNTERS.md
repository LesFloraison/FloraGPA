# Deferred CS UAV counter resets and KEEP

Follow-up: [OM counter validation](CAPTURE_DEFERRED_OM_COUNTERS.md) extends the
OM refusal described in this historical CS-stage report, with native and
original-player hidden-state evidence. Its initial-KEEP limits still apply.

Capture now lowers explicit CS Append/Counter resets and subsequent KEEP uses
from its own deferred journal. A pending reset belongs to the UAV identity and
survives pipeline restoration, unbinding and nested list boundaries. The first
consuming Draw/Dispatch receives that reset; later uses preserve the value
modified by the GPU. Each immediate submission starts a fresh proof of
initialization. Repeated lists execute every recorded reset again.

The model stores an explicit KEEP set separately from known numeric values.
It never computes an increment count from a shader, treats KEEP as a reset, or
uses compilation-time GPU data as an execution-time counter value. A first use
without a recorded reset refuses, including a first KEEP in an independently
submitted list. An outer reset can initialize a nested child's first KEEP.

## Encoding and boundaries

The encoder validates reset/KEEP exclusivity, view type, binding identity and
ordered initialization. It emits the previously verified normalized CSUAV
record (`0x25e`) before consumption, and leaves snapshot count fields at KEEP.
Both players execute the reset. Snapshot count fields alone are not captured
initialization evidence; the existing Replay guard remains unchanged.

The first experiment using only snapshot counts failed Flora's missing-counter
guard. A second using the raw API record (`0x3522`) executed in Flora but produced
wrong original-player pixels. Both failed corpora are retained. The normalized
record passes the unchanged CPU oracle and both players; no pixel tolerance or
Replay workaround was introduced.

OM hidden-counter state, including KEEP after a CS reset, still refuses at
encoding because that profile has not been validated. Extended UAV slots,
unknown initial KEEP, wrapped numeric value `UINT_MAX`, predicate/SO and broader
query states remain outside this increment. Bound hidden counters are treated
conservatively even if a particular shader might not use them. Production code
does not load GPA or depend on Core/Replay. The changed files belong to Capture
and its development validation tools; Format and Replay are unchanged.

## Native and player evidence

The fixture runs unwrapped and wrapped D3D11 with the Debug Layer on WARP and the
default hardware adapter. It uses both Counter/IncrementCounter and Append:

* Frame 0 resets to 2, dispatches twice with null-array KEEP between them, then
  resets to 5 and dispatches. The complete 16-word buffer contains 17 only at
  indices 2, 3 and 5; the final counter is 6 and the image is red.
* Frame 1 sets 4 on the parent, merges a child with null-array and explicit
  `UINT_MAX` KEEP, resets to 1 and dispatches. Submitting the root twice leaves
  17 only at indices 1, 4 and 5, counter 2 and a green image. Source deferred
  contexts/lists are released before encoding.

Independent native staging reads verify every buffer byte, the actual hidden
counter and each image pixel. Both devices complete without Debug Layer
warnings/errors or retained source COM objects. Seven malformed binding cases
per captured frame reject (56 checks across devices). Six encoder cases per
frame reject without publishing a file (48 checks): uninitialized KEEP,
ambiguous numeric KEEP, simultaneous reset/KEEP, OM state, unbound KEEP and
KEEP on a non-GPU command.

Twenty-four files pass **484** checks: both players execute exact CPU pixels,
Flora exposes the expected events, normalized resets, resources and three DXBC
shaders, and Flora's final resource bytes/counter match the independent oracle.
Replacing KEEP with a reset turns the image blue; deleting Draw leaves black.
Both negative files retain the positive preview. Original-player whole-buffer
and counter snapshots are not claimed; its new evidence is actual execution
and exact pixels. Its adapter selection is the development runner's default.

Mode 31 adds a self-owned startup-injected target that compiles reset/KEEP work
on a worker each frame, joins, executes and shades from the resulting buffer.
The package validation compares per-Present CPU colors in both players and
exercises busy, cancel, rejection/recovery, disconnect and continued execution.
Five files pass 33 dual-player checks with a system-only child PATH and an actual
module audit. The same target under the preceding runtime rejects the deferred
counter, publishes no file and exits normally.
Exact totals and binary identities are in
[capture-deferred-counters-baseline.json](capture-deferred-counters-baseline.json).

The complete capture-only regression passes **132/132**. Final guard/status
changes are additionally checked by focused capture/application suites and the
packaged run: **33/33** focused capture tests and **34/34** application tests pass.
This is not a full 282-test application sweep. GPU work runs serially.
Raw data, preliminary failures and package
outputs remain ignored under `artifacts/capture-deferred-counters-20261014`.
This stage adds no new GF2, original GUI, golden-frame or clean-machine claim.
The earlier scoped Intel GF2 six-file acceptance remains attached to its original
runtime hash; historical Intel/NVIDIA discrepancies and the full release remain
unfinished.
