# Native DXBC feedback with discarded data

Implementation: `f1073c18e2d36d0adc45bcd85e88f5c291962b3f`.
Analysis identity: `dx11-native/8`; GPU helper identity remains `dxbc-gpu/29`.

The independent executor now accepts a null data destination on resource-feedback
instructions. This is an actual compiler-generated form: a compound structured
load can discard unused fields while retaining their mapping status, and a
texture shader can consume only `CheckAccessFullyMapped`. Previously these
programs failed native destination validation.

## Execution contract

Normalization removes the separate feedback-status operand as before, then marks
a validated null data destination as discarded. The ordinary load or sampling
path still validates bindings, operands and resources and performs its resource
operation. Its data result does not create a temporary register or a trace write.
Feedback provenance is produced through the existing mapping path. Both data and
status may be discarded. Malformed null destinations and non-feedback loads with
null destinations remain rejected; the internal discard marker cannot be supplied
by shader operand bits.

Known non-tiled provenance produces a mapped status. Unknown provenance stays
unknown, including after `check_access_fully_mapped` and output arithmetic. Tests
compare only valid output bits. No opaque status encoding is invented. Backend
version 8 separates the new analysis behavior from earlier cached jobs; trace
format, GPU helper behavior and adapter selection are unchanged.

No RenderDoc or Intel GPA implementation is imported. The diagnostic shaders in
this study are compiled from owned HLSL. Here, "original shader" means the
unmodified compiler output, not a shader extracted from Intel GPA.

## Independent evidence

Nine buffer cases cover two-, three- and four-vector structures on WARP, NVIDIA
and Intel. Only the first vector contributes data, while every vector contributes
feedback. The compiler emits one, two or three null data loads respectively.
Each case executes the compiler shader and a separately constructed raw-feedback
OR variant on the GPU. All 18 GPU outputs match independently calculated integer
values and non-tiled mapping status. Native execution passes both known and
unknown mapping modes. A raw-load fixture additionally covers four mapping/status-discard
combinations and 28 repeated malformed or non-feedback rejection checks.

The texture matrix has 126 cases: three adapters, VS/PS/CS, seven operations and
two provenance modes. It covers ordinary texture loads, SampleLevel,
SampleCmpLevelZero and four gather variants. Every native case produces the
expected constant data and mapping validity without a null-register trace write.
For every case, two separate shaders retain observable sampled data: the feedback
and ordinary versions agree with independently calculated texels/comparison
results. The constant-output ordinary control also passes.

Unmodified null-output shader execution succeeds in 114 texture cases. Twelve
Intel cases fail during original shader creation: SampleLevel and
SampleCmpLevelZero in each of VS/PS/CS, repeated for the two native provenance
settings. These failures remain failing test assertions.

An isolated creation probe uses a fresh feature-level 11.1 Intel device for each
variant, without dispatching or drawing. Across six stage/operation combinations:

| Variant | Shader creation | Device removed reason |
| --- | --- | --- |
| Unmodified null-output compiler shader | `0x887a0005` in all six | `0x887a0020` in all six |
| Same null-output shader with compiler padding removed | `0x887a0005` in all six | `0x887a0020` in all six |
| Compiler shader retaining observable sampled data | Success in all six | Success in all six |

The recorded reason is `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. Removing instruction
padding does not resolve it. This isolates the failure from native execution and
GPU completion waits; it does not establish the internal cause in the driver.
The observed Intel device is vendor 32902, device 32103, LUID 74756, driver
32.0.101.8826. WARP and NVIDIA execute the original null-output variants. Test
oracles explicitly request feature level 11.1; product devices are not upgraded.

## Final validation

GUI, CLI, Worker and six Release test targets build. GPU suites run serially.
The final Qt result is **1,682 passed, 12 failed, zero skipped**:

| Suite | Passed | Failed |
| --- | ---: | ---: |
| DXBC execution | 1,232 | 0 |
| Execution trace export | 22 | 0 |
| Execution trace reader | 65 | 0 |
| Native compute debugging | 68 | 0 |
| Native analysis | 46 | 0 |
| Texture feedback, including null forms and creation observations | 249 | 12 |

The original aggregation validator passes nine cases and rejects 45 corruptions.
The existing texture validator passes all 126 original data-observing matrix
cases. The new null-feedback validator verifies nine buffer cases, 126 native
texture cases, all positive controls and the isolated creation evidence, and
rejects 687 corrupted observations. It returns **exit code 1**, with status
`blocked-by-original-shader-creation`, because all original shader variants have
not passed. No vendor waiver, changed golden or skipped assertion is introduced.
Creation observations are not counted as successful original null-shader execution.

Final evidence is under `artifacts/dx11-native/null-feedback-release*`. The evidence
ledger pins local sources, compiled executables, DXBC, reports and completed logs.
Earlier `initial` and `controls` runs remain supplementary. The final build log is
`null-feedback-release-final-build.log`; the earlier release build preceded the
final internal discard-marker hardening. No compiled source changed during GPU
execution, and logs were read only after their respective processes completed.

## Reproduction and remaining scope

```powershell
$env:FLORA_FEEDBACK_NULL_BUFFER_EVIDENCE = '<new-buffer-root>'
build/vs2022/Release/FloraDxbcExecutionTests.exe nullRawFeedback nullBufferData -o <new-buffer-log>,txt
$env:FLORA_TEXTURE_NULL_FEEDBACK_EVIDENCE = '<new-texture-root>'
$env:FLORA_TEXTURE_NULL_CREATION_EVIDENCE = '<new-creation-root>'
build/vs2022/Release/FloraDxbcTextureFeedbackTests.exe nullShaderCreation nullTextureData -o <new-texture-log>,txt
python tools/validate_native_null_feedback.py --buffers <new-buffer-root> --textures <new-texture-root> --creation <new-creation-root> --texture-log <new-texture-log> --out <new-ledger.json>
```

The final two commands are expected to fail on the recorded Intel driver. The
verifier requires every original result or matching creation failure to be
accounted for; a positive control does not replace a missing original result.

This study does not independently qualify null MSAA loads, typed UAV loads or the
four LOD-clamp forms, although they use the shared execution paths. Actual tiled
resources, partial residency and hardware unmapped feedback remain unqualified.
The earlier NVIDIA feedback-status and Intel clamp-effect conflicts are separate
open observations, not resolved by these results. Original fragment qualification,
public PS selection, source parity, complete dependency removal and standalone
release acceptance remain open. No portable package or full debugger parity is
claimed.
