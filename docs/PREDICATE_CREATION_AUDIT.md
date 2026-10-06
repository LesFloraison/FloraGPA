# M2: predicate creation boundaries

This batch executes Device5 `CreatePredicate` (`358e`) at the recorded event.
The record contains a signed HRESULT, optional `D3D11_QUERY_DESC` and returned
identity after the captured link/device fields. The original writer is
`shimd3d64.dll` RVA `efff0`; retained read-only decompilation is
`artifacts/m2-predicate-creation-wrappers.c`. Its observed descriptor-present
payload is 37 bytes. Missing descriptors, invalid types/flags, wrong devices,
snapshot disagreement, nested creation and repeated identities without lifetime
evidence are rejected. Truncation and trailing bytes are rejected.

Successful calls create a native predicate using the saved type/flags. A saved
resource, when present, must agree with the inline descriptor. An unused object
without a resource snapshot can still be created from the complete inline data.
Failed calls and validation-only S_FALSE calls create nothing and have explicit
per-event metadata handling. This is independent of ordinary `CreateQuery`:
its zero-ID/omitted-interval limitations are not resolved by this change.

## Initial result and execution

A predicate created inside the frame starts **unissued**. Creation does not
perform the old snapshot loader's empty Begin/End initialization. The recorded
Begin/End interval must execute before the new predicate can be bound; early
binding is rejected. Predicate inspection reports `unissued` with a null result,
then `active`, then a measured GPU result (or the existing hint-unavailable
status). The resource description reports `unissued_frame_time_creation` and
the creation event. Repeated replay clears the creation-time state.

Explicit SetPredication records remain authoritative until the next setter or
ClearState. In these original captures, the final Draw snapshot omits the newly
created predicate even though the preceding setter records it. Previously,
binding that snapshot silently cleared native predication. Modes 2/3 then drew
green instead of preserving magenta, on both hardware and WARP. This is a replay
state restoration defect, not pixel variability or a need for CPU query waits.
The fix also overlays the effective binding when preparing the Draw state.

The initial observer test incorrectly gated its final-image assertion on the
snapshot's nonzero predicate and therefore never checked these Draws. Full CLI
comparison exposed that test gap. Tests now assert the final conditional binding
and pixels exactly once, and run a complete replay with no observer before each
observed replay. The failed initial batch remains in
`artifacts/m2-predicate-creation-release-predicate-creation/validation.json`;
hardware/WARP reproductions remain in `artifacts/predicate-unobserved-first.txt`.

This follows the [SetPredication contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication):
the predicate must have been issued/signaled before binding. Occlusion and
whole-stream-output overflow descriptors are supported by
[CreatePredicate](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createpredicate).
Hint predicates are not read with unsupported GetData calls or assumed to have
the same suppression guarantees as non-hint predicates.

The existing pre-frame snapshot-only fallback is unchanged and still identifies
its empty interval as player initialization. It does not recover missing
frame-before query history. That remains separate M4 work, not an inferred zero
capture result or a claim of complete predicate initialization recovery.

## Original fixtures and independent checks

[predicate-creation-corpus.json](predicate-creation-corpus.json) enrolls ten
unmodified CaptureNextFrame files produced by
`tools/native/predicate_creation_probe.cpp`:

| Mode | Behavior |
|---|---|
| 0, 1 | Empty/visible occlusion interval followed by a permitted Draw |
| 2, 3 | Visible/empty interval followed by an intentionally suppressed Draw |
| 4 | Occlusion hint with visible work and a permitted Draw |
| 5 | Failed invalid predicate creation, then valid creation |
| 6 | Validation-only S_FALSE, then valid creation |
| 7 | Two created predicates; first unused |
| 8 | Whole-SO-overflow predicate with no overflowing SO work |
| 9 | Reuse one predicate for a visible interval followed by an empty interval |

Each native and original-shim producer checks twelve frames, totaling 240.
Non-hint query values are independently checked against the known work; expected
RGBA is generated on the CPU. Modes 2/3 expect magenta; the other eight expect
green. Hint mode only verifies the permitted rendering case. This does not
certify hint suppression or positive SO overflow in a newly created object.

Hardware/WARP tests run two unobserved/observed replay pairs per file on the same
Replay instance, verify
native descriptors and identity, measured interval values, actual conditional
bindings and exact final image bytes. The producer's outputs are the oracle;
they are not regenerated by the replay implementation. Negative copies test
bad status, references, flags, descriptor conflicts, repeated/future identities,
unissued binding and all truncated creation prefixes. These modified copies are
negative tests only; accepted samples remain unmodified original captures.

## Observations and evidence boundaries

Seven additional record families are decoded explicitly: Predicate
QI/AddRef/Release (`3162`–`3164`), GetPrivateData/SetPrivateData (`3166`/`3167`),
GetDataSize/GetDesc (`3169`/`316a`). The original writer RVAs are `28f080`,
`28f780`, `28fce0`, `2908a0`, `291000`, `291d50`, `2922b0`; private-data writers
are separately retained in `artifacts/m2-predicate-private-wrappers.c`.
Private-data API payloads retain opaque pointers, not the original bytes;
FloraGPA does not dereference them or invent names. Reference counts do not
release replay storage. The four private-data/getter families occur in the new
captures. QI/AddRef/Release use writer and strict synthetic wire evidence only;
the producer's same-interface QueryInterface is not an observed wire record.

## Reproduction and limits

The final package is `out/FloraGPA-predicate-creation-final-20261003`.
The serial matrix contains 254 files and 508 independent runs: 253 files repeat
exactly and the known Helldivers file retains its localized variability. All
243 previously stable images are unchanged. The batch retains 269 diagnostic
control runs and 88 texture boundary exports; all four golden checks pass.
Twenty original-player runs of the ten new files match the independent outputs
byte for byte. This observed equality does not identify the generic original
player's adapter or certify identical device configuration. The earlier class
player discrepancy remains documented separately; it is not erased by this batch.
All 31 related CTest suites pass, including 25 predicate-creation cases and 55
Qt/Worker cases with no skips. The unchanged corpus tools also pass 13 CPU checks.
Source, binary and evidence hashes are pinned in
[predicate-creation-baseline.json](predicate-creation-baseline.json).

Build `FloraPredicateCreationProbe` explicitly. Run
`<new-output-dir> <mode> [<capture.gpa_frame> <shimloader64.dll>]`, setting
`GPA_LOCAL_INJECT=true` for the capture process. Set
`FLORA_PREDICATE_CREATION_CAPTURES` to the numbered capture directory for
`predicate_creation` CTest. Run GPU checks serially. Captures and generated
evidence remain outside Git; GPA and Python are development-only tools.

Broader device/interface layouts, positive SO-overflow creation cases, general
resource lifetime/version reuse, missing frame-before results and deferred
execution remain incomplete. Ordinary Query results/control flow are not
synthesized. M2 and M3–M6 remain open; module migration counts are unchanged.

The later [M4 initial Predicate audit](INITIAL_PREDICATE_BOUNDARY_AUDIT.md)
adds original preframe cases, keeps the measured empty-query replay convention,
and distinguishes baseline inspection from application query values.
