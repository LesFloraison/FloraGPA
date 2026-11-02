# DX11 native backend: GPA feature boundary

Updated 2026-11-03. Branch: `feature/dx11-native-backend`.
Reference: installed GPA 2025 R1, version `25.1.26603.47871`.

## Acceptance policy

This ledger supersedes the former P0-P8 RenderDoc parity gates. Hidden protocols,
unavailable UI and capabilities added by RenderDoc do not create product requirements.
The previous ledger remains available in Git history. The current results and
problem classifications are recorded in [the closure report](NATIVE_GPA_CLOSURE.md).
Development closure and formal release acceptance are separate decisions.

| Capability | Decision | Evidence / implementation boundary |
| --- | --- | --- |
| Event pixel history | Retain | `research/DX11_PIXEL_HISTORY_DIAGNOSTICS.md`: event, pixel bytes, diagnostic status/count; overlapping triangles produce one event row |
| VS/PS/CS execution debugger | Remove | `research/DX11_DEBUGGER_AVAILABILITY.md`: original presenter property is constant false, independently verified |
| Per-fragment identity, ordering, shader outputs and merger reconstruction | Remove | Not required by the observed GPA event result; do not revive full RenderDoc parity gates |
| Native mesh/resources/counters | Retain | Direct GPA identities and isolated native Worker; no recapture |
| Event storage history | Internal validation only | No GUI mode or public CLI command; shared snapshots remain implementation infrastructure |
| GS/HS/DS checkpoint controls, stepping, watches and trace exports | Remove | No verified available original UI; shared mesh/instrumentation code remains internal |
| Shader inspection/replacement, Coverage/Quad, experiments, Intel metrics | Retain | Existing GPA-aligned scope; preserve shared source/format/instrumentation infrastructure |
| Independent live capture | Unchanged | Outside this branch closure task |

## Implementation status

- Recapture API, external loader/worker, vendor headers and runtime packaging removed.
- Execution debugger UI, sessions/interpreter and private-fragment implementation removed.
- Native mesh is the sole GUI path; its shared position/export model remains.
- Native analysis backend identity advanced to `dx11-native/21` to invalidate old results.
- Removed `debug-*`, `private-draw`, `private-primitive`, `rdc-analyze` and `--renderdoc`
  requests fail instead of selecting an external backend. Existing analysis/renderdoc
  settings are ignored. Checkpoint controls and their dedicated UI tests are retired.
- `shader-checkpoint` and public `history --history-scope events` are rejected;
  internal storage/model tests remain development tools.
- `history --history-scope gpa` has a separate event-diagnostic record contract.
  Its initial implementation is restricted to single-sample 2D color targets, layer
  zero; depth-only and broader target semantics are not certified. It uses isolated
  prefix replays, original event pixel readback, and counterfactual diagnostic draws.
  The pinned original twelve-scenario fixture is checked on each local hardware
  adapter; that evidence does not extend the supported target scope.

## Required closure evidence

1. Full configure/build and retained tests; retired tests do not count as passes.
2. Original-GPA differential history matrix: ordinary/same-value writes, depth,
   stencil, discard, blend, zero write mask, scissor, sample mask and overlap.
3. GF2/BF1/Helldivers replay, resources, mesh and metrics on NVIDIA/Intel/WARP as
   appropriate; same-device comparisons and explicit unsupported cases.
4. Cancellation, timeout/crash/retry, stale contexts and state isolation.
5. Fresh portable package, import and runtime process/module audit without GPA
   or RenderDoc dependencies. Independent clean-host validation is a formal
   release gate, separate from branch development closure.
6. Reproduce and classify the three historical baseline failures without weakening
   assertions. New regressions and release blockers must be fixed.

## Historical research

Existing DXBC execution, shader-debugger and private-pixel research documents and
binary-analysis evidence are archival. They explain removed work, not remaining
release gates. Their old executable names and reproduction commands refer to the
historical commits. Original binaries/databases remain local research inputs and
must not be linked or packaged into FloraGPA.

## Validation checkpoint — 2026-11-03

**Bounded development closure: complete. Formal release acceptance: not passed.**

The final build is `build/native-closure`, configured from a new directory;
`build/native-gpa` and the prior `out/FloraGPA-native-20261102` package are historical
inputs only. Source/import/runtime audits, the product entry inventory, retired
tests, full-suite outcomes and portable-package hashes are consolidated in
[NATIVE_GPA_CLOSURE.md](NATIVE_GPA_CLOSURE.md) and
[the compact evidence ledger](research/native-gpa-closure-validation.json).

The effective 150-suite result is 101 fully passed, 45 with skipped subcases,
and four failed. The NVIDIA class-linkage crash is handled by a narrow explicit
rejection with WARP fallback guidance. The BF1 golden mismatch, compute feedback
qualification and Intel hardware numerical qualification remain documented;
assertions were not weakened. Skipped cases do not count as passed acceptance.

The fresh package is a validation deliverable. A separate Windows host without
GPA/RenderDoc has not been exercised, so formal release acceptance remains false.
No merge, tag, push or publication is performed by this closure task.
