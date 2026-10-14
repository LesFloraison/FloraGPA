# GF2 shader ISA export through original GPA

The installed GPA Plugin API provides ISA text for the shader pair at Flora
DrawIndexed 5651. Two independent Frame Analyzer processes export identical
text. This establishes a working diagnostic route after the preceding IGC
environment-variable attempt produced no files. It does **not** resolve the
three-pixel mismatch or pass the formal six-frame gate.

## Shader and event identity

The input remains the retained frame SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
Comparing the complete ordered Draw/Dispatch action sequence, including argument
values, maps Flora event 5651 to original GPA event 1761: DrawIndexed(7401, 0, 0).
The retained original export binds program 57 at this event. Programs 56 and 57
both have exactly the expected VS 888 and PS 889 DXBC disassembly. The exporter
selects by both complete disassembly hashes, with only line endings and trailing
NUL normalized, and requires exactly two matching programs. It does not assume
that Flora resource/event IDs equal GPA IDs.

`tools/gpa_shader_isa_audit.py` is a development-only plugin source, installed as
`flora_shader_audit/__init__.py` in the user's GPA plugin directory for this
experiment. It calls the installed `Program.get_il_source(stage, "isa", 30000)`
for vertex and pixel stages of each matching program. The installed API source
and analyzer/player binaries are hashed in the raw evidence. No private address
is called, and no application target imports this script or links GPA.

## Results and limits

Both analyzer processes exit with code zero. All eight API requests return
nonempty instruction text. Program 56 and 57 return the same text within each
process, and the second process agrees byte for byte:

| Stage | UTF-8 bytes | Lines | SHA-256 |
| --- | ---: | ---: | --- |
| Pixel | 86,917 | 890 | `dc9bca5c2d7bd649cf06b2575c02829fcac051f11df822fbc0aca479ce8093a5` |
| Vertex | 21,871 | 228 | `3c9c3aa5c5ad33ab855796d49305aee0e12300b599f199d3be189f4098ce4796` |

The text contains driver-generated Intel instructions, including sampler and
render-target messages in the pixel shader. Raw source text stays outside Git.
These are GPA-provided ISA exports, not a capture of Flora's executing shader
machine code. The experiment does not establish which compiled variant ran in
the game, either final-color outcome, or a prior instrumented replay. There is
no simultaneous fragment-value or framebuffer measurement here. Two equal
exports do not prove compiler determinism for other pipeline states or processes.

Intel documents shader assembly inspection in its
[Graphics Frame Analyzer guide](https://www.intel.com/content/www/us/en/docs/gpa/user-guide/2023-2/optimize-shaders.html).
The callable interface used here was verified against the locally installed
`python_plugins/plugin_api/program.py`; the earlier general IGC dump attempt is
separate evidence.

## Reproduction and evidence

The plugin accepts an existing fresh audit directory containing `request.json`:
the two expected normalized disassembly SHA-256 values and
`expected_matching_programs: 2`. It refuses an existing report, saves progress
before each request, and records individual API failures without counting them
as successful ISA exports. Reports explicitly retain
`acceptance_established: false`.

```
FrameAnalyzer.exe --file_to_open FRAME --py_plugin flora_shader_audit --py_arg AUDIT_DIRECTORY --output_log NEW_LOG
```

Executions were serial. The retained full action sequence, expected DXBC
disassemblies, both exported texts, program/event mapping and file hashes are
independently checked by the development evidence summarizer. No production
source or binary changed, so the preceding stage's module-boundary and texel
regressions remain applicable; no additional GPU regression claim is made.

Raw evidence is in
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-isa-audit-20261014`.
The [baseline](capture-gf2-isa-audit-baseline.json) pins the commands, source,
identities, exports and manifest. The current exact comparison rule and legacy
migration parity remain unchanged.
