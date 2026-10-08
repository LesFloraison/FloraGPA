# Input texture clones retain resource MinLOD

Reviewed 2026-10-08. This follows [full texture storage readback](LOD_STORAGE_READBACK.md).
It is a scoped M4 correction with M5 regression checks, not completion of either stage.

## Corrected behavior

Input texture experiments previously refused known nonzero resource MinLOD.
Without that guard, the old clone discarded RESOURCE_CLAMP and sampled from
mip zero: the preserved identity-patch control changed green to red.

The clone now retains RESOURCE_CLAMP (and the existing cube flag), receives
its complete captured storage plus the requested patches, and inherits the
proved **current** MinLOD before its SRVs are published. Disabled setters do
not change that value. A future getter is not used as initial state. Unresolved
initial LOD still refuses with the resource/event diagnostic.

The source resource's bytes and LOD are unchanged. Input inspection restores
the object-map identities after either normal return or an exception. This
is not a claim that all native pipeline bindings are rolled back immediately;
ordinary event rebinding continues to manage those bindings. Failed replay
still disallows native-state inspection; a successful retry restores that API.

## Independent evidence

Producer revision `97281ba` adds twelve unmodified original GPA captures.
All use RGBA8, four mips and point sampling: 1D arrays, 2D arrays and 3D,
with MinLOD 0, 0.75, 1 and 4. Each native frame tests five separate resources:

- Identical storage and inherited LOD.
- Changed mip zero; lower mips must not bypass the clamp.
- Changed mip one; sampled pixels must reflect the edit when accessible.
- Changed mip two; unsampled storage must remain verifiable.
- Deliberately zero LOD as a diagnostic negative control.

A sixth Draw samples the unchanged original. Each case runs twelve frames on
hardware, WARP and injected hardware: 432 final and 2,160 variant image,
packed-storage and LOD checks. The CPU oracles are constructed by the producer
before capture. All six captured Draw boundaries are also checked individually
by the C++ test, rather than accepting only the final restored image.

The input-edit tests use the preceding storage captures and these independent
variant oracles. They check complete pixels/bytes, clone identity/flag/LOD,
normal and throwing inspection, malformed patch rejection without document
mutation, project save/load, repeated replay, Undo/Redo, before/after submission
failure and retry, and disabled Draw. Additional originals check disabled LOD
setters, a later LOD reset, and unresolved initial state.

Six Qt cases exercise actual import, Undo/Redo, replay, original storage after
Draw and the empty-SRV result. No new GUI controls or explanatory panels are added.

The first expanded test run failed because it attempted native inspection after
a deliberately failed replay. The test was corrected to assert that rejection,
then retry before checking native state; production inspection guards were not
weakened. The failure and successful retry logs remain in local evidence.

## Scope

The new sampling evidence covers the formats, dimensions and filters above.
It does not certify every clamp/filter/view combination, restore missing initial
state, or resolve deferred-list resource identities. Captures and generated
evidence remain outside Git. Runtime remains native C++ without GPA or Python.
Original private-player comparisons must be interpreted with their recorded
adapter/configuration uncertainty. Same-host relocated checks do not establish
clean-host deployment or renewed long-duration stability.

Two preliminary CTest runs are excluded from final acceptance: the first
included older linked test executables, and the next was stopped after a brief
scheduling overlap with the first recovery test. The final run starts only after
both owning processes have exited, rebuilds every relevant executable, and runs
GPU work serially. These preliminary logs are retained.

The previously documented resource-table selection caption issue remains:
image/bytes can correctly show the selected resource while the binding-tree/footer
caption still names the prior binding. This batch does not change that UI state.

## Registry correction

All 39 GPU suites passed on the first complete serial matrix run. Its final
summary rejected the stale registry totals (545/535) after the twelve new cases
were added. Only these expected counters were corrected to 557/547. The original
failed `gate.json` remains intact. `gate-rechecked.json` records a complete
offline re-audit with the same `check_case` rules, original report/executable
hashes, negative logs and recomputed totals. Reconstructing the previous config
matches its recorded hash and proves no other registry field changed. No GPU
results were regenerated or relaxed to resolve this bookkeeping error.

## Accepted results

Implementation `1c84431`; producer `97281ba`.
The [baseline](lod-clone-baseline.json) pins 2,285 local evidence files,
source hashes and all 44 package files. Package:
`out/FloraGPA-lod-clone-20261008/`. Only GUI/CLI/Worker executables differ from
the preceding storage package; the other 41 files retain their hashes.

- Ten serial CTest suites pass in 302.75 seconds; 530 explicitly logged Qt rows
  pass without failures or skips. Four suites also write direct Qt logs because
  their Windows CTest standard output does not retain row totals.
- The preserved identity-patch experiment now produces the exact original green
  image, where the preceding accepted package refused and the research draft
  incorrectly produced red.
- Four same-host relocated/system-PATH package checks pass, including the actual
  Qt clone importer on the Windows platform, recovery and both game screenshots.
  Four golden/disabled-Draw controls pass.
- The 39-suite matrix has 557 registrations / 547 unique capture hashes:
  521 replay completions / 511 unique completions, and 36 located refusals.
  All preceding 545 complete preflight reports, command counts and deterministic
  images remain unchanged. Known-variable differences stay separately recorded.
- There are 1,114 ordinary attempts,
  732 control runs and
  1,032 resource exports. These are executions,
  not independent sample counts or proof of universal API coverage.
- Twenty-four original private-player runs complete. Three zero-LOD cases match;
  nine nonzero cases retain the mip-zero discrepancy. Configuration equivalence
  is unverified. Hardware/WARP producer oracles and all six intermediate C++ Draw
  checks provide the positive evidence.

Completion is separate from fidelity: the matrix records 89 passed-fidelity,
32 capture-side mismatches, 13 missing-information and 423 unassessed
registrations. Thirty-five completed registrations retain known capture limits.
M3/M4/M5 and the 72 ported / 117 partial / 15 pending module ledger remain
incomplete. The test scheduling and failed-test records above are not counted
as successful acceptance.
