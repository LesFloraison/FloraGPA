# M2: resource minimum LOD replay

> Unused static SRV checks are superseded by the [shader usage audit](MIN_LOD_USAGE_AUDIT.md).
> This batch's baseline remains unchanged. The later [storage readback correction](LOD_STORAGE_READBACK.md)
> supersedes its nonzero-LOD export restriction. [Clone inheritance](LOD_CLONE_INHERITANCE.md)
> subsequently restores input experiments at proved current MinLOD. Unknown-state
> boundaries and wider UAV/output/interface dependencies need their own evidence.

The immediate Context4 records `SetResourceMinLOD` (`0x3515`) and
`GetResourceMinLOD` (`0x3516`) now have checked decoding. The setter calls the
native DX11 API at its captured event. Getters retain captured observations;
they do not overwrite an intervening experiment's native state.

This follows the immutable [discovery batch](MIN_LOD_DISCOVERY.md). The original
six capture files and their earlier failures are retained unchanged. Six further
unmodified original files, produced by `tools/native/min_lod_boundary_probe.cpp`,
exercise initialization and event order:

| Mode | Boundary | Result required |
|---|---|---|
| 6 | Nonzero value set before capture, no getter in the frame | Reject unknown initial LOD at the first affected draw |
| 7 | Pre-frame value, first getter after sampling | Restore the saved initial value only across an understood record prefix |
| 8 | Initial getter at zero, set to one, getter at one, reset after rendering | Green normally; red when the nonzero setter is disabled |
| 9 | Create texture in the frame, no setter/getter | Native creation establishes zero; red |
| 10 | Frame-time creation, setter to one, getter | Green normally; red with setter disabled |
| 11 | Bind SRV before setter | Binding alone does not require known LOD; subsequent sampling is green |

Both native and GPA-injected producers pass twelve image checks per mode. The
two producer sets together contain 288 verified frames. Modes 6 and 9 deliberately
do not call GetResourceMinLOD; their JSON observation value of -1 denotes an
absent measurement, not a captured or native LOD value.

## State provenance

`ResourceLod.cpp` validates the full 28-byte payload, link, immediate context,
texture identity, resource-clamp creation flag and finite in-range value. Invalid
references and unsupported linked layouts fail explicitly.

For a resource that predates the frame, a first LOD record which is a getter
proves its initial value only if there is no earlier saved setter or opaque API
record. This inference relies on the supported immediate stream and the captured
resource identity; it does not resolve deferred lists or other interface layouts.
Malformed LOD records also block inference across that prefix. Frame-time
creation is handled separately: successful native creation supplies the default
zero, not a later snapshot or future getter. Preflight records each recovered
initial value and the getter event that supplied it.

Replay installs a proved initial value when it materializes the texture. An
explicit setter subsequently changes that value at its own event. ClearState
does not reset resource LOD. Each replay run recreates its resources and state,
so a previous run cannot contaminate the next. A getter following a setter is
metadata; disabling that setter does not cause the getter to reapply it.

If no initial value is known, textures may still be materialized and bound before
their setter. Actual GPU accesses require a known value. Draw/dispatch checks
use native bound views, including experiment overrides; copy and other supported
resource operations check their resource operands. Conservative offline checks
use captured snapshots and operands. They do not claim GPU execution success.

The original mode-6 capture is a concrete counterexample to silently assuming
zero. The previous package completed it and produced wrong red pixels. The new
preflight identifies **event 22, resource 23**; runtime rejects with the same
event/resource instead of emitting a successful incorrect frame.

## Validation and interpretation

`ResourceLodTests` has 29 passing cases with no skips. It includes all
twelve original modes on hardware and WARP, repeated ordinary and observed
replays, direct native setter-state checks, disabled-setter controls, full mip
bytes at zero LOD, an opaque-prefix counterexample, every truncated record
prefix, trailing bytes, invalid links/references, NaN/Inf, negative and out-of-range
values. Malformed reconstructed copies are negative controls, not positive
original-capture acceptance evidence.

A separate malformed-resource counterexample proved that the first audit version
aborted preflight before the ordinary per-entry validators ran. The corrected
audit leaves that resource's error to the existing validator and continues
gathering other diagnostics. `malformedResourceKeepsDiagnostics` requires an
error, a completed scan and the full entry count. Its pre-fix failure is retained
in `artifacts/minlod-malformed-before.txt`; the final related CTest run passes
all 37 suites, including 29 LOD and 56 UI/Worker cases.

The first test run exposed a missing disabled-event check in the new dispatch
branch: modes 8 and 10 remained green when the setter was disabled. That failure
is retained in `artifacts/minlod-tests-first.txt`. The fixed check passes in
`artifacts/minlod-tests-second.txt`; added zero-LOD byte and opaque-prefix checks
pass in `artifacts/minlod-tests-third.txt`.

The packaged candidate replays eleven positive files twice with exact producer
image hashes, four disabled controls and eight full-resource boundary exports.
Original-player comparison completes 22 runs: three files agree with the
producer and eight retain the original mip-0 discrepancy. These are diagnostic
comparisons; device/configuration equivalence is not established. Neither a
successful original exit nor agreement with its red output replaces the producer
oracle. The missing-state file is registered separately as an explicit rejection
control, not counted as a positive replay.

The positive registry is [min-lod-corpus.json](min-lod-corpus.json); the negative
registry is [min-lod-missing-state-corpus.json](min-lod-missing-state-corpus.json).
Raw results are `artifacts/m2-minlod-comparison/` and
`artifacts/m2-minlod-missing-state/`. The final [acceptance baseline](resource-lod-baseline.json) pins 321 positive files /
642 independent runs, 320 stable files and the known-variable Helldivers file.
All 309 previously stable hashes are unchanged. The inventory also includes the
one explicit missing-state rejection. There are 353 diagnostic control runs and
208 resource exports: 204 strict byte goldens plus four retained Helldivers
before/after diagnostic exports. These four have their existing repeat/difference
policy, not invented golden hashes. All 37 related CTest suites and four separate
golden/negative frame checks pass. The package is
`out/FloraGPA-minlod-final-20261004/`; all five binaries match the final Release build.

## Remaining scope

Full texture-storage export at nonzero minimum LOD is explicitly rejected:
accessibility of clamped mip levels is not yet verified, and the inspector must
not label unspecified bytes as a complete resource. This also constrains input
texture experiments that depend on full readback. No temporary LOD reset is used
to invent inaccessible contents. All four mips at zero LOD are byte-checked.

Other interface wire versions, linked/deferred operations, tiled/shared resource
interactions, all texture dimensions/formats and noninteger/special sampling
combinations require their own evidence. The bound-view guard is conservative
for descriptors that remain bound but are not referenced by the active shader;
declaration-aware filtering needs a separate capture and regression. This is an
implementation boundary, distinct from proving that missing state affects an
actual output. M2 and M3–M6 remain incomplete.
