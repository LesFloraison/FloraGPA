# Native shader preparation for Quad diagnostics

This stage ports two prerequisites for the original serialized Quad diagnostic.
It does not expose a working native Quad command or Qt panel yet. The current
delivered GUI remains `out/FloraGPA-coverage-ui/FloraGPA.exe`.

## System IDs during split submissions

`application/DxbcSystemId.cpp::offsetSystemId` implements the complete
`dxbc_system_id.py` transformation for VertexID and InstanceID. It preserves
packed inputs by copying the entire declared mask into one private temporary,
then adds a uint32 offset to only the selected system component. The offset
retains native unsigned wrap semantics. Zero offsets and absent system inputs
return the original bytes and unchanged metadata.

Original input operands are rewritten at decoded token boundaries, including
nested relative indices and operand/instruction extensions. Immediate payloads
remain data, including validated four-word immediate64 vectors. Interface-call
function indices precede the operand walk. Unknown instructions, dynamic input
addressing, malformed tails, excessive recursion, unsupported signatures and
exhausted temporaries fail explicitly. The existing operand-arity table and
checked DXBC reader/container/checksum are reused. Stale debug/statistics chunks
are removed only for changed shaders.

## Array selection before counter side effects

`DxbcCoverage.cpp::filterQuadArrayIndex` implements `quad_targets.filter_shader`.
It reuses the producer-aligned array input declaration and uint equality from
the coverage transformation, then inserts `retc_z` before the first executable
instruction. It does not add a color output or move the test to shader exits:
nonselected indices must perform no original counter UAV instruction.

An absent selection returns the original shader. Without a routed array input,
index zero retains it and a nonzero index uses the original empty-PS compilation
behavior. These no-op and compilation paths preserve the Python result bytes.
Other `quad_targets.py` responsibilities remain pending integration.

## Evidence

- `tools/validate_quad_shaders.py`: **4,342** original-Python/native binary and
  metadata comparisons over **395** independent shaders, including **2,099**
  expected rejections. Inputs include real capture shaders, SM4/4.1/5 programs,
  packed and relative inputs, uint extremes, double literals, instruction
  extensions, interface calls, recursion limits, malformed encodings and the
  original counter/reference shaders. Report:
  `artifacts/quad-shaders-final/validation.json`.
- `tests/DxbcQuadTests.cpp`: actual hardware/WARP Stream Output compares all
  VertexID/InstanceID values from one draw against split submissions with
  nonzero starting vertices/instances. Five cases including setup/cleanup pass
  without failures/skips (`build/vs2022/dxbc-quad-Release.txt`).
- `tools/validate_quad_shader_gpu.py --suite identities`: **32** original GPU
  cases exercise indexed/nonindexed draws, instance step rates 0/1/2/3, ordinary
  draws and split instances/primitives. Uncompensated controls differ while
  native compensated outputs match. Fifteen native transformations are used.
- `--suite targets`: **691** checks execute native array-filter bytecode through
  the original counter fixture driver: hardware/WARP, arrays/mips, 2/4/8x MSAA,
  color/depth-only outputs, clip/discard, sample masks and all three depth modes.
  Independent atomic maps, histogram accounting, output storage, locks and
  prepared-depth integrity are checked. Eight native transformations are used.
- `--suite strips`: **80** hardware/WARP comparisons preserve complete rendered
  pixels and planned submission counts for point/line/triangle lists and strips,
  restart markers, degenerates, empty streams, 16/32-bit indices and indirect
  arguments. Twenty-six native transformations are used.

Each GPU suite retains a `validation.json` in
`artifacts/quad-shader-gpu-<suite>/`. These development drivers still use the
original Python scheduling code with injected C++ shader results; the **803 GPU
checks are evidence for these transforms, not a migrated native Quad engine**.
Original Python CLI/Tk subprocess checks are deliberately not counted because
their fresh interpreters would bypass the native injection.

The complete Release build passes (`artifacts/build-quad-shaders-full.log`).
Six relevant CTest suites pass: `dxbc_quad`, `dxbc_coverage`, `coverage`,
`coverage_ui`, `shaders` and `core`, including the real GF2 Coverage UI case
(`artifacts/ctest-quad-shaders-final.log`). Four original golden-frame/negative
controls pass with the updated Release CLI
(`artifacts/quad-shaders-golden/validation.json`).

`dxbc_system_id.py` is now `ported`; `quad_targets.py` is `partial` because only
its shader filter is migrated. Counts are 21 ported, 117 partial and 66 pending.
The complete Python-to-C++/Qt migration remains unfinished.

## Remaining integration

The full Quad migration still needs every original execution responsibility:

- Native serial IA/VS submission is now implemented and separately validated
  in [QUAD_SERIAL_MIGRATION.md](QUAD_SERIAL_MIGRATION.md). Its integration into
  the counter's private-output passes still remains.
- Final HS/DS/GS/SO primitive capture and bridge submission are now implemented
  and separately validated in [QUAD_FINAL_MIGRATION.md](QUAD_FINAL_MIGRATION.md).
  Their integration into the counter executor still remains.
- Target/view matching, selected subresources, buffer/3D adapters, dummy output
  allocation and full sample-storage fingerprints are now implemented and
  separately validated in [QUAD_RESOURCE_MIGRATION.md](QUAD_RESOURCE_MIGRATION.md).
  Their integration into the counter executor still remains.
- Private pre-raster UAV resources/counters, fresh isolation for each pass,
  exception-safe high UAV restoration, original SO suspension and predicate
  isolation. Original draw submission still occurs exactly once.
- Private depth preparation, copied stencil, per-sample digests and high-UAV
  restoration are separately migrated in [QUAD_DEPTH_MIGRATION.md](QUAD_DEPTH_MIGRATION.md).
  Depth strategy integration, counter/reference shaders, uint32 binary artifacts,
  histogram accounting and the original report/preview palette remain pending.
- Qt depth/target/layer controls, real uint32 heatmap cells, reports, experiment
  settings, cancellation/stale-result handling and complete ZIP export, using
  the established compact GPA-style layout.

The recovered 64-iteration lock is an experimental grouping diagnostic. The
native migration must preserve its contention and scheduling limitations and
must not relabel its output as physical quad invocations or GPA feature parity.
