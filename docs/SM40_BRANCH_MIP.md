# SM4.0 mip-count queries across structured branches

Reviewed 2026-10-08. This M4 correction extends the
[straight-line dimension-lane proof](SM40_MIP_DIMENSIONS.md). An untouched
original capture can contain full-vector `RESINFO`, consume only its mip count,
and branch on that count. The previous grammar rejected the branch and therefore
required an initial resource MinLOD that this workload never consumes.

## Proof and limits

The [RESINFO contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/resinfo--sm4---asm-)
keeps the total mip count independent of resource MinLOD. Dimensions retain
their separate undefined-result boundary. The proof now builds forward edges
for checked [IF](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/if--sm4---asm-),
ELSE and ENDIF instructions, including nested branches and an optional ELSE.
At a join, any queried dimension lane surviving either path remains live.
Sources are checked before destination overwrites, including the branch
condition. Both outcomes are visited; no observed image or presumed register
value is used to decide reachability.

The accepted grammar retains its limit of 256 instructions and adds at most 64
nested IF blocks plus unsigned less-than comparison. It requires balanced
blocks, a single final unconditional return, checked register/component operands
and complete instruction lengths. Calls, loops, relative/extended operands,
unfamiliar instructions and unproved dependencies stay conservative. Source
swizzles are still read in full. This is not a general shader optimizer.

The same result is used by preflight and production shader-identity caches,
including replacement shaders. Capture bytes, executable shaders, events,
resource values and comparison thresholds are not rewritten. Live dimensions
and sampling the same clamped texture still require known resource LOD.

## Original evidence

The development producer adds modes 12..19. Eight untouched GPA 2025 R1 files
are pinned in the [corpus](branch-mip-corpus.json). Each mode passes twelve
full-image checks on native hardware, native WARP and the injected hardware
producer: 24 runs and 288 images in the final batch. Producer source/binary,
original DLL hashes and device descriptions are retained in its manifest.

| Mode | Actual workload | Before | Corrected result |
|---|---|---|---|
| 12 | Count with IF/ELSE | Missing-LOD rejection | Exact application image |
| 13 | Same, reflection stripped | Missing-LOD rejection | Exact application image |
| 14 | Dimensions used in then branch | Missing-LOD rejection | Same located rejection |
| 15 | Dimensions used in else branch | Missing-LOD rejection | Same located rejection |
| 16 | Same-texture sampling in branch | Missing-LOD rejection | Same located rejection |
| 17 | Branch sampling with captured LOD setter | Exact application image | Unchanged exact image |
| 18 | Nested count branches | Missing-LOD rejection | Exact application image |
| 19 | HLSL without ELSE, compiled into IF/ELSE | Missing-LOD rejection | Exact application image |

The first discovery batch is preserved. Its initially written nested HLSL was
simplified by the compiler into one branch. The final producer uses independent
comparisons and its saved disassembly confirms two actual nested IF blocks.
Mode 19 is explicitly not presented as original evidence for an omitted ELSE;
checked program mutations cover that grammar boundary separately.

Before/after comparisons each preserve sixteen native attempts and sixteen
original private-player attempts. All 32 primary original runs complete with
unchanged per-mode images. The four newly accepted modes match the application
and local original player. Mode 17 remains exact against the application while
the original player differs, as before. Original GUI/adapter configuration is
unverified; no universal cross-implementation equivalence is asserted. Modes
14..16 still reject at Draw 23, resource 24, with unchanged complete preflight
reports and located runtime errors.

One initial after-run mistakenly used the build-tree CLI with isolated system
PATH. It exited with Windows status `0xc0000135` before preflight/replay (missing
runtime dependency); this run is retained and excluded from acceptance. The
correct comparison uses the complete release package and a fresh output folder.

## Validation

The full resource-LOD suite passes 106 rows without skips. It exercises all
twenty original mip-query modes on hardware/WARP with repeated replay, actual
replacement-shader dependencies and existing LOD cases. New parser controls
check every truncated branch DXBC prefix and eighteen invalid/dependent variants,
including live branch conditions, only-one-arm overwrites, live values at joins,
unmatched/duplicate branch markers, malformed operands, early return and excessive
nesting. Opposite branch-test polarity and a safely overwritten optional branch
are separate positive structural controls, not original captures.

The extended Qt workflow interleaves all eight new originals, retries after
located failures, checks every accepted pixel and repeats accepted replays.
Five related CTest suites pass in 188.50 seconds, including 69 main-UI and 70
pipeline-creation rows. Producer: `22311a2`; implementation: `b8ab3dc`.
Complete regression/package results and local evidence hashes are recorded in
the [acceptance baseline](branch-mip-baseline.json).

The complete serial gate passes 36 suites and 525 registrations / 515 unique
captures: 492 native completions (482 unique) and 33 located rejections. It
retains 1,050 ordinary attempts, 684 control runs and 826 resource exports. All
517 preceding registrations keep their complete preflight reports, execution
counts and deterministic images. Only the existing Helldivers variable-image
policy observes changed pixels; no policy or threshold was relaxed.

Completion remains separate from fidelity: 60 registrations have a passed
application assessment, 32 retain capture-side mismatches, ten lack required
capture information and 423 remain unassessed. Thirty-five completed cases have
known capture limitations. Gate success does not certify all completed captures
as faithful reproductions of their applications.

The 44-file package is `out/FloraGPA-branch-mip-20261008/`. GUI, CLI and Worker
change from the preceding payload package; the other files are identical. A
relocated Chinese/space-path, system-PATH Qt run passes all three mip-query
families (five rows including setup/cleanup, no skips). Reviewed mode-18 and
mode-17 screenshots show the expected red and yellow outputs after failure/retry.
The offscreen Qt font-directory warning is preserved. Four packaged GF2/BF1
golden and disabled-Draw checks keep their hashes and counts, with no Python or
original GPA replay modules observed. The baseline pins 1,365 evidence files.
This is same-host acceptance, not a fresh clean-source build, independent host
deployment or renewed sustained soak.

## Remaining work

Other control flow and shader operand families remain conservative. This proof
does not supply absent initial LOD, mapped texture pitches/data, pre-frame
query/counter history or retained command-list identity. M3/M4/M5, broader
stability and independent clean-machine acceptance remain open. Module migration
counts stay 72 ported / 117 partial / 15 pending.
