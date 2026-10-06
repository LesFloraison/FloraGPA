# Counter-independent UAV access

Reviewed 2026-10-06. This M4 correction permits ordinary indexed access to an
Append/Counter UAV when checked executable shader code proves that no hidden
counter operation can occur. It does not invent, initialize or recover the
unavailable hidden count. M3, M4 and M5 remain open.

## Original trigger and controls

Three unmodified GPA 2025 R1 captures are registered in
[the corpus](counter-usage-corpus.json). The development producer checks the
complete 64-byte buffer and 8-by-8 RGBA output against its CPU oracle on twelve
native and twelve GPA-injected frames per case (72 frames total).

| Mode | Operation | Expected behavior |
|---|---|---|
| 12 | Ordinary structured UAV, indexed store | Control: exact buffer and green image |
| 13 | Counter UAV, explicit count 2, IncrementCounter | Control: exact buffer, count 3 and green image |
| 18 | Counter UAV, KEEP, indexed store without counter operations | Exact buffer and green image; hidden count remains unavailable |

The preceding worker-recovery package rejects mode 18 at Dispatch; the original
player replays it twice successfully. With this correction all three captures
replay twice with exactly the producer's expected RGBA bytes, matching the
original player's repeated outputs. The original replay adapter has not been
identified: this is observed byte equality, not a matched-device equivalence
claim. The complete files and capture hashes are unchanged.

The original capture producer now uses the existing hash-pinned primary swap
chain selector. An earlier attempt without it produced valid producer oracles
but no capture file; that attempt is retained separately and excluded from
acceptance. Frozen producer sources and executable hashes are recorded in
`artifacts/m4-counter-usage-originals-final/manifest.json`. Subsequent source
comment or formatting changes do not replace that frozen evidence.

## Runtime and preflight semantics

`shaderMayUseHiddenCounters` scans checked DXBC SHDR/SHEX instruction streams.
Only understood SM4.0/4.1/5.0 streams without hidden-counter instructions may
prove absence. IMM_ATOMIC_ALLOC and IMM_ATOMIC_CONSUME require counter state;
ordinary InterlockedAdd accesses buffer words and is not a hidden-counter read.
These opcode identities follow Microsoft's
[tokenized program format](https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/D3D12TokenizedProgramFormat.hpp)
and [IMM_ATOMIC_ALLOC semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-alloc--sm5---asm-).

Missing code, empty programs, dynamic linkage, interface declarations/calls,
custom data, opcode extensions, reserved or unrecognized instructions and newer
shader versions retain the conservative requirement. Invalid lengths and
duplicate executable chunks cannot establish absence. Reflection data is not
required, so stripped shaders are covered.

Preflight examines the captured active stages. When an unknown counter is bound
but not consumed, it emits the informational `counter_value_not_consumed` with
event, view and storage IDs. Structural success is not GPU execution success.
Runtime examines the actual native shader bindings and class instances. Shader
creation caches the classification of the actual bytecode, including user
replacements; unknown native shader objects retain the conservative requirement.

This is a whole-active-pipeline proof. It does not prove individual slots unused
or evaluate shader branches. If any active stage may use a hidden counter, the
existing requirements remain for all relevant bound counter views. Geometry
variants or instrumented shaders without a classification also retain those
requirements. CopyStructureCount and direct counter inspection still require a
known value. Ordinary buffer writes never make an unknown count known. Explicit
counter experiments retain their existing provenance and are not automatic fixes.

## Verification evidence

The counter suite covers hardware/WARP, stripped/unstripped shaders, repeated
replay, a replacement that introduces IncrementCounter, CopyStructureCount,
explicit seeding, malformed code lengths, duplicate chunks and conservative
fallbacks. Six original-capture rows check the complete buffer before and after
Dispatch, repeated final buffer/image bytes, counter availability, and the
unchanged buffer when Dispatch is disabled. Registry controls additionally check
the exact magenta image from that diagnostic-only omission.

Before/after original-player evidence is retained in
`artifacts/m4-counter-usage-before/` and `artifacts/m4-counter-usage-after/`.
Build, unit, serial matrix and package evidence is pinned by the
[accepted baseline](counter-usage-baseline.json). Runtime: `c386d3a`; producer:
`96caae3`. Package: `out/FloraGPA-counter-usage-20261006/`.

| Acceptance check | Result |
|---|---|
| Registered matrix | 29 suites; 489 registrations / 479 unique files; 463 positives / 26 located rejections |
| Execution evidence | 978 ordinary attempts, 674 control runs, 692 resource exports |
| Prior 486 registrations | No diagnostic, outcome or deterministic image changes |
| Related CTest | 11 suites passed, GPU execution serial |
| Counter tests | 27 passed, no skips, including six original hardware/WARP rows |
| Qt interaction | 59 passed, no skips, including both optional original-capture workflows |
| Package with isolated PATH | GF2/BF1 goldens and draw-suppression controls: four passes |

The first matrix runner passed 28 suites and then stopped before launching the
last suite: the new manifest hash had used CRLF bytes instead of normalized LF.
After correcting that registration, the final suite passed with the same CLI
binary. A saved-results verifier rechecked all 29 suites using the original
`check_case` acceptance logic, pinned reports and native artifact hashes. Its
composed result is `artifacts/m4-counter-usage-verified-gate.json`; the interrupted
runner remains incomplete and is preserved, not relabeled as a successful run.

The earlier
[initial-counter provenance audit](INITIAL_COUNTER_BOUNDARY_AUDIT.md) remains
historical evidence: captures that really consume missing initial counts still
must reject, irrespective of original-player pixel similarity.
