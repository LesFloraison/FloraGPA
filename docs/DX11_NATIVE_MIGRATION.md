# DX11 native backend: GPA feature boundary

Updated 2026-10-31. Branch: `feature/dx11-native-backend`.
Reference: installed GPA 2025 R1, version `25.1.26603.47871`.

## Acceptance policy

This ledger supersedes the former P0-P8 RenderDoc parity gates. Hidden protocols,
unavailable UI and capabilities added by RenderDoc do not create product requirements.
The previous ledger remains available in Git history. This change is not a release
acceptance claim: the gates below must be completed before the branch is closed.

| Capability | Decision | Evidence / implementation boundary |
| --- | --- | --- |
| Event pixel history | Retain | `research/DX11_PIXEL_HISTORY_DIAGNOSTICS.md`: event, pixel bytes, diagnostic status/count; overlapping triangles produce one event row |
| VS/PS/CS execution debugger | Remove | `research/DX11_DEBUGGER_AVAILABILITY.md`: original presenter property is constant false, independently verified |
| Per-fragment identity, ordering, shader outputs and merger reconstruction | Remove | Not required by the observed GPA event result; do not revive full RenderDoc parity gates |
| Native mesh/resources/counters | Retain | Direct GPA identities and isolated native Worker; no recapture |
| Event storage history | Retain distinctly | Storage observations are not GPA diagnostic statuses or fragment history |
| Shader inspection/replacement, checkpoints, Coverage/Quad, experiments, Intel metrics | Retain | Existing product scope; preserve shared source/format/instrumentation infrastructure |
| Independent live capture | Unchanged | Outside this branch closure task |

## Implementation status

- Recapture API, external loader/worker, vendor headers and runtime packaging removed.
- Execution debugger UI, sessions/interpreter and private-fragment implementation removed.
- Native mesh is the sole GUI path; its shared position/export model remains.
- Native analysis backend identity advanced to `dx11-native/20` to invalidate old results.
- Removed `debug-*`, `private-draw`, `private-primitive`, `rdc-analyze` and `--renderdoc`
  requests fail instead of selecting an external backend. Existing analysis/renderdoc
  settings are ignored. Existing checkpoint controls are shared and retained.
- `history --history-scope events` retains storage observations.
- `history --history-scope gpa` has a separate event-diagnostic record contract.
  Its initial implementation is restricted to single-sample 2D color targets, layer
  zero; depth-only and broader target semantics are not certified. It uses isolated
  prefix replays, original event pixel readback, and counterfactual diagnostic draws.
  Status/count equivalence must be independently checked before release.

## Required closure evidence

1. Full configure/build and retained tests; retired tests do not count as passes.
2. Original-GPA differential history matrix: ordinary/same-value writes, depth,
   stencil, discard, blend, zero write mask, scissor, sample mask and overlap.
3. GF2/BF1/Helldivers replay, resources, mesh and metrics on NVIDIA/Intel/WARP as
   appropriate; same-device comparisons and explicit unsupported cases.
4. Cancellation, timeout/crash/retry, stale contexts and state isolation.
5. Fresh portable package and clean-host run without GPA or RenderDoc; import,
   loaded-module and child-process audit. A developer-host module snapshot alone
   is not a clean-host validation.
6. Reproduce and classify the three historical baseline failures without weakening
   assertions. New regressions and release blockers must be fixed.

## Historical research

Existing DXBC execution, shader-debugger and private-pixel research documents and
binary-analysis evidence are archival. They explain removed work, not remaining
release gates. Their old executable names and reproduction commands refer to the
historical commits. Original binaries/databases remain local research inputs and
must not be linked or packaged into FloraGPA.

## Validation checkpoint — 2026-11-02

Main GUI, CLI and Worker build successfully in the fresh `build/native-gpa`
directory. Six focused CTest targets pass: native_analysis (61 Qt rows),
native_history (169), native_replay_mesh_ui (105), native_history_ui (15),
native_counter_ui (7), and replay_mesh_model. First-run compiler crashes and a
native-history driver crash are retained in artifact logs; the focused rerun
passes without changes to those assertions. Interrupted-build object corruption
was also observed during the full build and requires regeneration.

The original twelve-case history fixture matches on both NVIDIA and Intel:
ten returned event rows, exact event order, pixel bytes, status and diagnostic
count against both original callbacks per device. This validates that fixture,
not every GPA target/shader configuration.

`out/FloraGPA-native-20261102` is a validation package, not an accepted release.
With PATH restricted to Windows/system32, its GUI-adjacent CLI and Worker replay
GF2, BF1 and Helldivers and collect all-event counters. Runtime snapshots contain
no GPA/RenderDoc modules. GF2 and BF1 framebuffer hashes match the recorded native
baseline. This developer-host check does not replace a clean-host run.

The historical class_linkage crash reproduces. Full-suite completion, remaining
historical failure classification and broader GPA history target acceptance are
still outstanding. No branch closure is claimed. Compact results are in
`research/native-gpa-scope-validation.json`; complete logs remain under artifacts.

The historical hlsl_recovery suite was rerun with the capture corpus explicitly
set: nine cases pass and simultaneousBf1Replacements reproduces the unchanged
prior-baseline actual hash `3d431509ec8258047bc7381367a2861e36ba26abad658bc642524692c07426cc`.
The packaged root binaries also pass a dumpbin import audit for GPA/RenderDoc
imports. The validation ZIP identity is recorded in the compact evidence file.
