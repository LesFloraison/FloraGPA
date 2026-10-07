# Unused SM4.0 RESINFO dimension results

Reviewed 2026-10-08. This M4 correction follows the
[mip-count-only dependency proof](MIP_COUNT_LOD_COMPATIBILITY.md). SM4.0 compilation
can emit `resinfo r0.xyzw` even when HLSL only consumes the total mip count.
The prior audit conservatively treated the written dimension lanes as a MinLOD
dependency and rejected three newly captured, valid workloads.

## Semantics and implementation

Microsoft's [RESINFO contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/resinfo--sm4---asm-)
keeps total mip count independent of the resource clamp. Dimensions of clamped
mips can be undefined. Merely observing a count in the application's output is
insufficient: the dependency proof must account for every written dimension lane.

The checked decoder now has an additional bounded SM4.0 straight-line proof.
It parses complete operand boundaries for understood register/literal forms and
follows the queried temporary's dimension lanes until they are overwritten or
die at the final unconditional return. Sources are checked before destination
writes, including when an instruction uses the same temporary for both. Source
swizzles are conservatively read in full; masked-off source components do not
receive an additional exemption.

The accepted grammar contains resource/sampler/output/temp/global declarations,
MOV, UTOF, ADD, AND, IEQ, MUL, LD, RESINFO and SAMPLE, ending in one RET. It accepts
at most 256 instructions. Unknown instructions, branches/loops/calls, relative
or extended operands, instruction extensions, unfamiliar index forms and dynamic
linkage retain the previous dependency. This is not general liveness, reachability
or shader optimization. Other SM versions retain the earlier direct count-only
rule. Sampling the queried texture still independently requires known MinLOD.

Both offline preflight and runtime use the same dependency result. The runtime
cache still follows the actual native shader identity, including replacements.
No instruction, shader, capture byte, resource value or saved LOD is rewritten.

## Original captures and controls

The extended development producer creates six untouched GPA 2025 R1 captures,
using SM4.0 equivalents of its previous SM5 workloads. Each mode passes twelve
complete images on uninjected hardware, uninjected WARP and the injected
hardware producer: 18 runs and 216 checked frames. The producer's old SM5 modes
0 and 5 additionally pass six runs / 72 images with the updated harness.

| Mode | Workload | Previous replay | Required corrected result |
|---|---|---|---|
| 6 | Full-vector RESINFO, count only | Missing-LOD rejection | Exact producer image |
| 7 | Same, reflection stripped | Missing-LOD rejection | Exact producer image |
| 8 | Width/height and count | Missing-LOD rejection | Same located rejection |
| 9 | Count plus ordinary second-texture sampling | Missing-LOD rejection | Exact producer image |
| 10 | Count plus sampling the clamped texture | Missing-LOD rejection | Same located rejection |
| 11 | Same, explicit in-frame LOD setter | Exact producer image | Same exact image |

The first discovery attempt completed all mode-6 producer image checks but saved
no capture file. It is retained separately, not counted as an original. The
harness now invokes the already-pinned original primary-swap-chain selector and
requires the frame file to exist. The selector validates the shim hash and code
entry point; this development-only code is not linked into the application. All
six final captures use that original selector and remain unmodified.

The before/after comparison preserves twelve independent attempts and twelve
original private-player runs for each package. Before the correction, native
replay rejects modes 6..10 and completes mode 11. Afterwards, modes 6, 7 and 9
complete with exact application images and observed original-player equality.
Modes 8 and 10 retain identical preflight reports and runtime rejection at Draw
event 23, resource 24. Mode 11's native image is unchanged and matches the
application; its original-player image still differs. All 24 original-player
runs complete with the same per-mode images across the two batches. The original
adapter and GUI configuration are not verified, so the independent application
image is the correctness oracle, not a universal equivalence assertion.

## Validation scope

The focused suite passes 40 rows: compiled SM4.0/4.1/5.0 shaders with and without
reflection, twelve original modes on hardware/WARP with repeats, replacement
shaders and parser boundaries. New proof controls cover every truncated SM4.0
container prefix, eighteen altered program/operand cases and dynamic linkage.
They include read-before-overwrite, partial overwrites leaving a live dimension,
missing/multiple returns, loop presence, bad indices and lengths. Edited programs
are negative controls, not positive original-capture evidence.

Five related CTest suites pass in one invocation (187.01 seconds). Preserved
QtTest rows are 89 resource LOD, 70 pipeline creation, 68 GUI, 173 preflight and
five shader setters, without failures or skips. The latter two were also run
explicitly to retain their row output, absent from this CTest log. GUI tests
open both SM5 and SM4.0 families, check complete 8x8 images, reject missing-input
files and retry through valid captures.

The [new corpus](sm40-mip-corpus.json) records the six originals separately.
The two missing-input cases retain explicit event/resource rejection requirements.
The complete serial gate passes 35 suites: **517 registrations / 507 unique
captures**, with **487 completions (477 unique)** and **30 located rejections**.
It includes 1,034 ordinary attempts, 684 controls and 826 strict resource exports.
All 511 preceding cases retain complete preflight reports, execution counts and
deterministic images. Known-variable changes remain confined to Helldivers and
`query_sync_9`; the latter alternates correct/wrong images because the original
file omits the Query/End boundary, as documented in [Query completion](QUERY_COMPLETION.md).
It is not accepted as faithful replay or classified as application nondeterminism.

Fidelity assessments remain separate: 55 passed, 32 capture-side mismatches,
seven information-missing and 423 unassessed registrations. Thirty-five completed
cases have known capture limitations. Passing the gate includes correct refusals
and does not certify all 487 completed cases as matching their applications.

The initial aggregate run completed all 511 preceding registrations, then rejected
the new manifest's raw CRLF hash: the registry contract requires LF normalization.
Its reports and initial manifest bytes are preserved. The registry was corrected
and normalized before a fresh aggregate run; neither replay binaries nor captures
changed. This configuration failure is not counted as a passing full gate.

Implementation: `559db57`; development producer: `02ecf8e`. The build retains
existing deprecated Qt assertion-macro warnings; it is not warning-free.

The 44-file package is `out/FloraGPA-sm40-mip-20261008/`. Only GUI, CLI and Worker
binaries differ from the preceding buffer-acceptance package. Relocated
Chinese/space-path, system-PATH Qt tests pass four rows with no skips, exercising
both mip families and failure/retry. Offscreen Qt's font-directory warning is
preserved; reviewed mode-9 and mode-11 screenshots show the expected yellow
output. Four packaged GF2/BF1 golden and disabled-Draw checks preserve their full
hashes and counts. Loaded modules contain no Python or original GPA runtime.

The [acceptance baseline](sm40-mip-baseline.json) pins package files and local
evidence, including both aggregate runs, before/after original comparisons,
producer sources/binary, malformed-input tests, Qt results and raw images.
Local captures and generated evidence remain outside Git. This package has not
had a new full clean-source build or sustained soak; earlier batches retain
their own narrower acceptance records.

## Remaining work

Other shader control/data-flow forms remain conservative. This does not supply
missing initial LOD, mip contents, texture differential pitches, counter/query
history or retained-list identity. M3/M4/M5 and independent clean-machine
acceptance remain incomplete; the Python migration ledger is unchanged.
