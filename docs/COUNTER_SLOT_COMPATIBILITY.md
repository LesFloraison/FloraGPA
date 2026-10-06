# UAV counter dependencies by shader slot

Reviewed 2026-10-06. This M4 correction removes a reproduced false rejection of
mixed-slot UAV workloads. It preserves unavailable counters and does not change
shader code, dispatch parameters, resource bytes or recorded reset values.

## Original trigger and controls

Three unmodified GPA 2025 R1 captures come from the extended self-owned buffer
view producer. Each mode runs twelve frames without GPA and twelve under capture
injection. Both complete 64-byte buffers and the final 8x8 RGBA image are checked
against independent CPU oracles on every frame: 72 producer frames in total.
The capture files and frozen producer sources are retained outside Git.

| Mode | Hidden counter operation | Companion UAV at u1 | Before correction |
|---|---|---|---|
| 40 | IncrementCounter at u0, reset to 2 | Indexed write, KEEP | Rejected at Dispatch |
| 41 | IncrementCounter at u0, reset to 2 | Indexed write, reset to 7 | Passed control |
| 42 | IncrementCounter at u3, reset to 2 | Indexed write, KEEP | Rejected at Dispatch |

Both rejected originals stop at event 45 because created UAV 38 has no defined
counter. Its storage is buffer 9 and it is only indexed by the shader; the actual
counter operation addresses UAV 23 / buffer 5 through u0 or u3. The original
player replays all three files twice with stable output. Its adapter is not
identified, so this is observed output evidence rather than matched-device
execution equivalence.

## Correction and limits

Previously, any hidden-counter instruction caused all bound counter UAVs to
require a defined count. The checked DXBC decoder now records the immediate UAV
register used by each IMM_ATOMIC_ALLOC / IMM_ATOMIC_CONSUME instruction. Their
view-local counter behavior and operand encoding are documented by Microsoft:
[SM5 instruction semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-alloc--sm5---asm-)
and [tokenized program format](https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/D3D12TokenizedProgramFormat.hpp).

Preflight combines the captured shader-stage slot requirements. Runtime combines
requirements cached for the actual native shader objects, including replacements.
A view bound in multiple slots requires a value if any of those slots may consume
its counter. Ordinary writes at independent slots do not establish or reset its
hidden value. Direct counter inspection and CopyStructureCount still require it.

Only checked SM4/5 programs and the understood immediate operand layout provide
slot-specific proof. Unknown versions, dynamic linkage, unrecognized instructions,
extended/relative operands, invalid slot indices and unfamiliar destination
layouts conservatively retain all 64 slots. Invalid container/program lengths
and duplicate executable chunks still reject. Reflection stripping is supported.
The scan includes every instruction; it does not prove branches unreachable or
infer which side of runtime control flow executes. Wider operand/control-flow
analysis, missing capture contents and M3 remain open.

## Validation design

The new original-capture tests run all three modes on hardware and WARP, repeat
each replay, compare both full buffers, check the used counter is 3, and verify
the companion stays unavailable for KEEP or remains 7 for the explicit-reset
control. A stripped shader replacement that starts consuming u1 must reject at
the correct Dispatch and view. An explicit experiment seed then admits that
replacement and yields the checked counter value 8. No seed is inserted into the
ordinary replay path.

DXBC tests cover immediate slot extraction, stripped code, out-of-range UAV slots,
extended and non-immediate operands, invalid destination masks and the preexisting
unknown-interface/opcode/length cases. The registered corpus adds exact buffer
boundaries before and after Dispatch and a disabled-Dispatch image control for
each original. The GUI workflow opens the three originals, checks all pixels and
navigates to their last API event using the actual Worker.

Producer commit: `705a4e1`. Runtime: `d09352a`. Initial build evidence includes a
Qt test macro collision with the local variable name `slots`; the test variable
was renamed before the successful build. This did not change decoder semantics.

## Registered-scope regression

The continuous serial gate passes 30 suites: 492 registrations / 482 unique
capture hashes, with 466 native completions and 26 located rejections. It runs
984 ordinary attempts, 680 diagnostic controls and 716 resource exports. The
previous 489 registrations retain their complete diagnostic findings, statuses
and deterministic image hashes. Helldivers retains its observed variation and
separate policy. The full gate was performed with the packaged CLI.

The 466 completions still include 33 known capture limitations. Capture labels
are 423 unassessed, 36 passed within their documented scope, 32 capture-side
mismatches and one information loss. Independent application-image references
now cover five cases: four match and one known missing-data MSAA case differs;
487 remain unassessed on that axis. These counts do not establish an overall
feature-completion percentage or full workload equivalence.

## Final acceptance

All eight related CTest suites pass serially: `uav_counters`, `frame_validation`,
`pipeline_creation`, `shader_setters`, `class_linkage`, `texture_creation`,
`buffer_view_creation` and `ui`. The focused counter suite passes 34 rows without
skips, including twelve original hardware/WARP rows across the old and new
fixtures. Full Qt interaction passes 63 rows without skips. A separate portable,
system-PATH Qt harness opens and navigates all three new files; its three rows
include setup/cleanup. It links the production UI library and uses the packaged
Worker, but is not a new production executable launch or clean-machine claim.

The final 44-file package passes GF2/BF1 strict goldens and disabled-Draw controls
(four checks), with native dependency audits. The two original-player batches
perform six runs each, before and after the correction. All three corrected
files have stable images observed equal to the original player. Their twelve
registered before/after buffer boundaries each pass both repeat exports.
The [pinned baseline](counter-slot-baseline.json) retains hashes of raw evidence,
including the initial test compilation error and its successful correction.
Package: `out/FloraGPA-counter-slots-20261006/`. Qt workflow commit: `ec94f8a`.

No game shader is modified and no hidden counter value is invented. The registry
continues to distinguish capture fidelity from replay completion. The module
ledger remains 72 ported / 117 partial / 15 pending. M3, broader M4 boundaries,
long-duration M5 operation and independent-machine deployment remain incomplete.
