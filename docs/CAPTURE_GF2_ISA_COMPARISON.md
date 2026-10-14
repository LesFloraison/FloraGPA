# GF2 original and unused-output ISA comparison

Original GPA exports different render-target message encodings for the original
PS and the count-only PS, even though the DXBC transformer preserves every
original color instruction. The modified shader changes control flow and the
ordering/encoding of render writes. This explains why its observations cannot
be treated as an undisturbed measurement of the original shader's execution; it
does not establish the cause of the three-pixel difference. The formal six-frame
gate remains **failed** and no production rendering change is made.

## Exact diagnostic derivative

After recovering space from regenerable build intermediates, a regular file
copy succeeds. The retained source frame is SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
The derivative is
`D:/CDXrepo/FloraGPA-Cpp/artifacts/capture-gf2-isa-comparison-20261014/count-output.gpa_frame`,
SHA-256 `49ec1932269370ca800a9dea89473549a9e520c8b22aaa2ddcd6af41b61eb097`.

The new file appends replacement data for shader resource 889 / data record 1785
and a new index. All original records except that shader data compare byte for
byte. The original body, preview, descriptors, resources, states and events are
preserved. Only the index and empty-pool header offsets change, plus the appended
data/index. This changes every use of PS 889, unlike the prior scoped single-Draw
override. The derivative is explicitly diagnostic and is never counted as an
accepted capture or a full-frame equivalence result.

The source PS SHA-256 is
`fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95`;
the changed PS is
`36e81f84589bff2faedefa6aaf92befc50d62cbb1532b6e02bba69e20f0f5199`.
Removing exactly the added output declaration and constant write restores every
original executable instruction word. Shader 1783, which has the same original
DXBC but belongs to a different resource, remains unchanged.

## Original GPA exports

Three successful analyzer processes run serially: one fresh original-frame
control and two fresh derivative exports. The original control matches both
previous original ISA exports byte for byte. Both modified exports agree with
each other. The selected VS ISA is unchanged in every run.

| Selected stage | Original UTF-8 bytes / lines | Modified bytes / lines |
| --- | ---: | ---: |
| PS | 86,917 / 890 | 87,782 / 900 |
| VS | 21,871 / 228 | 21,871 / 228 |

Original PS ISA SHA-256:
`dc9bca5c2d7bd649cf06b2575c02829fcac051f11df822fbc0aca479ce8093a5`.
Modified PS ISA SHA-256:
`71cc3c3ee94b9be272e3fb374e1636f3b25396ed55c790012dc060a74ab42a2a`.
Unchanged VS ISA SHA-256:
`3c9c3aa5c5ad33ab855796d49305aee0e12300b599f199d3be189f4098ce4796`.

The selected shader remains original GPA program 57, associated with original
event 1761 / Flora DrawIndexed 5651 by the preceding full action-sequence
comparison. Exact VS/PS disassembly hashes select it in the derivative. Two
initial requests incorrectly expected two modified programs and fail before
requesting ISA. The original frame has programs 56 and 57 with equal shader
bytes, but only program 57 uses the replaced data. Corrected requests require
exactly one match. Both failed reports and their exit-zero process results are
retained; process exit zero alone is not export success.

## Descriptor evidence

The small CPU decoder uses fields from Intel's
[legacy RC decoder](https://github.com/intel/intel-graphics-compiler/blob/c189f01356d4d887b47a4ae72ad770ec086824a9/visa/iga/IGALibrary/Backend/Messages/MessageDecoderOther.cpp)
(`tryDecodeRC`, before the Xe2 path). Message type is bits 14:17, FP32/FP16 is
bit 30, subtype is bits 8:10, last-render-target is bit 12, and binding-table
index is bits 0:7. It accepts only the observed SIMD8/M0 FP32 write forms.

| Shader | Write order: binding-table indices | Descriptor / subtype | Last target |
| --- | --- | --- | --- |
| Original | 1 | `0x10030201` / `.lo8ds` | No |
| Original | 2 | `0x040b1402` / `.simd8` | Yes |
| Modified | 2 | `0x040b0402` / `.simd8` | No |
| Modified | 4 | `0x040b0404` / `.simd8` | No |
| Modified | 1 | `0x08031401` / `.simd8` | Yes |

Binding-table indices are reported as encoded; they are not independently
validated resource IDs or RTV slot numbers. The observed message format also
does not establish that application blend factors enable dual-source blending.
Both shaders still use FP32 render-write payloads in these exports. The first
conditional branch changes predicate form, and the original nearby `else` is
absent from the modified export. Later register assignments and scheduling also
differ. This is not an instruction-equivalence proof or a reconstruction of
fragment values, and it is not a trace of the ISA executed by Flora or the game.

## Validation, failures and storage

`tools/inspect_gpa_render_messages.py` reads exported ISA only and writes a new
JSON file without overwriting an existing one. Six CPU tests cover observed
descriptor words, comments, malformed/unsupported forms and descriptor kinds.
All pass, as does the source-boundary CTest. No production source/binary changes
or new golden-frame rendering claim are involved. The ordinary replay hash is
still `215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.

A prior sparse-file attempt deliberately zeroed large resource payloads. Its
readback verification hit WinError 112 before completing; it never entered GPA
and is excluded. The incomplete generated derivative was removed with its
length, allocation and failure recorded; its full hash could not be read. The
successful comparison uses the exact ordinary copy described above, not any
zeroed-resource fixture. No inference about the Windows storage failure's
underlying cause is made.

The bounded cleanup removes 169 regenerable `.ilk` files and Debug `.obj` files
from the current repository's build tree, totaling 1,498,493,087 bytes. No build
process was running; every resolved path was checked against that build root.
Release objects, executables, DLLs, symbols and capture evidence are retained.
The cleanup list is saved, and subsequent Debug builds will regenerate objects.

Raw source excerpts, scripts, reports, ISA text and the diff are outside Git in
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-sparse-isa-20261014` (the directory
name retains the first failed approach). The
[baseline](capture-gf2-isa-comparison-baseline.json) pins inputs, source identities,
successful/failed exports and the evidence manifest. The next discriminating
experiment can isolate render-write selection while retaining the original
color calculations; no shader rewrite is proposed as a production capture fix.
