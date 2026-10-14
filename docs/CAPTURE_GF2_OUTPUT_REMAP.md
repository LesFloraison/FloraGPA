# GF2 output-slot remap diagnostic

Moving the second PS output from slot 1 to slot 3, together with its original
RTV and blend descriptor, gives capture-matching images in three diagnostic
replays. Three interleaved original-shader recreations give one matching image
and two known alternate images. The output count and arithmetic instructions
remain unchanged. Original GPA exports change from one `lo8ds` and one `simd8`
render write to two `simd8` writes. This narrows the preceding added-output
experiment; it does not establish deterministic behavior, the executed machine
code, or a production fix. The formal six-frame gate remains **failed**.

## Scoped replay experiment

`FloraReplayTexelProbe output-remap` accepts only the previously pinned PS:
`fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95`.
Both original and changed DXBC are 13,328 bytes. The changed SHA-256 is
`05380474f3b2dd8c2b9a0de8b9815b21b078ab26318206556b66c3850322a9ec`.
Independent byte comparison verifies exactly these chunk-word changes, all 1
to 3, plus the regenerated container checksum:

| Chunk | Byte offsets within chunk body | Meaning |
| --- | --- | --- |
| OSGN | 36, 48 | Second output semantic and register indices |
| SHDR | 380, 12748, 12828, 12848 | One output declaration and three write destinations |

All other chunk words, instruction lengths and input signatures are unchanged.
There are still two outputs. No calculation, constant, instruction, texture,
view or resource attachment is added. Before DrawIndexed 5651 the scope creates
the changed PS and blend state, moves RTV1 to previously vacant RTV3, and copies
the slot-1 blend descriptor to slot 3. RTV0, remaining RTVs, DSV, blend factors
and sample mask are retained. The actual bound pipeline is checked while the
scope is active; all original bindings are restored immediately after the Draw.
Unsupported predication, OM UAVs, PS classes, missing objects, occupied slot 3,
alpha-to-coverage, non-independent, logic and dual-source blending are rejected.
The helper is development-only and is not linked into capture or the application.

| Mode | Executions | Capture-matching | Known alternate |
| --- | ---: | ---: | ---: |
| Output/RTV/blend 1 moved to 3 | 3 | 3 | 0 |
| Original PS recreation | 3 | 1 | 2 |

The same frame, adapter, binary and event are used throughout. Independent PNG
decoding confirms the previously recorded matching/alternate hashes and the
same three differing pixels/channels. Selected pre-Draw half words remain
`[7759,8093,7731,15285]`; matching post-Draw words are
`[9127,9247,8891,15143]`, alternate words `[9109,9233,8865,15151]`.
All fifteen input-resource records, original native pipeline, depth state and
both selected depth samples equal the preceding verified observations. There
are zero warnings/errors within all six observed Draw scopes; the existing
608 prefix debug messages are recorded separately. Diagnostic copies, shader
creation and state changes still perturb execution timing. Reference pixels
are used only for comparison, and comparison rules remain unchanged.

## ISA-only derivative

A separate ordinary file copy replaces only shader data 1785, used by PS 889,
in place. All 6,395 records are compared and every byte outside that shader is
verified unchanged, including header, index, preview and other resources.
This derivative retains the original RTV/blend records and changes every use
of PS 889. It is solely an ISA export input: it does **not** encode the scoped
pipeline remap above and is not an image-equivalence or acceptance control.

Two independent original GPA processes select program 57 by exact VS/PS
disassembly hashes. All four ISA requests succeed with nonempty source. The PS
exports are byte-identical, 889 lines, SHA-256
`6cc8f6ab41b9b4944016784ecaacc2575cf7c3a0d4fb3bee46fa16c5d7e966e8`.
The VS retains the earlier hash
`3c9c3aa5c5ad33ab855796d49305aee0e12300b599f199d3be189f4098ce4796`.

| Shader | Write order / encoded binding-table index | Descriptor / subtype | Last target |
| --- | --- | --- | --- |
| Original, previous verified export | 1 | `0x10030201` / `lo8ds` | No |
| Original, previous verified export | 2 | `0x040b1402` / `simd8` | Yes |
| Remapped | 4 | `0x040b0404` / `simd8` | No |
| Remapped | 1 | `0x08031401` / `simd8` | Yes |

The decoder and interpretation limits are those in
[the preceding ISA comparison](CAPTURE_GF2_ISA_COMPARISON.md). Encoded
binding-table indices are not independently validated RTV IDs. This does not
prove application dual-source blending is enabled, isolate render writes from
all other compiler changes, or measure the code executed by Flora/the game.
The original ISA is a retained prior export, not a new control in this stage.

## Validation and retained failures

The new native oracle runs on WARP and Intel at feature level 11.1 with 2x/4x
MSAA. Independently compiled two-output shaders, distinct target blend rules,
two consecutive Draws and a partial sample mask produce 96 exact texel checks
across four cases. The tests verify the mapped pipeline, unchanged untouched
targets/samples, restored original pipeline, 44 explicit refusals and zero
debug warnings/errors. An initial refusal fixture used an invalid dual-source
blend descriptor and failed at device creation. Its report and binary are
retained. The corrected fixture uses a creatable descriptor and reaches the
helper's explicit refusal; it does not skip or accept the initial failure.

The existing texel validator passes 28 cases and 26 refusals. Source-boundary
CTest and capture-only configuration pass. GPU processes run serially. Both
native targets compile under `/W4 /WX`. No production Capture, Format or Replay
source changes; the production CLI retains SHA-256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.

```text
cmake --build --preset capture-full-release --target FloraOutputRemapTests FloraReplayTexelProbe
FloraOutputRemapTests NEW_REPORT
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 output-remap 5651 5651 7
```

Raw reports, frozen binaries, verification scripts and the diagnostic derivative
are retained outside Git under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-output-remap-20261014`.
The [baseline](capture-gf2-output-remap-baseline.json) pins source, frame, binary
and evidence identities. Legacy migration parity remains unchanged.
