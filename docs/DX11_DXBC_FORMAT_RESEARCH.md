# Independent DXBC operand framing

Date: 2026-10-24. This removes the old operand-count table's dependency on a
RenderDoc-derived reference. It does not implement shader execution or close the
GPA debugger-engine investigation.

## Sources and reproducibility

The replacement uses scalar format facts from two pinned Microsoft sources:

- [DirectX-Headers token format](https://github.com/microsoft/DirectX-Headers/blob/461ef5138bd30922b44fbad77c4e47662812d2d5/include/directx/D3D12TokenizedProgramFormat.hpp)
  defines opcode numbers and identifies custom declaration layouts. File SHA-256:
  `5862ef7c8b819f0d02ba73563a9df4dc0e89f11fe59d8f598def39122273c626`.
- [DirectXShaderCompiler ShaderBinary metadata](https://github.com/microsoft/DirectXShaderCompiler/blob/3620b16697cf57313d7a3abdb7a1c3aa3838c35c/projects/dxilconv/lib/ShaderBinary/ShaderBinary.cpp)
  lists instruction operand counts. File SHA-256:
  `0558a390ce2de0d76fc072893f15501d36e39abf379ff0611ead3b515ca7b63d`.

`generate_dxbc_operand_arities.py` accepts only these exact source bytes, extracts
opcode numbers and operand counts, and rejects duplicate or unfamiliar metadata.
Declarations, custom-data and reserved tokens remain outside the generic framing
table. All 190 previously accepted opcode identities remain present. The generated
header includes mnemonic comments for review. No upstream parser implementation,
runtime or header is linked, copied into the product or downloaded by its build.
Original research inputs remain under ignored artifacts.

```powershell
python tools/generate_dxbc_operand_arities.py `
  --shader-binary artifacts/dx11-native/microsoft-dxbc-format/ShaderBinary.cpp `
  --token-format artifacts/dx11-native/microsoft-dxbc-format/D3D12TokenizedProgramFormat.hpp `
  --out src/application/DxbcOperandArities.h --check
```

Omit `--check` to regenerate. Obtain the two pinned files from the links above as
an explicit research step; ordinary builds and tests use only the checked-in
header. The script never accesses the network or a legacy reference checkout.

## Independently reproduced discrepancy

Comparing the independently extracted facts with the previous table identified
three missing clamp operands:

| Opcode | Instruction | Previous count | Compiler-emitted count |
| --- | --- | --- | --- |
| 231 | sample_b_cl_s | 6 | 7 |
| 232 | sample_d_cl_s | 7 | 8 |
| 233 | sample_c_cl_s | 6 | 7 |

The new probe compiles PS 5.0 HLSL with dynamic clamp and bias/gradient/comparison
inputs, a nonzero texture offset and a consumed residency-status result. It walks
self-contained operand tokens to the instruction boundary, independently of the
count table. All three actual compiler rows contain the extra operand. The old
checkpoint parser rejects those valid rows with an instruction-layout error;
the original three failures and token arrays are retained. The replacement parses
the rows completely and still rejects truncated operand payloads.

The probe also compiles a VS SampleGrad case with VertexID, applies the existing
system-ID offset and output-log transforms, and checks that the sampling tokens
are preserved exactly. D3DDisassemble validates the compiled instruction names and
the transformed shader layout. This is a framing/instrumentation probe, not a
test of texture sampling execution, tiled-resource residency or a CPU interpreter.

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcFormatTests FloraDxbcLogTests FloraCheckpointTests FloraPostTransformTests FloraNativeVertexInputsTests
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(dxbc_format|dxbc_output_log|checkpoint_model|post_transform|native_vertex_inputs)$' --output-on-failure
```

All five focused CTest executables pass in 34.77 seconds. The Qt case counts are
6 format probes, 54 native VS inputs, 15 checkpoint-model cases, 6 output-log cases
and 13 PostTransform cases: **94 passed, zero failures and zero skips**. The two
stdout-only suites were rerun with explicit log files to retain their case-level
results. A clean capture-only configure and its source-boundary test also pass;
that configuration does not register the replay DXBC test. This is not a complete
capture-only release build.

The evidence ledger is `research/dx11-dxbc-format-evidence.json`. Existing
declaration/interface handling and checkpoint restrictions remain distinct from
the scalar table; an operand count is not proof of execution semantics. Full
VS/PS/CS native debugging, Ghidra debugger-engine closure and the removal of other
RenderDoc dependencies remain open.
