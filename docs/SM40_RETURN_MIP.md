# SM4.0 mip-count queries with early returns

Reviewed 2026-10-09. Producer revision `94aa5b4`; implementation `42b86c2`.
This M4 increment removes four demonstrated false missing-MinLOD rejections
for count-only shaders with early unconditional returns. Captured shader bytes
and unavailable initial resource state remain unchanged.

## Dependency proof

The [RESINFO contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/resinfo--sm4---asm-)
defines total mip count independently of the per-resource clamp. SM4.0 can still
write all four result lanes, so FloraGPA must prove that the dimension lanes
cannot be consumed before releasing the missing-MinLOD dependency.

The existing bounded control-flow proof now treats a checked unconditional
[RET](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ret--sm4---asm-)
as a path endpoint, including inside IF and LOOP blocks. Calls and labels remain
outside the grammar; this excludes subroutine returns that resume a caller.
Other IF successors and loop backedges are still followed. A return on one
branch cannot suppress a dimension read reachable on another branch, and a
dimension used to decide whether to return is still a dependency.

All instructions remain parsed, including those after an early return.
Truncated/oversized RET forms, reserved control bits, unbalanced blocks and a
missing final RET fail the proof. The 256-instruction and 64-block limits remain.
Unverified operands, calls, labels, conditional RETC and executable SWITCH
fallthrough remain conservative. Direct SWITCH cases still need their checked
unconditional BREAK shape. This is a dependency proof, not a general shader
validator or proof that the GPU program terminates.

## Untouched originals and controls

Eight files were generated with local GPA 2025 R1 and never rewritten. Each
mode runs twelve frames on native hardware, WARP and with the original shim:
24 producer runs and 288 full-image checks. Saved compiler disassembly confirms
actual early RET instructions; nested and loop forms are checked explicitly.

| Mode | Workload | Previous replay | Required replay |
|---|---|---|---|
| 36 | Count-only early return | Missing-LOD rejection | Exact application image |
| 37 | Same, reflection stripped | Missing-LOD rejection | Exact application image |
| 38 | Dimensions consumed before return | Located rejection | Same rejection |
| 39 | Dimension controls return | Located rejection | Same rejection |
| 40 | Sampling with omitted initial LOD | Located rejection | Same rejection |
| 41 | Sampling with saved LOD setter | Exact application image | Unchanged image |
| 42 | Nested count-only returns | Missing-LOD rejection | Exact application image |
| 43 | Count-only return inside loop | Missing-LOD rejection | Exact application image |

The old package is checked twice per capture on hardware and WARP. The seven
refused modes fail at `Event 23 (Draw): Resource 24: initial minimum LOD is
unresolved at this access`. Mode 41 matches the application on both backends.
The extended producer additionally repeats modes 0, 6, 12, 20, 27, 28 and 35 on
hardware/WARP: 168 images pass and their image, DXBC and disassembly bytes match
the earlier producer artifacts.

The original private-player adapter completes two runs of every new capture.
It matches the independent count-only application images. Its images for modes
40 and 41 differ from their application outputs, consistent with the previously
recorded MinLOD discrepancy. The original player's adapter/GUI configuration
is not positively matched; these results do not establish universal GPA GUI
behavior or strict matched-device equivalence.

## Focused verification

The complete Resource LOD Qt suite passes **181 rows without failures/skips**,
including all eight new captures on hardware and WARP, repeated full replays,
and exchanges of count-only/sampling shader inputs to verify dependency
recomputation. The earlier focused 106-row run also passes.

New CPU graph controls cover dead reads after termination, reads before return,
surviving false edges, both branches returning, loop return versus a live
backedge, dimension-controlled returns, rejected calls/RETC, malformed RET
encodings, unmatched blocks and every truncated prefix of the compiler DXBC.
Three prior tests now correctly classify legal early/unreachable returns as
safe; dimension-use and malformed counterparts retain their original rejection.
Synthetic instruction graphs are explicitly test controls, not original captures.

The rebuilt preflight CTest passes. Native Windows Qt navigation passes eight
rows across existing and new mip-query families, alternating good captures with
located failures, retrying and checking every accepted pixel. Two packaged
GF2/BF1 goldens and two suppressed-Draw controls pass with system-only PATH.

## Full matrix and registry correction

All 43 GPU sub-suites satisfy their individual criteria: **629 registrations /
619 unique captures**, **587 completions / 577 unique completions** and **42
located refusals**. The retained evidence includes 1,258 ordinary attempts,
828 controls and 1,448 resource exports. Compared with the preceding Map baseline,
all 621 previous complete preflight reports, execution counts, refusal messages
and deterministic images remain identical. The registered Helldivers variation
is assessed separately. An additional audit rehashes 3,522 ordinary artifacts.

The first wrapper run nevertheless exits with `Corpus totals changed`: the
registry still declared the preceding 621/611/39 totals. Its failed `gate.json`
is retained unchanged. Revision `5382cb5` corrects those three declarations and
checks manifest hashes, unique capture counts and rejection IDs before launching
GPU work; five new CPU tests pass. Separate `registry-recheck.json` revalidates
every retained case using the unchanged per-case rules, original report hashes,
capture hashes and executable identity. No GPU attempt was repeated or replaced
to obtain this result, and no threshold or case policy was relaxed. A first
additional artifact audit also failed when reading progress-prefixed error logs;
its retained failure and corrected parser run are pinned separately.

Completion is distinct from application fidelity: the matrix records 155 passed
fidelity assessments, 32 capture-side mismatches, 19 information-missing cases
and 423 unassessed cases. Thirty-five completed registrations retain known
capture limitations. Application-image comparisons separately record 109
matches, three differences, 16 unavailable references and 501 unassessed cases.

## Archived-source package

A fresh 1,225-file Git archive of implementation `42b86c2` configures, builds
and packages successfully into 44 runtime files. Its separately compiled tests
and relocated package, under a path containing spaces and Chinese characters,
pass 19 Resource LOD rows, three native Windows UI rows and four strict game
golden/control checks with system-only PATH. The UI screenshot for mode 41 was
visually inspected and displays the expected yellow image. Package hashes remain
unchanged after these checks.

That archive precedes the development-only registry correction. All 634 current
production/test source files and both CMake build configuration files match the
archive after line-ending normalization. This does not claim the archive contains
the later validation-tool fix. The full matrix uses the incremental candidate;
the source-built package has the separate focused acceptance above.

## Reproduction and scope

```powershell
cmake --build --preset release --parallel 4 --target FloraMipCountProbe FloraResourceLodTests
python tools/capture_mip_counts.py --producer build/vs2022/Release/FloraMipCountProbe.exe `
  --out artifacts/new-return-originals --modes 36 37 38 39 40 41 42 43
$env:FLORA_RETURN_MIP_CAPTURES = "$PWD/artifacts/new-return-originals"
# Run only new original rows explicitly, or supply the other fixture variables
# for the full resource_lod CTest. Missing corpora are skips, not acceptance.
```

The [discovery registry](return-mip-discovery-corpus.json) preserves pre-fix
evidence; the [compatibility registry](return-mip-corpus.json) records five
accepted and three precisely rejected cases. The three missing-state cases do
not become recoverable because another shader's return paths are safe.

Candidate package: `out/FloraGPA-return-mip-20261009/`. The
[fixed evidence index](return-mip-baseline.json) pins package identities, original
captures, reports and the retained failures. Independent clean-Windows deployment
and a renewed sustained run remain open; same-host relocation does not establish
either. The earlier GUI GDI observations remain unresolved. Capture files,
original DLLs and generated evidence remain outside Git. Runtime remains C++/Qt
without Python or GPA. M3/M4/M5 and the 72/117/15 migration ledger remain incomplete.
