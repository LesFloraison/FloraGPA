# Deferred DX11 MSAA capture

This is the evidence for the MSAA milestone. Subsequent deferred Resolve support
and its separate runtime/package validation are in
[CAPTURE_DEFERRED_RESOLVE.md](CAPTURE_DEFERRED_RESOLVE.md).

Immediate `ExecuteCommandList` now uses the capture-owned initial-resource
snapshot path for retained MSAA resources instead of rejecting every deferred
MSAA use. It snapshots at submission, preserves any earlier frame-start data,
and shares the frame byte/plane limits with immediate actions. The existing
bounded command-list expansion supplies execution order and repeated-list
identity. No Core, Replay, Qt or original-GPA dependency was added to Capture.

The supported initialization formats and view checks remain those of the existing
RGBA8, RGBA16 and D32S8 encoder. Unsupported MSAA CopyResource/region/Update/Map
mutations are rejected before initial GPU readback; the encoder independently
rejects malformed CPU models. Deferred Resolve remains unsupported. Private
sampling does not enter the application's event stream.

## Deterministic native and dual-player checks

Color fixtures seed two array layers with distinct per-pixel/per-sample integer
patterns. A worker compiles list A to change half the image and alternating
samples, and list B to reset exactly that region. The immediate thread joins
the worker and submits A/B/A with TRUE/FALSE/TRUE restoration. Native getters
verify the immediate pixel shader after each submission. Frame metadata must preserve
all three execution ordinals and the repeated A identity. Removing the last A
must produce the initial pattern, demonstrating that B and the repeated A affect
execution. Untouched regions still depend on the saved initial sample planes.

The D32S8 fixture repeats a list that writes exact depth and stencil values under
the same partial coverage, using both restore flags and checking the immediate
depth/stencil state and reference. Its negative removes both
submissions; the shared output filename is `missing-last-execution`, but the
depth test explicitly removes all deferred actions. Depth initialization covers
exact float32 bits and stencil bytes, not a visual approximation.

These checks follow the native
[ExecuteCommandList restoration semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).
All formats run at 2x/4x quality zero on WARP and the hardware adapter. Source
COM objects die before encoding. The CPU pixel oracle is independent of either
player. Application-Draw deletion and initialization-Draw deletion preserve the
preview and must change actual output in both players. Initial sample planes,
shader exports, event counts and module independence are also checked.

The final three corpora contain **36 files / 820 passing checks**. Each native
profile also refuses a malformed encoded MSAA copy, a real deferred MSAA copy,
insufficient bytes and insufficient planes for both sample counts: **48 refusal
cases** across the six native runs. Scoped positive Debug Layer queues are clean.

## Startup injection

Native application modes 40 and 41 compile their MSAA draws on a worker, join,
submit on the immediate context and release the list each frame. They exercise
direct startup creation and factory/flip creation at 2x and 4x, respectively.
The application has no Capture or Replay linkage. A per-Present CPU oracle
checks the final application output after disconnection.

The independently packaged Runtime captures **eight frames across two launches**
with a Windows-system-only child PATH. Busy/cancel, unsupported-update failure,
recovery, disconnect, continued rendering, resource budgets and process exit
pass. Automatic selection correctly requests explicit selection when the
foreground condition is not met; no automatic-success count is fabricated.
The eight files and four deletion controls pass **279 dual-player checks**.
Loaded modules exclude Qt, Core/Replay, GPA, RenderDoc and Python.

The previous `f2d92d...` Runtime, with the same new mode-40 executable, refuses
at `Deferred MSAA recording is not verified`, publishes no frame or partial
file, disconnects and lets the target finish with exact CPU pixels. This is a
before/after capture capability control, not a replay-only compatibility test.

## Regression and remaining scope

Builds, CTest results, binary identities, frame hashes, failed build attempts
and raw evidence are recorded in the
[baseline](capture-deferred-msaa-baseline.json). The first build rejected a
shadowed parameter under warnings-as-errors; the next exposed two incorrect
test field names. Both were repaired before GPU testing. Initial positive
corpora are preserved separately from the final guarded corpora.

Both configurations build. The complete capture-only suite passes **156/156**;
the related application selection, including Qt capture controls, passes
**75/75**. This is not a complete application test-suite claim. GPU checks ran
serially.

The production Replay/Format sources and replay executable are unchanged.
Historical GF2 evidence remains tied to its original Runtime; this stage does
not fix its pixel repeatability issue, complete original-GPA GUI inspection,
or validate new GF2 captures under the new Runtime. Overall independent capture
delivery remains partial. Deferred MSAA Resolve/mutations, broader formats,
hidden/query state and arbitrary application synchronization remain unfinished.

Raw captures, packages and research remain outside Git at
`C:/Users/Myrrr/FloraGPA-evidence/capture-deferred-msaa-20261014`.
