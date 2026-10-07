# Mip-count metadata without an initial resource MinLOD

Reviewed 2026-10-08. This M4 correction removes three demonstrated false
rejections. It neither reconstructs omitted MinLOD state nor changes a shader,
event, texture byte or global image-comparison threshold.

## Failure and proof

The previous dependency audit treated every declared SRV as consuming resource
MinLOD. An untouched original capture whose shader only asks for the view's
total mip count was consequently rejected at Draw 23, resource 24, even though
the descriptor and mip count were saved. Reflection stripping did not help.

Microsoft's [RESINFO specification](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/resinfo--sm4---asm-)
distinguishes the total count in `.w` from dimensions in `.xyz`: the total count
is unaffected by MinLOD, while dimensions for clamped mips have undefined
results. This is narrower than assuming all `GetDimensions` calls are safe.

`shaderSrvLodDependencies` now examines checked SM4/5 program instructions and
immediate SRV operands. A declared slot is independent only if it has a verified
RESINFO count-only witness and no other resource use. Every destination lane
must select `.w`. A count query mixed with sampling another ordinary texture is
allowed; sampling the same clamped texture retains its LOD requirement. The
offline audit and production shader-identity cache use the same proof. Actual
replacement shader bytecode determines runtime dependencies. Ordinary SRV
declaration/binding validation is unchanged.

Unknown versions/opcodes/extensions, dynamic linkage, relative/extended operands
and dimensions remain conservative. A word resembling a resource operand can
retain a dependency even when it is a literal. This deliberate false-positive
boundary is not a general data-flow or reachability analysis.

The SM4.0 compiler used locally emits full-vector `resinfo ...xyzw` even when
HLSL only uses the count. Such programs still require LOD: proving unused
dimension results dead needs separate analysis. SM4.1/5.0 produce the verified
single-component form. The initial test assumed identical lowering and failed
two SM4.0 rows; disassembly established the actual boundary, now asserted by
the regression. No runtime guard was weakened for those rows.

## Original captures and independent application oracle

The development-only `FloraMipCountProbe` creates a four-mip, 8×8 texture, sets
resource MinLOD before captured frames, and checks every output pixel. Six
untouched GPA 2025 R1 captures are registered in
[the corpus](mip-count-corpus.json); capture files remain outside Git.

| Mode | Shader/capture | Previous native result | Corrected result |
|---|---|---|---|
| 0 | Count only | Missing-LOD rejection | Exact application image |
| 1 | Count only, reflection stripped | Missing-LOD rejection | Exact application image |
| 2 | Dimensions and count | Missing-LOD rejection | Same located rejection |
| 3 | Count plus sampling an ordinary second texture | Missing-LOD rejection | Exact application image |
| 4 | Count plus sampling the clamped texture | Missing-LOD rejection | Same located rejection |
| 5 | Same sampling, explicit captured LOD setter | Exact application image | Exact application image |

Hardware, WARP and original-shim-injected producers each check 12 frames per
mode: 216 full-image checks across 18 runs. Device identities, producer sources,
binary and original DLL hashes are frozen in the batch manifest. The first
producer attempt used a fractional dimension color and exposed 127-versus-128
conversion rounding; the final fixture uses integer boolean comparisons for
the dimension check. That failed batch is preserved, and all six modes were
recaptured with the corrected oracle.

Before/after comparisons each run 12 independent attempts and 12 original
private-player attempts. Native expected failures are retained, not counted as
successful replays. The three newly allowed modes agree byte-for-byte with the
application and local original player. Mode 5 agrees with the application but
differs from the local original player, as before. The original GUI and its
selected adapter were not verified; this is an observation, not universal
cross-implementation equivalence. Modes 2/4 do not receive fabricated LOD values
to match any player output.

## Verification and release evidence

Focused tests pass 27 rows without skips: SM4.0/4.1/5.0 with and without
reflection, same-slot/other-slot accesses, six originals on hardware and WARP,
repeat replay, replacement shaders, every truncated container prefix and nine
operand/version/extension counterexamples. The Qt workflow opens all six modes,
retries after each missing-state failure, checks every accepted pixel and repeats
successful replays. It adds no new UI surface.

The complete serial GPU gate passes 34 suites: 511 registrations / 501 unique
files, 483 native completions (473 unique) and 28 located rejections. It retains
1,022 ordinary attempts, 684 controls and 826 strict resource exports. All 505
previous cases have identical complete preflight reports, execution counts and
deterministic images. Helldivers' diagnosed image variation remains recorded.
Thirty-five completed cases retain known capture limitations. Capture-fidelity
assessments are separately 51 passes, 32 capture-side mismatches, five
information-missing cases and 423 unassessed registrations.

Five relevant CTest suites pass: 76 resource-LOD, 70 pipeline-creation, five
shader-setter, 173 preflight and 67 Qt rows, with no final failures or skips.
Two stdout-only suites were additionally saved to explicit text files to retain
their individual rows. Four packaged GF2/BF1 golden and disabled-Draw checks
pass. Relocating the package to a Chinese/space path with only system PATH also
passes the six-mode Qt open/failure/retry workflow (three Qt rows, no skips).
Its offscreen font-directory warning is retained; the inspected screenshot has
readable controls and the exact accepted yellow output. This is same-host
portability, not independent clean-machine acceptance.

Producer: `f202db5`; runtime: `d340c8d`; Qt regression: `30e8ec3`. The 44-file
package is `out/FloraGPA-mip-count-20261008/`; only GUI, CLI and Worker binaries
change from the preceding asynchronous-image package. The
[baseline](mip-count-lod-baseline.json) pins deployed binaries, original-capture
manifests, full matrix reports, before/after evidence, screenshots and initial
failed attempts. Runtime binaries load neither Python nor original GPA DLLs.

## Remaining scope

This proves only the narrow count-only dependency exemption. General shader
data-flow, missing mip/Map contents and pitches, omitted counter/query history,
retained command lists, long-duration reliability and independent clean-machine
acceptance remain open. M3/M4/M5 are incomplete. The 72/117/15 Python-module
ledger does not measure GPA feature completeness.
