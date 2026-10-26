# Native VS attribute index override

`analyze debug-vertex --index N` now overrides the effective per-vertex attribute
index instead of asserting that it equals the original draw's index. The native
VS/CS view exposes an optional **Actual index** field for VS; leaving it empty
uses the draw. Zero is a valid explicit override. The index is an unsigned
32-bit value with all draw offsets already applied.

## Input semantics

The selected input reference and instance still identify the original draw.
The override changes per-vertex IA attribute addresses only. Per-instance
attributes retain the original start instance and step-rate behavior.
SV_VertexID retains the selected original raw index (indexed draws) or relative
reference (non-indexed draws); SV_InstanceID remains draw-relative. Neither ID
is inferred from the overridden attribute address.

Acquisition uses a private one-point R32 index with base vertex zero. For
UINT32_MAX it uses index UINT32_MAX-1 and base vertex one, avoiding both a cut
index and the D3D11 non-indexed start/count overflow check. The native IA still
performs format conversion and out-of-range attribute reads. The helper never
runs the application's shader body or changes the replay's bound resources.

The existing original reference/instance, predicate and selected index validity
checks remain. A selected cut index, missing index buffer or out-of-range index
reference is still rejected even when an override is present; this checkpoint
does not establish legacy parity for those cases. Full source-buffer snapshots
and the prior instance-address overflow restriction also remain. These limits
must be considered in the final capability matrix before removing legacy code.

## Results and compatibility

New VS jobs publish `flora.native-vertex-debug` version 2. Their invocation has:

- `observed_vertex_index`: the original index plus base/start vertex offset.
- `vertex_index`: the effective per-vertex attribute index used for this run.
- `index_override`: the explicit request value, or null.
- `vertex_index_source`: `explicit_override` or `draw`.
- `status`: `explicit_vertex_index_override` for an explicit input override,
  otherwise `eligible_in_observed_replay`.

An overridden execution is not labeled as an observed original invocation.
Version 1 sessions remain readable with their previous index-assertion contract.
Version 2 checks the request, original offset arithmetic, effective address,
provenance/status and journal header's `index_override` together. Missing,
negative, boolean, oversized or mismatched journal indices fail. Existing shader,
frame, adapter, artifact and reversible-state validation remains in force.
The journal format remains version 1 with an additive optional index field;
older journals without the field represent no override.

Editing the optional UI index invalidates the active selection. Empty and zero
are distinct, stale results with an extra index are rejected, and the input is
disabled during active work. GUI report exports now include the invocation
metadata as well as the analysis context. Session archives retain both forms.

## Validation and reproduction

`FloraDxbcVertexInputsTests` exercises changed attribute indices 0 and 7,
UINT32_MAX out-of-range reads, indexed/non-indexed draws, negative base vertex,
instance step rates and unchanged system IDs on NVIDIA, Intel and explicit WARP.
Direct and indirect GPA fixtures run original, index-0 and index-4 Worker/CLI
jobs with experimental shader/buffer edits and verify resulting registers,
rewind and module audit. The former unsupported-index rejection is replaced by
positive execution checks and a conflicting-index-request rejection; original
cut/bounds/budget/cancellation checks remain. No golden or skip rule changes.

Session tests mutate the original/effective index, provenance, status, request
and journal independently. UI tests cover empty versus zero, UINT32 limits,
invalid text, stale selection, busy state, stage switching and exported metadata.

Real-capture validation uses an identity override, equal to the previously
observed effective index, so the existing native-output oracle remains valid:

```powershell
python tools/validate_selected_vertex_inputs.py --bin build/vs2022/Release `
  --captures D:/CDXrepo/captures `
  --baseline artifacts/dx11-native/vertex-precision-validation `
  --out artifacts/dx11-native/vertex-index-real --identity-index-override
```

Use a new output directory and add Qt to PATH. The runner executes GPU work
serially and retains baseline hashes, commands, executable identities and
session/module checks. Identity overrides validate transport and state retention;
they do not substitute for the synthetic changed-index address checks. See
`research/dx11-vertex-index-override-evidence.json` for results and intermediate
failures. Full VS/PS/source parity, fragment history and standalone P7/P8 removal
acceptance remain open.

The final ten-suite run passes **446 checks, zero failures and zero skips**.
All nine real identity-override jobs pass exact register/native-output checks,
Worker module audits and exported session reloads on NVIDIA, Intel and WARP.
The selected effective index is 0 for GF2/BF1 and 39,562 for Helldivers. Three
previously generated version 1 real sessions also load, rewind and export with
the new reader. GUI, CLI and Worker Release targets build successfully.

An initial boundary run correctly failed D3D11 validation for a non-indexed
UINT32_MAX start. The private R32 index/base-vertex representation fixes that
failure without removing the boundary assertion or disabling validation.
Intermediate build/test logs remain in the evidence ledger. These results do
not establish complete debugger compatibility or standalone release acceptance.
