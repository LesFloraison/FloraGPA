# Capture fidelity is separate from replay completion

Reviewed 2026-10-06. This M4 acceptance correction changes development reports,
not C++ replay behavior. A stable replay and equality with the original player
do not establish reconstruction of the original application workload.

## Observed reporting gap

The corpus already labels 32 merged-list captures `capture_side_mismatch` and
32 `passed`. The prior serial report dropped those labels. Separately, both
active MSAA files were classified as ordinary repeat-stable observations even
though historical original/producer evidence establishes missing frame-before
samples in the retained case. The technical limitations were documented, but
the machine-readable result did not preserve this distinction.

The MSAA capture hashes and source hashes were rechecked against the original
manifest. Both files contain a four-sample texture with just 16,384 bytes of
ordinary GenData, all zero. The saved data cannot represent the producer's four
distinct sample planes. Fresh paired runs complete twice in both implementations
and agree byte-for-byte. The initialized file matches the independent application
reference; the retained file differs. The original replay adapter remains
unidentified, so no matched-device equivalence is claimed.

## Report semantics

Each case now reports two independent dimensions:

- `capture_assessment` propagates the corpus's documented workload assessment:
  `passed`, `capture_side_mismatch`, `capture_information_missing` or `unassessed`.
  Its source is explicitly the corpus manifest; it is not inferred from pixels.
- `application_image_comparison` compares every completed replay against an
  explicitly supplied independent application image hash. It is `matches`,
  `differs`, `unavailable`, `not_run` or `not_assessed`. The reference requires a
  scope and a hash-bound evidence file within the capture root.

Existing saved-capture goldens, runtime outcomes, resource boundaries and
original-player comparisons retain their separate meanings. Neither dimension
sets `full_workload_equivalence_proven` true. Missing annotations are unassessed,
not implicitly successful. Coincidentally matching pixels do not erase a known
capture-information limitation.

An unexpected independent-application image mismatch fails the development batch.
A mismatch already attributed to capture-side divergence or missing information
remains an explicit warning and limitation; runtime failures and existing strict
golden/resource failures still fail. The tool does not change shaders, insert
resource values, disable events or relax a global image threshold.

The gate verifies that each per-case fidelity report agrees with its manifest
and observed runs. It aggregates capture assessments, independent-image results
and `completed_with_known_capture_limitations` separately from `positive_cases`.
The latter remains a replay-completion counter and must not be described as full
application-fidelity coverage.

## Scope and evidence

The initialized MSAA sample now has a strict producer-image golden. The retained
sample records missing per-sample input and the independent producer reference
without pinning undefined replay bytes as a cross-device expected image. The
32 deferred merge distinctions come from the existing
[merge audit](DEFERRED_MERGE_AUDIT.md); this does not accept traditional command
lists or reconstruct overwritten capture-side resource versions.

The legacy research evidence remains at
`D:/CDXrepo/FloraGPA/analysis/ACTIVE_MSAA.md` and its linked, hash-bound capture
manifest. Original captures and GPA dependencies remain outside Git. Unassessed
cases may have other unit/producer evidence elsewhere; the absence of a linked
independent-image reference is not itself proof of a replay defect.

## Acceptance on 2026-10-06

Implementation: `3fa31f2`. The 29-suite continuous serial run passes all expected
outcomes: 489 registrations / 479 unique files, 463 native completions and 26
located rejections. It includes 978 ordinary attempts, 674 control runs and 692
resource exports. All saved cases were rechecked with the final gate implementation.
Previous diagnostic findings and deterministic image hashes are unchanged;
Helldivers keeps its observed, separately classified variation.

| Manifest capture assessment | Registrations |
|---|---:|
| Unassessed | 423 |
| Passed within its documented scope | 33 |
| Known capture-side mismatch | 32 |
| Known missing capture information | 1 |

The 463 completions include all 33 known capture limitations. The independent
application-image field currently covers only the two MSAA cases: one matches,
one differs as expected from missing data; 487 are not assessed on this axis.
These counts are not a feature-completion percentage.

Fresh paired MSAA validation completes two native and two original runs per file.
Both implementations agree on each file; only the initialized file matches the
application oracle. A newly compiled, uninjected native producer passes 24 frames
across both modes, including exact independent CPU checks of the resolved image
and all four sample-plane exports. No GPA capture was modified or regenerated.
The 25 CPU regression tests cover missing/invalid evidence, all repeats, known
limitations, unexpected differences and mixed positive/negative gate behavior.
No C++ execution changes required a new package or CTest run in this batch.

See the [hash-bound acceptance baseline](capture-fidelity-baseline.json).
Raw local evidence is excluded from Git. Historical reports lacking these fields
remain historical evidence; the updated gate deliberately requires the fields.

## Reporting boundary at this baseline

The following gap is now addressed by the subsequent
[MSAA initialization notice change](MSAA_INITIALIZATION_NOTICES.md).

Preflight already locates ordinary MSAA GenData limitations by resource ID.
Texture inspection also exposes `msaa_initial_data_not_applied`. Ordinary replay
tracks the affected IDs internally, but its final-image report does not propagate
them when Resolve produces a single-sample output. Connecting this existing
provenance to the final report and compact GUI diagnostics was next work at this baseline.
Such a warning must not imply that an in-frame fully initialized image is wrong,
or claim that every materialized resource necessarily affects the final image.
