# Immediate pipeline getter acceptance

> Subsequent [SO lifetime investigation](SO_LIFETIME_DISCOVERY.md) resolves the
> development capture-delivery issue below and verifies four resource-complete
> SO originals. Missing-buffer lifetime support remains open. The original
> getter baseline and historical failed-probe record are preserved.

Reviewed 2026-10-04. This M2 batch implements the 34 immediate Context4 getter
families identified in [the discovery](PIPELINE_GETTER_DISCOVERY.md). Their
handling is **checked observation metadata**. They do not restore pipeline state,
materialize their returned objects, or call an original GPA component at runtime.
The [acceptance baseline](pipeline-getter-baseline.json) pins the final evidence.
M2–M6 remain incomplete.

## Recovered scope

| Family | Records | Treatment |
|---|---|---|
| VS/GS/HS/DS/CS shader | `352a`, `3530`, `3540`, `3544`, `3549` | Returned shader, optional count and class array |
| Six-stage SRV | `3527`, `3532`, `3535`, `353f`, `3543`, `3547` | Start/count and optional returned IDs |
| Six-stage CB | `3526`, `352b`, `352f`, `3542`, `3546`, `354b` | Start/count and optional returned IDs |
| Six-stage sampler | `3529`, `3533`, `3536`, `3541`, `3545`, `354a` | Start/count and optional returned IDs |
| IA | `352c`–`352e` | Layout; independent optional VB/stride/offset arrays; IB/optional format/offset |
| Rasterizer | `353c`, `353e` | State identity and optional rectangle output |
| OM/CS outputs | `3538`, `3548` | RTV/DSV/UAV observations |
| SO/predication/context | `353b`, `3534`, `354e`, `354f` | SO identity array; predicate/value; context type/flags |

Numbers are hexadecimal wire types. Existing PSGetShader, topology, viewport,
blend/depth and ordinary OM getter paths retain their separate decoders.
`src/core/PipelineGetters.cpp` supplies one bounded decoder to API inspection,
preflight and replay. Presence flags, array limits, optional count dependencies,
complete lengths and immediate-context identity are checked. Linked records and
unresolved/deferred contexts are rejected. Adjacent or older record families are
not implicitly admitted. Scalar return values remain observations, not commands.

An omitted optional output array is distinct from a present zero-length array.
A zero returned object ID does not distinguish an omitted object output pointer
from a null binding in this encoding. A null predicate does not establish a
meaningful default predicate value. Returned IDs may name unsaved objects; they
are displayed as references and are never converted to native pointers.

## Original evidence and tests

The eight frozen discovery captures now pass, alongside four new originals for
optional outputs, populated UAVs, a bound predicate and a nonempty dynamic class
instance. The [positive corpus](pipeline-getter-corpus.json) has twelve files.
The original nonempty SO capture is retained separately as a
[missing-resource negative](pipeline-getter-missing-so-corpus.json).

The initial eight files preserve their original producer source/binary manifest.
The five boundary files preserve their manifest and exact producer sources and
executables under `artifacts/m2-pipeline-getter-boundaries-final/producer`.
The currently tracked boundary producer additionally attempts a SO readback and
checks capture delivery; its unsuccessful capture attempts are not substituted
for the saved originals. Captures, executables and generated outputs stay out
of Git. The class producer and all final captured bytes remain unmodified.

`PipelineGetterTests` checks twelve originals on hardware and WARP, twice per
row, with exact independent producer pixels. It also disables all new getters
and checks identical output, verifies dynamic-class dispatch buffer bytes, and
compares native binding identities/scalars immediately before and after each
getter. A mutated observation-ID counterexample verifies that metadata cannot
restore bindings or require materialization of an observed object.

Bounds tests cover every truncated prefix of all saved getter payloads, trailing
bytes, invalid flags/counts, absent array counts, nonzero links, zero/unknown or
wrong-category contexts, and matching offline/runtime rejection at the event.
The focused run passes 30 cases without skips, including 2,426 truncated prefixes,
94 invalid flags, 110 invalid counts and 170 malformed whole-frame controls.
These synthetic negatives do not count as original compatibility captures.

## SO boundary and failed development probes

The unmodified SO file binds buffer 44 at event 43, observes it with SOGetTargets
at event 45, then unbinds it before drawing. GPA did not save buffer entry 44.
The getter payload is decodable, but the existing strict setter implementation
cannot materialize or validate its missing descriptor. Preflight now emits
`stream_output_resource_unresolved` at event 43/resource 44; runtime preserves
its explicit failure. No buffer is fabricated and the setter is not silently
skipped. Whether this unused binding lifetime can be proven irrelevant is a
separate compatibility question; the absent descriptor alone does not prove
that recovering the final image is impossible.

A follow-up producer copies and reads back the SO buffer, intended to make GPA
save the resource and to supply a byte oracle. Its native and injected workload
checks pass, but the attempted runs have not delivered a capture file. Even
with bounded waiting, successful Present results and a separate output drive,
capture delivery remains unconfirmed. Those runs are failed research evidence,
not accepted nonempty-SO production replay. The original saved SO file and all
failed output directories are retained.

Two earlier exploratory probes crashed: VSGetShader with a null required shader
output, and SOGetTargets with count one and a null array on this local runtime.
Breadcrumb runs isolate the calls; original sources/executables and logs are
archived. They are not FloraGPA replay failures or accepted inputs. Their corrected
saved boundary workloads complete twelve native and twelve injected frames each.

## Remaining gates

- Obtain a saved original SO readback capture and independently verify its buffer
  bytes and the nonempty getter path. Keep the missing-resource lifetime case separate.
- Extend observed getter layouts only with original evidence for further interface
  versions and populated shader stages/descriptor combinations.
- Continue the M2 command audit; metadata observations do not implement deferred
  command-list construction, execution or resource-version semantics for M3.
- Missing initial state, original workload races and hardware-specific behavior
  retain their own diagnostics. No shader rewrite or global tolerance change was made.

The final matrix, original-player outcomes, package hashes, Qt checks and historical
regressions are recorded in `pipeline-getter-baseline.json`; image agreement does
not establish unidentified original-player device/configuration equivalence.
