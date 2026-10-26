# Native DXBC vertex input observation

This P6 checkpoint connects actual GPA draw inputs to the independent C++ DXBC
executor. It does not complete VS debugging, replace the legacy debug UI, or
close the RenderDoc removal gate.

## Input acquisition

`vertexInputShader` reads the effective SM4.0, SM4.1 or SM5.0 vertex shader. It
retains its input signature, including ISG1 minimum-precision metadata, and the
actual input declarations. The generated program contains input/output
declarations, bit-preserving `mov` instructions and `ret`. Original arithmetic,
resource declarations, UAV writes, atomics, class instances and debug metadata
are not copied into the acquisition program.

Every declared input component is copied to a synthetic unsigned Stream Output
field. All 32 input registers can be captured without spare UAV slots or spare
original output registers. Undeclared components remain invalid in the decoded
`Value`; they are not filled with plausible values. An input-free shader uses a
private output word to count points and produces an empty input register map.

`captureVertexInputs` creates a separate D3D11 device on the reference device's
exact adapter LUID and feature level. WARP is explicit and must match the
reference device. It copies supplied buffer snapshots into private storage and
recreates the input layout. Original IA offsets, index values, base vertex,
instance count, start instance and step rates are submitted to D3D11. The GPU
performs format conversion and component defaulting; the acquisition code does
not approximate these conversions in C++.

The helper uses point topology, private SO/staging buffers, an SO statistics
query and the existing bounded/cancellable GPU completion implementation. A
count or overflow mismatch fails the observation. With a debug reference device,
D3D11 validation errors fail the helper. No reference-context bindings, original
UAV counters, SO cursors or predicate state are modified by the helper.

Microsoft documents the IA system-generated IDs and the draw offsets separately:
[Using System-Generated Values](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-input-assembler-stage-using)
and [DrawInstanced](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-drawinstanced).
The tests independently verify actual system ID bits and attribute addresses,
including nonzero start/base values and instance step rates 0, 1 and 2. Metadata
is not used as the only oracle.

## GPA event boundary

`snapshotVertexInputs` replays to the prepared before-draw boundary and observes
the live IA layout and buffers inside the experiment scope. Shader overrides,
buffer edits and indirect arguments are therefore current. The selected input
is a zero-based draw reference and a draw-relative instance, with cut indices
retained in the reference numbering. Out-of-range references, selected cut
indices, disabled draws and false or unknown observed predicates fail explicitly.

The selected `vN` values are added to the existing resource snapshot, which
retains the effective shader, frame hash, generation, actual adapter identity,
constant buffers, resources and samplers. The accompanying invocation record
uses GPA IDs and records offsets, parameters, helper identity and provenance.
It does not claim to recover original VS cache reuse, invocation order or
original captured predicate results.

The older `PostTransform` stage named `vs-inputs` runs the original VS and
captures its **outputs** with input-reference identities. It remains useful as
a separate GPU execution comparison, but is not used as initial `vN` state.

## Reproduction and evidence

Build `FloraDxbcVertexInputsTests`, `FloraDxbcVertexInputProbe`,
`FloraDxbcInputSnapshotTests` and `FloraNativeVertexInputsTests` in Release. Run
the three corresponding CTest suites serially:

```powershell
ctest --test-dir build/vs2022 -C Release -R '^(dxbc_vertex_inputs|dxbc_input_snapshot|native_vertex_inputs)$' --output-on-failure -j 1
```

The new tests run on local NVIDIA, Intel and explicit WARP devices. They cover
full/sparse/shifted input masks, SM4/SM5 profiles, NaN and integer raw bits,
minimum precision, normalized/half/float IA conversion, default components,
indexed/instanced offsets, repeated and cut indices, budgets and cancellation.
The original test shader can contain a UAV atomic without requiring any UAV
binding in the helper. GPA fixtures also cover direct/indirect draws, effective
shader and vertex-buffer edits, selected-lane C++ execution, restored original
buffer bytes, rejection and retry. Existing resource-snapshot and mesh-input
tests remain unchanged.

The final targeted run records 235 passes, no failures and no skips: 36 new
vertex-input checks, 82 resource-snapshot checks, 54 existing mesh-input checks
and 63 native compute debug checks. GUI, CLI and Worker Release targets also
build successfully. This suite result is separate from the unresolved real
shader execution comparisons below.

The standalone probe accepts:

```text
FloraDxbcVertexInputProbe <frame.gpa_frame> <event> <reference> <instance> <vendor-or-0-for-WARP> <report.json> [--execute | --compare-native]
```

`--compare-native` also executes the original shader through the existing native
point-output diagnostic and records every output component's raw bits and
validity. It reports differences without silently accepting them as floating
point tolerance. Neither this probe nor these tests launch RenderDoc or GPA.
The original GPA captures stay outside Git. The evidence ledger records hashes
of local logs/reports and summarizes the observed outcomes:
`research/dx11-native-vertex-input-evidence.json`.

## Remaining acceptance gates

- GF2 event 113, reference 0, instance 0 completes 12 executor steps and matches
  all 14 native output components exactly on NVIDIA, Intel and WARP.
- BF1 event 876 at the same selection completes 58 steps, with all four output
  components valid. The initial three floating-component differences on NVIDIA
  and Intel are resolved by the arithmetic precision continuation; all four
  components now match exactly on all three adapters without changing a golden.
- Helldivers event 200 initially rejected a minimum-precision operand extension.
  The precision continuation now completes 42 steps and exactly matches all 13
  output components on NVIDIA, Intel and WARP. See `DX11_DXBC_PRECISION.md` and
  `research/dx11-native-precision-evidence.json` for encoding, independent probes,
  raw comparison evidence and remaining precision limitations.
- The selected-input continuation now submits one point and allocates one output
  record, while retaining whole-draw acquisition as a comparison path. Complete
  bound buffer snapshots are still retained and copied; large resources can
  exceed the byte budget. See `DX11_SELECTED_VERTEX_INPUTS.md` for provenance,
  address limits, validation and remaining performance gates.
- Out-of-bounds index snapshots are rejected; their complete runtime semantics,
  unbound/missing IA inputs, dynamic class execution, original undefined values,
  atomic schedules and all deferred/DrawAuto experiment combinations still need
  explicit coverage. Existing resource-snapshot restrictions continue to apply.
- Native VS trace journaling, source/debug sessions and CLI/GUI dispatch are now
  available through the continuation in `DX11_NATIVE_VERTEX_DEBUG.md`. Arbitrary
  index override is now implemented for valid selected references, as described
  in `DX11_VERTEX_INDEX_OVERRIDE.md`. Full VS parity remains open with PS input/quad
  discovery, fragment history, remaining executor semantics and P7/P8 standalone
  release acceptance.

Original successes must still be preserved before retiring any legacy feature.
