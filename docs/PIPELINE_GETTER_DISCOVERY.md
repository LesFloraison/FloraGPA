# Pipeline getter compatibility discovery

This is the historical discovery record. The subsequent implementation and
remaining boundaries are documented in [getter acceptance](PIPELINE_GETTER_AUDIT.md).

Reviewed 2026-10-04 against replay revision **b11464c**. Eight new, unmodified
GPA 2025 R1 captures expose **34 unsupported immediate Context4 getter families**.
They are implementation gaps, not missing resource contents. Production replay
and the metadata allowlist are unchanged in this discovery batch.

## Reproduction and results

The self-owned producer is `tools/native/pipeline_getter_probe.cpp`, built with
the explicit `FloraPipelineGetterProbe` target. It binds known shaders, SRVs,
constant buffers, samplers, IA buffers and rasterizer state, observes getters,
then draws an independently expected red 8×8 image. Native and shim-injected
executions each check twelve frames per mode. Getters inspect both populated
and null slots; array families also exercise zero-count calls.

| Mode | Getter group | Unsupported families / records | First blocking event |
|---|---|---|---|
| 0 | VS/GS/HS/DS/CS shader observations; existing PS getter control | 5 / 5 | 43 |
| 1 | Six-stage SRV arrays | 6 / 12 | 43 |
| 2 | Six-stage constant-buffer arrays | 6 / 12 | 43 |
| 3 | Six-stage sampler arrays | 6 / 12 | 43 |
| 4 | IA input layout, vertex buffers and index buffer; topology control | 3 / 5 | 43 |
| 5 | Rasterizer and scissor rectangles; viewport controls | 2 / 3 | 43 |
| 6 | OM RTV/DSV/UAV arrays and CS UAV arrays | 2 / 4 | 43 |
| 7 | SO targets, predication, context type and flags | 4 / 6 | 43 |

All **192 producer frames and 1,008 getter-return assertions** pass. The current
independent package rejects all eight files on both attempts: sixteen located
implementation failures. The original player completes sixteen runs and every
export matches the producer pixels. Original adapter/configuration identity
remains unproven, so this is observed agreement on these fixtures, not a general
cross-player equivalence claim.

The [catalog](pipeline-getter-discovery-corpus.json) records capture hashes and
producer image references. The [discovery baseline](pipeline-getter-discovery-baseline.json)
pins sources, binaries, raw command exports, errors and reverse-engineering
evidence. Captures and artifacts remain outside Git.

## Wire evidence

The 59 unsupported records are retained in
`artifacts/pipeline-getter-wire-samples.json`. Each has the observed 16-byte
link/context prefix. The following are observed shapes, not accepted decoders:

| Family | Observed tail |
|---|---|
| SRV / CB / sampler / CS UAV | Start slot, count, array-presence flag, 64-bit object IDs |
| Shader | Shader ID, optional class-count output, optional class-ID array |
| IA vertex buffers | Start/count and separately optional buffer, stride and offset arrays |
| IA index buffer | Buffer ID and separately optional format and offset outputs |
| Input layout / rasterizer | Returned object ID |
| Scissor rectangles | Optional returned count, rectangle-presence flag and 16-byte rectangles |
| OM combined outputs | RTV count/optional array, DSV ID, UAV start/count/optional array |
| SO targets | Count and optional object array |
| Predication | Predicate ID and optional BOOL output |
| Context type / flags | Returned 32-bit value |

Windows SDK 10.0.26100.0 interface slots, wrapper strings and all 34 serialized
record-type constants agree with the observed record IDs. Read-only Ghidra
exports are in `artifacts/pipeline-getter-wrappers.c`; the imported program MD5
matches the installed shim, whose SHA-256 is pinned separately. Some wrappers
use GPA's cached-state vtable. A matching numeric offset in decompiled text is
only a locator, not proof of a direct native vtable call or complete semantics.

## Next implementation gate

1. Add a shared checked decoder and command-inspection fields for these observed
   immediate layouts. Treat returned IDs as captured observations; do not replace
   live replay bindings with them or dereference original-process pointers.
2. Validate array bounds, presence flags, context identity, trailing/truncated
   data, illegal lengths and slot ranges. Preserve explicit rejection for
   unexplained linked/deferred layouts and other interface versions.
3. Retain exact native/WARP replay checks and original/producer comparisons.
   This first fixture set has empty class-instance, SO-target and UAV outputs;
   it does not prove their nonempty layouts. Null predicate output does not
   establish a meaningful predicate-value default. Add boundary captures before
   extending those claims.
4. Run related CTest and historical golden/corpus regressions after production
   changes. Do not accept a getter solely because its method name sounds passive.

The enrolled inventory is now **340 files: 328 accepted positive files, four
missing-information rejections and eight newly blocked implementation cases**.
The prior 332-file acceptance baseline remains unchanged. No full historical
matrix was rerun here because production code was not changed; the new producer,
capture validation and original comparisons are this batch's new evidence.
M2–M6 remain incomplete; module migration totals stay 72/117/15.
