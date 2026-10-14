# Deferred OM UAV counters

The capture encoder now restores known OM Append/Counter values through the
verified normalized CS UAV setter. Counter values belong to the view, so a
temporary CS binding can initialize that same view before its OM use. Ordered
reset/KEEP proof still comes from Capture's deferred journal; compilation-time
getters are not used to invent an execution-time count.

For an OM reset the encoder emits a labelled helper group: ClearState, normalized
CS reset (`0x25e`), then explicit CS unbinding. The complete action snapshot
restores graphics bindings with KEEP counters. Constant-buffer window setters
follow the helper, so ClearState cannot discard a previously emitted CB1 window.
This also avoids leaving the temporary view simultaneously bound as CS and OM
output. A KEEP-only action does not emit another reset/helper.

The helper runs only in the saved file. Live capture forwards the application's
own calls and retains its independent CPU journal; it does not clear the game's
state to encode a frame. No new wire layout, GPA private runtime call, Replay
dependency or Replay/Format source change is introduced. Predicate, SO and
unverified state profiles keep their existing explicit refusal rules.

The fixture follows Microsoft's distinct meanings for per-counter KEEP,
retaining RTV/DSV bindings and retaining UAV bindings. See the
[OM setter contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews).
The raw OM API record is not substituted for the verified executable helper.

## Native and dual-player checks

The existing counter fixture is shared between CS and OM build targets. The OM
variant uses a one-pixel viewport, RTV slot 0 and pixel-shader UAV slot 1. Both
Counter/IncrementCounter and Append execute three actions, or a nested list
submitted twice. The expected indices, full 16-word buffer and final hidden
counts remain the prior independent CPU oracle: red/6 for indices 2,3,5;
green/2 for indices 1,4,5. Null-array KEEP, explicit `UINT_MAX` KEEP, RTV/DSV KEEP
and UAV-array KEEP are exercised through native calls.

The writer shader reads a 16-constant CB1 window starting at constant 16. Its
window contains 17, while the beginning of the buffer contains the wrong value
99. Correct buffer bytes and output therefore also verify restoration of the
window after each encoded helper. Unwrapped and wrapped WARP/hardware agree,
without Debug Layer warnings/errors or retained source objects. Deferred lists
and contexts are released before encoding.

Twenty-four OM files pass **484** checks in Flora and the original GPA kernel.
Replacing KEEP with another reset turns the image blue; deleting only the final
read/visualization Draw leaves black while counter-producing Draws still run.
Their previews remain unchanged. Flora inspection checks normalized helper
records, action counts, all result-buffer bytes, final counter values and three
shaders. The separate CS corpus guards the existing path after moving counter
setters before CB1 restoration: another 24 files pass 484 checks, for 48 files
and 968 checks in the final combined corpus.

## Original-player hidden-state observer

A development-only extension of the preserved original Draw observer checks
OM slot 1 at the actual native Draw. It reads the hidden count and all 64 buffer
bytes before/after each counter-producing call. All **108 Draws** across the 24
OM files match the independent incremental CPU model at both boundaries.
The final framebuffer remains exact with the observer enabled. Original-player
execution, closure and callback/error status are verified in fresh processes.

This uses the already hash-pinned GPA `dx11_player.dll`, the established Draw
RVA `0x27ec0`, native context slot 13, and public D3D11 readback. Research source,
logs, full byte observations and results remain ignored under
`artifacts/capture-deferred-om-counters-20261014`. Production code does not import
that code or load GPA. The baseline pins the observer and reference sources.

## Delivery and limits

Mode 32 compiles two OM counter Draws on a worker each frame, then shades the
swap-chain image from the result after immediate submission. The independent
package run checks repeated capture, busy, cancellation, rejection/recovery,
disconnect, continued execution, and both players against per-Present CPU colors.
The prior runtime control uses the same target and must refuse OM encoding
without publishing a file. Exact package and regression totals are in
[capture-deferred-om-counters-baseline.json](capture-deferred-om-counters-baseline.json).
The packaged run passes **five files / 33 checks**, with a system-only child PATH
and an actual module audit excluding Qt, Core/Replay, GPA, RenderDoc and Python.

The complete capture suite passes **135/135**. Additional KEEP cases subsequently
pass four native CS/OM suites. Both configurations build, and the full
application's related suites pass **37/37**; this is not a complete 285-test
application sweep. All GPU work is serial. This is new OM-counter evidence,
not new GF2, original GUI, golden-frame or clean-machine acceptance.
The six accepted Intel GF2 files keep their original runtime identity.

First-use KEEP without an observed reset, numeric counter value `UINT_MAX`,
extended UAV slots, broader shader-stage/resource combinations and hidden
predicate/SO/query state remain incomplete. The new native OM oracle covers PS
slot 1 with one RTV; it does not claim exhaustive validation of every slot/stage.
Historical Intel/NVIDIA discrepancies remain open. Overall capture stays partial.

A later stage resolves first-use KEEP through execution-time native UAV readback;
see [CAPTURE_DEFERRED_INITIAL_COUNTERS.md](CAPTURE_DEFERRED_INITIAL_COUNTERS.md).
The results above retain their original source/runtime identities.
