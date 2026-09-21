# Native serial submission for Quad diagnostics

`application/QuadSerial.cpp` migrates the original `quad_serial.py` helper.
It is an internal diagnostic building block. The native counter execution,
artifact export and Qt Quad panel are still pending; the delivered application
package remains `out/FloraGPA-coverage-ui/FloraGPA.exe`.

## Preserved behavior

- Point/line/triangle lists and line/triangle strips are submitted one primitive
  and instance at a time. HS/DS/GS, adjacency and active SO require the separate
  final-geometry strategy and are rejected here.
- Strip assembly reads the currently bound native index buffer, including
  preceding GPU writes and scoped experiment edits. It respects R16/R32 formats,
  binding offsets, starting indices, signed base vertices, restart markers,
  odd-triangle corner order and degenerate primitives. Empty, short and
  zero-instance draws retain the original empty-draw metadata and avoid reads.
- Nonindexed strips retain absolute vertex fetch addresses while compensating
  the native VertexID. All split submissions compensate InstanceID and, where
  required, VertexID with the previously migrated DXBC transformation.
- Per-instance VB addressing preserves the captured starting instance, offsets,
  strides and step rates, including zero. Overflow and mixed slot step rates
  fail explicitly. IA vertex/index bindings and topology are restored through
  RAII on normal completion and partial-submit failure.
- Diagnostic VS variants use experiment-replaced bytecode, original class
  linkage and bound class instances. They are cached by instance/vertex offset.
- `DrawParameters.cpp` extracts the existing PostTransform parameter resolver
  without changing its behavior. Indirect arguments come from current GPU
  storage; DrawAuto uses the replay's captured/reconstructed SO count and its
  original provenance metadata.

The caller must own the bound pipeline and private-output inspection scope.
This helper restores IA bindings; full shader/pipeline restoration remains the
caller's responsibility, as in Python. It neither replays the prefix nor counts
its helper draws as original frame events. This distinction is required when
integrating the counter's multiple isolated passes and single original draw.

## Validation design

`tools/validate_quad_serial.py` compares the unmodified original Python helper
with a C++ probe that owns native replay and serial submission. It compares full
output-storage SHA-256 values, submission counts, all diagnostic metadata,
execution counts, exact rejection messages and native IA binding restoration.
DrawAuto cases also compare SO storage, including unused buffer bytes.

The matrix includes exhaustive ternary strips through six indices, both native
hardware and WARP, culling/front-face modes, direct/indirect draws, negative base
vertices, restart/degenerate/empty strips, per-instance steps 0/1/2/3, byte-offset
overflow after partial submission, dynamic VS class instances, GPU-written
indices/arguments and experiment edits/replacement shaders. DrawAuto fixtures
cover interleaved/paired/padded records, append cursors, implicit SO strides and
a stale captured count corrected from actual SO history.

The first matrix exposed 24 differences solely in duplicate Python
`Dispatch_records` counters. Python increments both `<name>_records` and the
execution counter; the native engine exposes the latter. The verifier now
asserts those duplicate counters agree before removing them for comparison.
Original unnormalized expected/actual JSON remains in each evidence directory.
No image, submission, diagnostic metadata or execution-count comparison was
relaxed. The initial evidence is retained in `artifacts/quad-serial-v1/`.

The final matrix passes **2,378 comparisons**: 2,186 exhaustive CPU strip cases
and **192 GPU cases**, including 10 expected rejections with exact messages.
All expected/actual values and original source hashes are retained in
`artifacts/quad-serial-final/`; the report identifies the native probe binary.
The earlier DrawAuto expansion passes 2,364 comparisons in
`artifacts/quad-serial-v2/`. That run preceded ordinary Draw/DrawIndexed and
experiment VS-replacement cases and is not counted again.

The complete Release build passes (`artifacts/build-quad-serial-full.log`).
Four independent PostTransform Python/native comparisons pass on hardware and
WARP (`artifacts/quad-serial-post-transform/validation.json`), covering all
geometry metadata, vertex bytes, tables and OBJ output after parameter-resolver
extraction. GPU operations are run serially.

Ten relevant CTest suites pass: `quad_serial`, `dxbc_quad`, `post_transform`,
`geometry`, `stream_output`, `class_linkage`, `ia_setters`, `coverage`,
`coverage_ui` and `core` (`artifacts/ctest-quad-serial-final.log`). The new QtTest
suite has four passing cases including setup/cleanup, with no skips. It also
compares a native unsplit triangle's complete color/depth storage to serial
submission and checks rejected adjacency state restoration.

GF2/BF1 original golden pixels and both suppress-draw negative controls pass,
with original command counts and runtime module audits under isolated child
process paths (`artifacts/quad-serial-golden/validation.json`). These checks
protect existing replay; they do not prove a complete native Quad feature.

The module ledger marks `quad_serial.py` as `ported`; all its callable helper
responsibilities are present. Counts are now 22 ported, 117 partial and 65
pending. These are module statuses, not a percentage of completed work.

## Remaining Quad work

Final HS/DS/GS/SO primitive capture and bridge shaders, target/depth adapters,
per-pass private UAV/output isolation, counter/reference execution, artifact
reports and Qt controls still need integration. Serial splitting changes GPU
scheduling; this diagnostic must retain the original experimental scope and
must not be presented as a physical quad invocation measurement.
