# Native VS identity and unique output tables

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

Geometry's **VS identities** stage ports the recovered `vs-index` path. It
associates actual assembled vertex-shader output with the original instance,
input vertex index and native VertexID. The Qt table selector exposes expanded
outputs, unique output variants and the reference mapping. Export includes the
original geometry files plus `unique_vertices.csv`, `unique_vertices.bin` and
`references.csv`. Stage and table choices round-trip through experiment projects,
including the Python `VS索引` setting.

## Native instrumentation

`DxbcIdentity.cpp` preserves the original SM4/SM5 vertex shader tokens and adds
private output components carrying native VertexID and InstanceID. It reuses
consumed system inputs, supplies a constant zero for a single original instance,
or allocates a whole unused input register. Output allocation prefers an unused
register, then an unused component. Signatures, declarations and declared index
ranges reserve occupied storage; generated semantic names avoid collisions.

New declarations and identity moves precede executable instructions, preserving
original control flow, math, class linkage and logical system IDs. Stale debug
and statistics chunks are removed. The native DXBC builder retains chunk order
and uses the existing retail checksum implementation. Exact patched DXBC hashes
are part of the Python comparison, not just shader-creation success.

For an SM5 shader, an operand-aware walk detects UAV declarations and uses,
including nested relative operands and four-DWORD immediate64 vectors. Introducing
an unconsumed system input into a UAV shader is rejected because it can change
native invocation reuse and side effects. This batch implements the analysis
needed for that guard; it does not yet implement general UAV relocation.

The augmented shader runs through the existing private-output SO capture path.
Original shader attributes are extracted from the augmented output, retaining
their exact bytes. Indexed identities are checked against the **current GPU
index buffer** after preceding commands and scoped input edits; strip-cut values
are excluded. Indirect arguments and DrawAuto parameters use the existing native
resolvers. VertexIndex applies the original signed BaseVertex or StartVertex
offset without rewriting either the draw or the system inputs.

Selected instances filter the one full native capture by its actual identity.
Incomplete primitives and omitted adjacency vertices are not fabricated. Invalid
instances, nonzero streams, exhausted signature storage and unsupported addressing
fail explicitly. Disabled or empty draws yield empty tables without instrumentation.

## Bit-preserving unique rows

Identity is `(instance, vertex_index)`. A unique row is an identity **and its exact
output byte pattern**, so signed zero and different NaN payloads remain distinct.
Each reference records identity, variant and unique-row indices. Reconstructing
expanded output through `unique_vertices.bin` and `references.csv` must reproduce
`vertices.bin` exactly. Reports expose unique identity counts, conflicting identity
counts, native markers, original parameters and original/patched/full-output hashes.

```powershell
# Run from the repository root; set external paths for your environment.
$CaptureRoot = 'C:/captures'
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
.\out\FloraGPA-vs-identity\FloraGPA.Cli.exe post-geometry "$CaptureRoot/sample.gpa_frame" --event 100 --geometry-stage vs-index --instance 2 --out "$ResultsRoot/vs-identity"
```

## Evidence and limits

The development matrix passes 104 comparisons on Hardware/WARP: indexed and
nonindexed native assembly, point/line/triangle lists and strips, adjacency,
patch control points, 16/32-bit indices, negative base vertex, GPU-written indirect
arguments, instancing/step rates, zero strides, empty draws, packed output
components, exhausted output registers, semantic collisions, SM4.0/4.1/5.0, and
UAV guards. Every successful comparison checks exact metadata (including patched
DXBC hash), both binary tables, numeric CSV/OBJ values and lossless reconstruction.
Expected rejections are counted separately from successful inspections.

The original GF2/BF1 development comparison passes 16 cases at GF2 event 181 and
BF1 events 876, 1415 and 11276, including tessellation control points. These check
full output and selected instances. Final package evidence is recorded in
`MIGRATION_STATUS.md`.

The final package extends this to **108 synthetic cases** (100 successful
captures, eight expected rejections), including an edited GPU index absent from
initial storage and a disabled event. Its **16 original-frame cases** compare
full output and the last original instance, including BF1 event 1415 instance 8.
The previous Final/VS/DS/GS matrix remains green at **140 cases**. Release CTest
passes all 39 suites; the original Qt suite passes 53 cases and the extended
geometry suite passes nine cases including setup/cleanup.

Evidence: `artifacts/vs-identity-portable-final/validation.json`,
`artifacts/vs-identity-real-portable-final/validation.json`,
`artifacts/vs-identity-post-regression/validation.json`,
`artifacts/ctest-vs-identity-final.log` and
`artifacts/vs-identity-tests-final.txt`. Binaries and capture evidence remain
outside Git; reference source hashes are retained in the validation reports.
GF2/BF1 full-frame golden and suppressed-draw negative controls also pass
(`artifacts/vs-identity-golden-final/validation.json`). The final module audit
checks 250 successful reports, verifies package-local Qt, finds no
Python/Tk/GPA/RenderDoc runtime and confirms all packaged EXEs match the final
Release build (`artifacts/vs-identity-runtime-audit.json`).

The native geometry suite also checks NaN/signed-zero variants, original UAV data
and hidden counters, preserved SO storage/cursors with subsequent direct native
draws, overflow followed by limit failure/retry, real Qt Worker calls, all three
tables, complete UI export and project save/restore. The verified UI screenshot
is `artifacts/vs-identity-ui-final/vs-identities.png`.

The full migration remains open. HS output, per-invocation VS/DS writes and GS
emissions were migrated in subsequent batches; see `HULL_OUTPUT_MIGRATION.md`
and `OUTPUT_LOG_MIGRATION.md`. Coverage/quad consumers and shader debugging remain
pending. Broad dynamic class-linkage combinations, malformed-token fuzzing,
large-output memory stress and a clean-machine run remain verification gaps.
As in the prior geometry batch, the SO storage limit does not bound the memory
used by all attribute tables and mesh JSON. Atomic return ordering is not made
deterministic by private output isolation.
