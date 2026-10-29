# Native DXBC feedback aggregation

The independent executor now preserves opaque mapping provenance through DXBC
OR aggregation. Previously, OR cleared the provenance, so a subsequent
`check_access_fully_mapped` rejected the result. Ordinary integer OR is unchanged.
The analysis identity advances to `dx11-native/7`; the GPU helper remains
`dxbc-gpu/29`. No RenderDoc or GPA code is imported or loaded by this implementation.

Implementation and reproducible validator:
`3284d9eea61334d085afcdd0b2dec94ac63af8e5`.

## Semantics and scope

The [Microsoft D3D11.3 functional specification, section 5.9.4.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
describes OR aggregation of feedback from constituent resource accesses. Mapping
tokens remain opaque. Two mapped accesses aggregate to mapped; any known unmapped
access makes the aggregate unmapped; otherwise it is unknown. OR with a known
integer zero preserves a token by bitwise identity. Unknown zero bits and arbitrary
nonzero integers do not acquire mapping provenance. No raw status bits are guessed.

The destination retains raw-bit validity separately from semantic mapping status.
Opaque components keep invalid raw bits with zero placeholders. Existing source
modifiers, minimum-precision rejection and trace handling remain in force. Masked
writes preserve untouched components, including when the destination aliases an
input. Reverse application of step changes restores the preceding register state.

The actual local compiler lowers the owned composite `StructuredBuffer.Load`
examples into individual mapping checks and Boolean AND, in both ordinary and
preserved-optimization exploratory builds. Therefore these examples do not prove
that this compiler emits raw feedback OR. The GPU tests retain its unmodified
shader as a control, and separately construct the specified aggregation form:
mapping checks become token copies, Boolean conjunctions become raw OR, and one
mapping check follows the final aggregation. Existing declarations, resource
accesses and data arithmetic are retained.

## Independent checks

Nine cases cover two-, three- and four-vector structures on WARP, NVIDIA and
Intel, explicitly creating feature-level 11.1 test devices. Every case executes
both the original compiler shader and the constructed aggregation shader. All
18 GPU results agree with independent integer sums and the expected non-tiled
mapping result. Native execution runs with both known non-tiled and unknown
mapping provenance: data stays exact, while unknown mapping results stay invalid.
The original input buffers remain unchanged in the native runs.

The separate executor fixture covers 20 combinations of mapping provenance,
destination masks, aliasing, operand order, known zero, nonzero and unknown zero.
It checks preserved opaque tags, raw-bit validity and reverse step changes.
It does not inject opaque values into external shader inputs: those inputs must
remain ordinary values. Feedback is produced by actual native load instructions.

`tools/validate_native_feedback_aggregation.py` independently parses both DXBC
containers, counts load/OR/check instructions, checks hashes, recomputes expected
values and validity, and rejects corrupted observations. Its matrix requires
nine cases, six shader variants and both native provenance modes per case.

Final Release builds of the GUI, CLI, Worker and five test targets pass. The
serial regression run passes 1,423 Qt cases with zero failures or skips:

| Suite | Passed |
| --- | ---: |
| DXBC execution, including the new aggregation cases | 1,222 |
| Execution trace export | 22 |
| Execution trace reader | 65 |
| Native compute debugging | 68 |
| Native analysis | 46 |

The independent aggregate verifier passes all nine final cases and rejects 45
corrupted reports. Final evidence is under
`artifacts/dx11-native/feedback-aggregate-release*`; earlier `owned` observations
are supplementary and are not added to those counts. Backend-version assertions
in the Worker test and current compute validators advance to version 7 without
changing value, validity or resource assertions.

## Reproduction and retained limitations

```powershell
$env:FLORA_FEEDBACK_AGGREGATE_EVIDENCE = '<new-root>'
build/vs2022/Release/FloraDxbcExecutionTests.exe feedbackStatusAggregation compiledFeedbackAggregation -o <new-log>,txt
python tools/validate_native_feedback_aggregation.py <new-root> --out <new-ledger.json>
```

All GPU suites run serially. Each independent shader dispatch has a five-second
completion-query deadline. The evidence ledger pins final sources, artifacts and
binaries. Preliminary logs are retained, including an invalid initial attempt
to seed temporary registers, the incorrect compiler-OR expectation, a temporary
DXBC span-lifetime error, feature-level 11.0 rejection and a stale test variable
after expanding the unknown-zero cases. Corrected fixtures keep the original
controls and independent value assertions; no historical golden is changed.

Actual tiled resources, partial residency and hardware-produced unmapped tokens
are not qualified here. The unmapped aggregation rule is specification-derived,
not a claim of a successful tiled-resource GPU probe. A separate exploratory
partial-structure shader also exposes the still-open null data destination case;
this change does not claim to resolve it. Original fragment qualification, public
PS selection, complete debugger parity and P7/P8 removal/release remain open.
