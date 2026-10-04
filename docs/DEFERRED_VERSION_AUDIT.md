# Deferred resource versions and capture-side expansion

**Superseded rejection outcomes:** the [CB lifetime correction](CONSTANT_BUFFER_LIFETIME_AUDIT.md)
now accepts all nine omitted-sentinel originals through proved unused intervals.
The counts and expected rejections below describe the preceding fixed executable;
the original captures, binary investigation and capture-side observations remain valid.

Reviewed **2026-10-04**, following the [M2 registered gate](M2_ACCEPTANCE_GATE.md).
Production correction: `7227382`. Package: `out/FloraGPA-m3-deferred-20261004/`.
This is an M3 evidence milestone, **not acceptance of traditional Command Lists**.

## Results and boundaries

| Measurement | Result |
|---|---|
| Newly registered original captures | 18 distinct files: nine omitted-sentinel and nine complete-sentinel |
| Independent replay | Nine complete-sentinel cases pass twice; nine omitted-sentinel cases reject twice at the expected binding |
| Original player | All 18 cases replay twice to the expected green image |
| Resource boundaries | 54 exported buffers match strict CPU byte oracles |
| Diagnostic controls | 27 disabled-Dispatch runs produce the expected red image |
| Producer verification | 432 frames, 1,296 result readbacks, 432 images, 216 complete-sentinel readbacks |
| Capture-side restoration discrepancy | Three failed post-Execute binding checks in frame 6 of each Restore=TRUE mode, in both producer variants: 24 observations |
| Serialized traditional lists | Zero list resources and zero Execute records in all 18 files |
| Regression | Four related CTest suites, 162 new preflight cases, ten gate-checker tests, four isolated GF2/BF1 checks pass |
| Previous corpus preflight | All 388 cases retain their previous error sets and locations; 22 remain blocked |

The [corpus](deferred-version-corpus.json), [expected rejection gate](deferred-version-gate.json)
and [baseline](deferred-version-baseline.json) separate successful replay from
correctly located rejection. The new-batch replay rate is **9/18**; all **18/18**
meet their specifically registered acceptance outcomes. Neither number is full
DX11/GPA feature coverage. The original player's adapter/configuration remains
unproven, so equal observed pixels do not establish cross-device equivalence.

Combined with the previous fixed M2 inventory, there are now **406 registered
cases / 396 distinct capture hashes**, comprising 375 positive registrations
(365 unique positives) and 31 explicit rejection files. This batch reruns the
new 18 cases and the old 388 preflights; it does not claim a fresh GPU run of
the entire combined corpus.

## Probe semantics

`FloraDeferredVersionProbe` is an excluded-from-default-build, development-only
DX11 program. It uses two native deferred contexts and real command lists.
Mode 0 is an immediate control. Modes 1–8 encode these bits in `mode - 1`:

| Bit | Off | On |
|---|---|---|
| 0 | Finish restore FALSE | Finish restore TRUE |
| 1 | Execute restore FALSE | Execute restore TRUE |
| 2 | Lists recorded before frames | Lists re-recorded every frame |

Each list retains a different WRITE_DISCARD version of the same dynamic constant
buffer: A has factor/tag 3/1, B has 5/2. An immediate upload of 17/99 before each
Execute must not replace that retained version. Input uploads are 2, 5 and 11;
execution order is **A/B/A**. The compute shader updates four uints, with exact
post-Dispatch expectations:

```text
A: [ 3, 1,  2, 3]
B: [22, 2,  5, 5]
A: [78, 3, 11, 3]
```

The final pixel shader reads all four words and draws green only on an exact
match. Disabling any one Dispatch produces red. Intermediate byte checks prevent
a correct final marker from hiding a wrong resource version or execution order.
Every producer frame saves its raw image and all three readbacks.

A separate CB sentinel checks native post-Execute state identity. In the first
variant it is bound but never consumed by GPU work, and GPA omits its resource
record. The paired complete-sentinel variant (executable modes 9–17) explicitly
writes `[101,103,107,109]`, copies and reads it back each frame. This causes its
descriptor/data to be saved through real application work. Neither variant's
capture file is patched; the omitted originals remain independent negatives.

## Preflight correction and remaining omitted-binding gap

Previously the API decoder could decode a CB setter without checking its saved
buffer identity. A capture could therefore have no preflight errors and then
fail replay with `Missing capture entry`. Preflight now uses the existing native
binding validator for all six stages in each of three known encodings. Missing
or non-buffer references emit `constant_buffer_resource_unresolved` with event,
resource and wire type; context, descriptor and CB1 range failures also block.

New tests cover valid/null bindings, missing and wrong-type references, invalid
descriptors, truncated records, out-of-range slots, CB1 array mismatch/trailing
bytes and deferred owners. Runtime execution semantics are unchanged.

The nine omitted-sentinel files each have three located errors. Modes 0–4 first
fail at event 23/resource 24, modes 5/7 at event 25/resource 26, and modes 6/8 at
event 27/resource 28. Original playback succeeds, but this does **not** recover
the absent buffer descriptor, contents or native binding. Supporting these
unused binding intervals would require a proved lifetime analysis that also
defines event-prefix, inspection and edit boundaries. It is an implementation
opportunity with missing native-state information, not proof the final image is
unrecoverable. No placeholder buffer or guessed contents were introduced.

## Capture-side RestoreContextState observations

Native runs have zero Finish/Execute state failures. Under the original shim,
modes 3, 4, 7 and 8 fail three Execute-state checks **only in capture frame 6**;
storage results and final images still match. Finish checks remain correct.
The two variants reproduce the same observation independently.

The saved getters corroborate the producer's pointer comparisons. For example,
complete-sentinel mode 3 binds sentinel buffer 5 before execution, but getters
56/57, 88/89 and 120/121 report shader 48 and list CB 29 afterwards. These are
capture-time observations, not native-state queries made by FloraGPA replay.
The workloads deliberately clear/rebind before the next step, so the state
discrepancy does not affect their later computation.

Microsoft specifies target-state restoration for TRUE in
[ExecuteCommandList](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist),
and separately defines deferred-state restoration in
[FinishCommandList](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-finishcommandlist).
The present result is a measured limitation of this capture path. It is not a
claim that every GPA capture or every Restore=TRUE workload fails.

## Pinned original binary investigation

All addresses below are RVAs in `shimd3d64.dll`, SHA-256
`cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31`.
Ghidra ran against the existing project in read-only/no-analysis mode.

| Path | Recovered behavior |
|---|---|
| Factory `310c70`, case `9a` | Constructs `StreamCommandListDX11` with vtable `4d3210` |
| Resource dispatch `310520` | Resolves the stream writer and calls initialization/write stages |
| Vtable slot 1, `3182b0` | Validates and stores resource, identity and stream pointers |
| Vtable slot 2, `318220` | Calls virtual slot 4 before writing the record |
| Vtable slot 4, `317500` | Stores the resource pointer, then unconditionally throws failure to obtain the device context ID |
| Capture request `2ede80` → `302260` | Stores callback/path and pending capture request; no alternative serializer was established |

Thus the observed traditional resource writer cannot successfully prepare a
list through this call chain. The existence of a factory/vtable is insufficient
evidence of a usable capture path. Other routes or binaries are not ruled out.
Earlier expansion analysis at `2e4be0` is consistent with the real observations:
wrapped commands are emitted onto the immediate context and TRUE has no saved
state restoration in that recovered branch.

Evidence files include `artifacts/m3-command-list-{tables,construction,writer}.*`,
`m3-stream-resource-{callers,routing}.*` and `m3-capture-{entry,request}.c`.
This does not justify inventing list identities or treating an original player's
no-op Execute handler as successful list execution.

## Reproduction and next work

Build with `cmake --build --preset release --target FloraDeferredVersionProbe`.
Run `tools/capture_deferred_versions.py` with `--exe`, the installed `--shim`,
and a fresh `--out`; add `--materialize-baseline` for the paired variant. The
harness freezes source/executable/DLL hashes and keeps native/injected outputs
separate. Its final data-validation exit check was tightened after capture;
the frozen per-run harnesses remain the authoritative provenance.

Use `tools/validate_corpus.py` with the registered manifest, `--repeat 2`,
`--captures-root artifacts` and optionally the original `--oracle-tools`.
Export `commands` and `command-lists` for each capture into separate directories.
`tools/validate_deferred_evidence.py --artifacts artifacts --validation <run-dir>
--api-evidence <api-root> --out <new-json>` rechecks hashes, raw producer bytes,
original/native pixels, buffer exports, exact rejection diagnostics and saved
getter evidence. `tools/validate_compatibility_gate.py --suites
docs/deferred-version-gate.json` provides a serial native-only gate using its
usual root/output arguments. GPU runners must not overlap.

M3 remains open: find an unmodified traditional-list capture/usable original
writer path, resolve list identity and resource versions, then verify production
execution and event navigation. Keep unproved traditional layouts rejected.
The missing-CB lifetime proof is a separate, lower-risk compatibility item.
M4–M6 are not advanced or declared complete by this batch; the module ledger
remains 72 ported / 117 partial / 15 pending.
