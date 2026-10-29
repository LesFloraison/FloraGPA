# Native DXBC texture feedback

## Scope and implementation

This increment extends the independent DXBC executor to texture feedback opcodes
219–225 and 228–229: gather, comparison gather, programmable-offset variants,
texture and multisample loads, typed UAV loads, explicit-level sampling and
level-zero comparison sampling. Buffer feedback and opaque status propagation
remain covered by the [preceding study](DX11_NATIVE_FEEDBACK_STATUS.md).

The data operation uses the existing native resource model and GPU assistance.
The extra destination receives semantic mapping knowledge, never fabricated
hardware status bits. A texture snapshot records both captured and actual native
resource MiscFlags before private copies. Only two non-tiled descriptors establish
fully mapped status. Otherwise status remains unknown, including null bindings;
helper-lane UAV suppression retains unknown results. Snapshot identity advances
to `independent-replay-resource-inputs/12` and analysis identity to
`dx11-native/5`, invalidating earlier analysis caches.

The [Microsoft D3D11.3 functional specification, section 5.9.4.5.5](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
separates operations offering feedback alone from sampling operations that also
accept an LOD clamp. It specifies fully mapped accesses for non-tiled resources.
This implementation does not normalize away clamp operands: opcodes 230–233
remain outside this increment.

## Compiler framing evidence

The locally compiled `sample_l_s` and `sample_c_lz_s` instructions contain six
operands followed by one zero DWORD. The pinned Microsoft opcode metadata in the
[format study](../DX11_DXBC_FORMAT_RESEARCH.md) also specifies six operands.
The observed extra word is treated as a narrow compiler-format compatibility
case, not as an additional shader operand or a recovered hardware requirement.
The local system D3DCompiler version inspected during this run was
`10.0.26100.9711`.

The shared parser checks the opcode, the complete six-operand boundary, the SRV
and sampler operand kinds, and exactly one trailing zero. Other instruction
tails retain their existing validation. Tests require rejection of nonzero
padding, extra padding and a malformed SRV kind. They also accept a correctly
framed unpadded instruction. Existing SAMPLE_POS compatibility stays covered.
The first execution run rejected all 36 explicit-level/comparison-level rows;
that failed evidence is retained. Initial negative-test fixtures also exposed a
C++ declaration/expression ambiguity and were corrected without changing their
expected rejection conditions.

## Reproduction and evidence

Build `FloraDxbcTextureFeedbackTests`, `FloraDxbcMultisampleTests`,
`FloraDxbcTextureUavTests`, `FloraDxbcTextureInputTests` and
`FloraDxbcFormatTests` in Release. Run GPU suites serially. Set
`FLORA_TEXTURE_FEEDBACK_EVIDENCE` to a new output directory when running the first
suite. Its reports contain actual adapter identities and shader bytecode; then
run:

```text
python tools/validate_native_texture_feedback.py --root <evidence-directory> --out <report.json>
```

The matrix contains 126 cases: WARP, NVIDIA and Intel; PS, VS and CS; seven
operations; known non-tiled versus deliberately unknown source provenance.
Every case compares native data, the original feedback shader and an independently
compiled ordinary shader. The Python verifier additionally computes the expected
point-sampling, comparison and gather values from the fixture's texels, verifies
42 shader identities across adapters/provenance, and checks all operand framing.
Unknown provenance must leave the final status-dependent component invalid.

The existing MSAA matrix additionally compares feedback and ordinary reads for
all tested stage/sample/address/offset/mask combinations, including invalid sample
indices. The typed UAV matrix executes a separate feedback program against a
fresh snapshot, checks data against the original replay, verifies status knowledge
and confirms that diagnostic writes did not alter original resources. Texture
input tests check both descriptor provenance fields.

The [evidence manifest](dx11-native-texture-feedback-evidence.json) records
implementation commit `0f02c3896b4abc9a592bb5fda62b25c7bc0925be`,
1,823 artifact hashes and 23 committed source-file hashes. Twelve suites passed
3,393 tests with zero failures or skips:

| Suite | Passed |
| --- | ---: |
| Texture feedback, including parser rejection tests | 134 |
| MSAA | 105 |
| Typed texture UAV | 765 |
| Texture inputs | 76 |
| DXBC format | 16 |
| DXBC execution | 1,212 |
| Input snapshots | 82 |
| Native compute debug | 68 |
| Native analysis | 46 |
| Explicit-level sampling | 156 |
| Comparison sampling | 190 |
| Gather | 543 |

The GUI, CLI and Worker Release builds succeeded. This is an incremental
development build, not the clean standalone release acceptance build.
Local shader binaries, complete logs and reports
remain under the ignored `artifacts/dx11-native/texture-feedback-*` paths.

## Remaining acceptance gates

This is synthetic shader/resource validation, not proof of original captured
invocation selection. Tiled/partially mapped resources, clamp feedback, null data
destinations, remaining precision combinations and full stage/source debugger
parity remain open. The two NVIDIA buffer-feedback oracle failures documented in
the preceding studies are unchanged; texture success does not waive them.

The GPA debugger host object's engine identity, full Pixel History, geometry
parity, removal of the RenderDoc backend and standalone release acceptance remain
open. No RenderDoc dependency has been deleted by this increment.
