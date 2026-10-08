# Resource selection identity and recovery

This M5 correction follows [input clone LOD acceptance](LOD_CLONE_INHERITANCE.md).
Replay compatibility remains scoped to the separately recorded M4 matrix.
Implementation: `61ec9a8` (`Keep resource selection and image ownership consistent`).

## Corrected behavior

Directly selecting a texture previously changed its image/resource ID but left
the resource browser and footer naming the previous RTV. An already-current
resource-table row could also fail to leave binding mode. Final Frame retained
the stale tree selection, and loading an unbound project could retain the
default RTV selected during event navigation.

Explicit resource inspection now clears binding selection, including a real
click on an already-current table row. It does not guess a view when a resource
has both RTV and SRV aliases. Final Frame and manual output target/boundary changes
leave binding mode. Accepted images update the active resource workspace's
caption and edit identity from the checked resource ID. Background replay does
not replace a shader or buffer editor's active selection.

Project restoration reapplies the saved output target, subresource range and
boundary after event navigation, then restores only an explicitly saved binding.
No new project format or direct-texture persistence feature is introduced.

Unavailable bindings clear stale images and disable editing/export of their
previous resource. Coverage diagnostic images keep their displayed PNG export,
but cannot export a previous resource's raw output/texture storage or accept
texture edits. Selecting a valid resource afterward restores ordinary actions.

Image publication now checks the reported resource before changing the image:
missing/unknown IDs, non-image resource kinds and mismatched texture requests
are rejected. This prevents an invalid report from poisoning later edit-action
updates. It does not certify every semantic field in every specialized report.

## Evidence

The preserved old-code test reproduces the incorrect selected RTV while directly
viewing texture 10 in an untouched original LOD capture. The expanded real-capture
UI case covers direct selection, same-row clicks, explicit SRV/RTV choices,
Before/Final output, no-output targets, and both bound and unbound project restore.

An existing self-owned synthetic frame proves the alias case: resource 20 has
both SRV and RTV identities. It correctly rejects capture-initial inspection
without saved bytes, then succeeds at the After boundary without choosing an
alias. An explicit unavailable-binding control clears old data/actions and
recovers on selecting the RTV. Coverage diagnostic and return-to-resource checks
verify raw export ownership. The original test first expected initial bytes and
then used the wrong expected error text; both failed logs remain as diagnostics.

Three isolated Worker controls corrupt only resource identity (missing, unknown,
wrong kind). Their otherwise valid image fixtures use the real resource ID 20
in the positive controls. Rejection must preserve the previous accepted image,
leave the UI operable and permit successful retry.

The final rebuilt binaries pass all seven relevant CTest suites serially in
463.43 seconds. Their 250 top-level Qt rows have no failures or skips: 12 resource,
83 main UI, 6 thumbnail, 11 display, 28 image reader, 102 isolated recovery
(100 scenarios plus setup/cleanup), and 8 persistent recovery rows. Nested child
totals are not counted again. The earlier seven-suite pass predates the final
diagnostic raw-export change and is retained separately.

## Package acceptance

`out/FloraGPA-resource-selection-20261008/` contains 44 files. Only `FloraGPA.exe`
differs from the accepted LOD-clone package; all 43 other files, including CLI
and Worker, are byte-identical. The predecessor's 557 registrations / 547 unique
captures, 521 completions and 36 located refusals are inherited, not rerun or
reclassified by this UI change. Completion is still separate from fidelity.

Five serial checks pass from a relocated directory containing spaces and Chinese
characters with a system-only PATH: Windows selection/project/LOD UI (16 Qt rows),
Windows alias handling (3 rows), two GF2/BF1 recovery iterations (3 rows), and
shipping GUI screenshots of GF2 and BF1. Four independent golden/control CLI
replays also pass their strict image hashes, execution counts and runtime-module
checks. Screenshots were reviewed for resource identity and image consistency.

The [machine-readable baseline](resource-selection-baseline.json) pins nine source
files, test executables, 44 package files and 344 local evidence files, including
failed development controls. Evidence paths are local and not supplied by a Git
clone. Final CTest logs use `artifacts/m5-resource-selection-accepted-*`; portable
results use `artifacts/m5-resource-selection-portable QA 中文/validation.json`.

Captures, binaries, screenshots and process logs stay outside Git. No GPA or
Python component is added to the C++ runtime. This is not completion of M3/M4/M5,
clean-host deployment or renewed long-duration stability acceptance.
