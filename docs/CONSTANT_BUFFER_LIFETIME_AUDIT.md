# Unused missing constant-buffer bindings

Reviewed **2026-10-04**. This compatibility correction follows the
[deferred-version investigation](DEFERRED_VERSION_AUDIT.md). The nine omitted-CB
captures that previously rejected now replay through proved unused intervals.
The files and original capture-side state observations are unchanged.
Production commit: `6bc3cca`.

## Accepted semantics

An absent resource identity is not sufficient permission to skip a binding.
`proveUnusedConstantBufferLifetime` checks the original setter, its immediate
context and each subsequent command until an explicit close. It tracks the
affected shader stage and missing slots, including partial overwrites and
repeated missing bindings. Closure requires every unresolved slot to be
overwritten with a saved buffer/null or a same-context `ClearState`.

Only checked CB setters, pipeline getter observations, annotations, CommandList
QueryInterface observations and saved-resource Map/Unmap operations can occur
inside the accepted interval. A GPU command or an unclassified state operation
blocks the proof. Even a disabled Draw/Dispatch blocks: replay prepares its
snapshot and must not hide unresolved native bindings. Uploads cannot refer to
the missing storage. Linked setters, context changes and malformed records reject.

At replay, saved slots retain their native bindings; absent slots are temporarily
unbound until their proved close. No buffer object, descriptor or contents are
fabricated. This temporary internal state is not exposed as recovered state:

- A prefix ending inside the interval, including immediately before its closing
  command, rejects. A prefix after the close can succeed.
- Native per-command observers and command-scope callbacks reject these intervals.
  Draw-only observers after closure remain available.
- Changed command payloads and CB binding edits within the interval reject;
  disabling the only effective close cannot certify the interval.
- Unexpected commands and unsupported layouts continue to reject. A later Draw
  snapshot alone is not used as evidence that the binding was harmless.

Preflight emits `constant_buffer_unused_binding` warnings and records opening
event, closing event, shader stage, missing resource IDs and
`native_binding_recovered: false`. Runtime JSON reports the same provenance in
`unused_constant_buffer_lifetimes`. All-missing setters are classified as
metadata for this proved interval; mixed setters retain execution of saved slots.
Inspection of the missing buffer itself still cannot recover its contents.

## Evidence

The [current corpus](cb-lifetime-corpus.json) and [gate](cb-lifetime-gate.json)
reuse the original 18 capture hashes. The prior negative
`deferred-version-corpus.json`/gate/baseline remain historical evidence for the
earlier executable, not expected results for the corrected executable.

| Final package measurement | Result |
|---|---|
| Original captures | 18/18 replay successfully: nine omitted-CB plus nine complete-CB |
| Repeated native replay | 36 successful attempts; exact green images; dependency audits pass |
| Resource exports | 108 strict post-Dispatch byte checks pass |
| Disabled-Dispatch controls | 54 runs produce the exact red image |
| Original player | 36 successful runs, same observed output; adapter/configuration equivalence unproven |
| Proven omitted-CB intervals | Three per omitted-CB capture; none in its complete-CB counterpart |
| Previous corpus preflight | 388 outcomes unchanged, including the same 22 located rejection sets |

The producer's 432 frames, 1,296 result readbacks and 216 complete-sentinel
readbacks are rechecked from the prior immutable evidence. They were not
recaptured in this batch. All 18 files still contain expanded immediate streams,
not serialized traditional Command Lists. The original capture-side Restore=TRUE
discrepancy is preserved rather than repaired by changing game/shader behavior.

`ConstantBufferLifetimeTests` covers all 18 setter encodings across six stages,
including mixed saved/missing slots, partial closes, new missing identities,
wrong slots, GPU consumption, Execute records and invalid links. Original
workloads run on hardware and WARP, twice per case, with additional prefix,
observer and edit rejection checks. Every truncated prefix of the representative
setter, upload, Unmap and closing setter is rejected. Further whole-frame
counterexamples cover invalid contexts, resource families, counts, trailing data,
missing upload storage and invalid Map kind. The suite passes **165 cases**.

Ten related CTest suites pass after fixture corrections; the optional getter
suite was rerun with all original fixtures and passes without skips. Four
GF2/BF1 golden/negative runs pass in an isolated runtime environment. The Qt
suite additionally opens the omitted-CB original, verifies its exact image,
expands compatibility diagnostics, navigates events and replays again.
All **57 Qt/Worker cases** pass without skips.
See [baseline](cb-lifetime-baseline.json) for final Qt counts and evidence hashes.

Earlier failed test runs remain in `artifacts/`: an obsolete SO directory and a
getter-producer directory without a delivered mode-10 capture were corrected to
the saved original fixtures. A newly added malformed-capture test initially kept
a mapped file open while attempting to replace its temporary fixture; its scope
was corrected. These were test setup errors, not reasons to modify originals or
weaken replay checks.

## Reproduction and remaining work

Package: `out/FloraGPA-cb-lifetimes-final-20261004/`.
Run `validate_corpus.py` against `docs/cb-lifetime-corpus.json` with the usual
`--exe`, `--captures-root artifacts`, `--repeat 2` and optional original
`--oracle-tools` arguments. `validate_compatibility_gate.py` accepts
`--suites docs/cb-lifetime-gate.json` for the serial native gate.

For the independent saved-evidence audit, run `validate_deferred_evidence.py`
with `--corpus docs/cb-lifetime-corpus.json --gate docs/cb-lifetime-gate.json`
and its artifact/validation/API-output paths. It checks raw images and buffer
bytes as well as matching preflight/runtime lifetime provenance. CTest originals
use `FLORA_CB_LIFETIMES=.../artifacts/m3-deferred-version-originals`.

The combined inventory remains **406 registrations / 396 distinct hashes**,
now **384 positive registrations (374 unique) / 22 explicit rejection files**.
This is a nine-file compatibility gain, not a new overall completion percentage
or a fresh full-corpus GPU gate. Traditional list identity/order/resource-version
recovery remains open in M3. M4–M6 and the module ledger are unchanged.
