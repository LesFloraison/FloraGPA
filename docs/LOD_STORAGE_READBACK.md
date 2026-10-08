# Full texture storage at nonzero resource MinLOD

Reviewed 2026-10-08. Implementation `22fdac3`; producer `60c9371`.
The [acceptance baseline](lod-storage-baseline.json) pins 1,906 evidence files.
This completes this correction batch, not M3/M4/M5.

> Historical batch: [input clone LOD inheritance](LOD_CLONE_INHERITANCE.md) now
> supersedes the input-experiment refusal described below. This baseline is unchanged.

## Corrected boundary

Full texture inspection/export previously rejected every nonzero resource
minimum LOD. That was an implementation restriction, not evidence of missing
capture bytes. The [D3D11 functional specification, section 5.8.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
distinguishes shader SRV accesses from resource-copy operations: the latter are
unaffected by the resource clamp. Section 5.8.4 also defines fractional clamping.
The [Microsoft Learn method overview](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setresourceminlod) uses broader wording; this correction relies
on the functional specification and independent native byte checks.

`TextureReadback.cpp` now copies all stored mip levels to staging without
changing the resource's MinLOD. The checked explicit-subresource, non-MSAA,
layout, size and mapped-pitch requirements remain. Unknown initial LOD remains
an explicit boundary in this batch. This does not invent missing captured data
or broaden shared/tiled resource support.

## Evidence and counterexamples

The development-only producer `tools/native/min_lod_storage_probe.cpp`, committed
as `60c9371`, creates twelve untouched GPA 2025 R1 captures. The independent
application constructs CPU image and byte oracles before capture. Each case runs
on hardware, WARP and injected hardware, twelve frames each: 432 image checks,
432 complete packed-storage checks and 432 preserved-LOD observations.

| Modes | Resource | MinLOD values |
|---|---|---|
| 0–3 | 1D array, two layers | 0, 0.75, 1, 4 |
| 4–7 | 2D array, two layers | 0, 0.75, 1, 4 |
| 8–11 | 3D texture | 0, 0.75, 1, 4 |

All textures use RGBA8 and four mip levels; the value 4 excludes the complete
SRV from sampling. Raw storage still contains every initialized mip. Sampling
returns red at zero, green at 0.75/1, and zero RGBA at 4. No shader is changed
to obtain these results. Broader filters, formats and clamp combinations need
their own sampling evidence.

The prior catalog package completes all twelve ordinary replays. It accepts
12 zero-LOD boundary exports and refuses 36 nonzero boundary exports. These
failures are retained under `artifacts/m4-lod-storage-before/`. A separate CLI
control on the older `minlod_1` capture records the exact former error in
`artifacts/m4-lod-storage-old-cli.log`. The first Qt negative control ends on an
unhandled expected exception; its crash/test log is also retained, not counted
as a successful test.

The new tests inspect full storage before and after Draw on hardware and WARP,
check native MinLOD and SRV bindings around inspection, and compare complete
rendered images. All eleven older positive MinLOD captures now have strict
full-storage checks; seven nonzero cases are newly accepted. The missing-state
capture continues to reject. Existing malformed LOD
records and initial-data tests cover invalid references, truncation and lengths.

### Input experiments retain their boundary

The input-experiment clone currently drops RESOURCE_CLAMP and does not inherit
resource MinLOD. Simply deleting the storage guard would therefore enable an
incorrect new path. The preserved draft package proves this: an identity patch
of mip 0 changes mode 6 from green to red. The preceding package rejects that
experiment. Evidence is `artifacts/m4-lod-storage-clone-control/`.

The old protection is retained in `TextureEdits.cpp`, before the clone can be
published. Its specific error is now:

> Input texture experiments with nonzero resource minimum LOD are unverified

All twelve original modes test this boundary on both devices: zero LOD remains
accepted, nonzero LOD refuses, and an ordinary replay/storage inspection still
succeeds afterward. Input-clone LOD inheritance is a concrete follow-up item;
this batch does not claim to restore it.

## Original-player interpretation

The original private-ABI player completes the twelve captures repeatedly. Its
three zero-LOD cases agree with the independent application; its nine nonzero
cases retain the earlier mip-0 discrepancy. Device/configuration equivalence is
unverified. These are diagnostic comparisons, not strict cross-player acceptance
or a claim about the complete GPA GUI workflow. Independent native hardware and
WARP images and full resource bytes remain the positive oracles.

## Acceptance results

The final package is `out/FloraGPA-lod-storage-final-20261008/`. Its 44 files
change only the GUI, CLI and Worker executables relative to the preceding catalog
package; all other 41 files match. The earlier `out/FloraGPA-lod-storage-20261008/`
is a retained research draft with the demonstrated clone regression, **not the
accepted package**.

| Check | Result |
|---|---|
| Related CTest | 9 suites pass in 251.99 seconds |
| Logged Qt rows | 470 pass, no failures/skips; four stdout-only suites are additionally run with explicit result files |
| Resource LOD / main UI | 147 / 76 rows, including 24 new device/capture rows and 6 new UI scenarios |
| Registered matrix | 38 suites, 545 registrations / 535 distinct capture hashes |
| Execution outcomes | 509 completions / 499 distinct hashes; 36 located refusals |
| Matrix attempts | 1,090 ordinary attempts, 708 controls, 888 resource exports under their registered policies |
| Previous 533 registrations | Complete preflight reports, execution counts and deterministic images unchanged |
| New export boundaries | 48 strict new-capture exports; 14 additional strict exports for older nonzero-LOD cases |
| Original-player comparisons | 48 runs across before/after packages; original images unchanged |
| Relocated package | Windows Qt inspection/export/retry, four GF2/BF1 recovery cycles, and both shipping GUI startups pass |
| Separate goldens | GF2/BF1 and their disabled-Draw controls all pass |

The relocated run uses a Unicode path and system-only PATH. Native module audits
pass without GPA/Python replay dependencies. This is same-host acceptance, not
independent clean-host deployment, a fresh committed-source rebuild or a renewed
long-duration soak. Build and relocated recovery each check twelve strict game
images. The standalone clone counterexample now refuses at **event 25 (Draw)**;
the no-op patch is not allowed to silently alter sampling.

Helldivers retains its existing variable-image policy. `query_sync_9` happens to
repeat identically in this batch but differs from the prior run; its recorded
information-missing/variable policy is unchanged. No thresholds are relaxed.

Completion is separate from fidelity: the registry records 77 fidelity-passed,
32 capture-side mismatch, 13 capture-information-missing and 423 unassessed
registrations. Thirty-five completed cases retain known capture limitations.
Do not interpret 509 completions as 509 fully faithful captures.

Captures and generated artifacts remain outside Git. Module totals remain
72 ported / 117 partial / 15 pending; these are not an overall GPA completion
percentage. M3/M4/M5 remain incomplete.

## Follow-up scope

- Preserve resource-clamp creation/state semantics in input experiment clones,
  with independent sampling and lifetime checks before enabling them.
- Audit conservative LOD dependencies on non-SRV operations separately; this
  change does not remove unknown-state guards from ordinary copy/update paths.
- Review binding-tree caption synchronization after selecting a different entry
  in the resource table. The new UI test snapshots show resource 10 in the
  inspector while the tree/footer retain the prior output binding; byte/image
  assertions verify the requested resource, not that caption. No UI layout or
  caption fix is claimed here.
- Continue capture-data/pitch provenance, retained command-list identity,
  predicate/query boundaries and the existing M5 stability/deployment work.
