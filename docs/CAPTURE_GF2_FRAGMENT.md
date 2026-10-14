# GF2 fragment input and unblended output controls

This continues the [single-triangle reduction](CAPTURE_GF2_PRIMITIVE.md). The
production capture runtime, format, encoder and replay renderer are unchanged.
The current-runtime six-frame acceptance remains incomplete, with exact-byte
comparison unchanged.

## Float observation tools

The standalone native fixture has an explicit `--float-rt0` diagnostic mode. It
requires a selected pixel in RTV0 and independent blending, creates a private
RGBA32F MSAA target with the original dimensions/sample descriptor, clears it,
and disables blending only for output zero. Original VS/PS bytecode, input
resources and other state remain in use. Every original output's initial samples
are still restored and verified before Draw. The report records the transform,
complete float sample hashes and the selected pixel's four raw 32-bit words.
The private target replaces the original RTV0; its original-resource comparisons
are intentionally not equivalent full-frame checks.

The excluded development target `FloraReplayFragmentProbe` performs the same
output transformation immediately before the selected Draw in a normal FloraGPA replay
prefix. An optional diagnostic PS is installed only at that boundary, so earlier
uses of the same captured shader remain original. It records prefix and scoped
debug messages separately and fails on scoped warnings/errors. Its private
integer-view readback does not need a new public Replay API.

```text
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 3 --indices 4839 3 --pixel 618 1562 1492 --float-rt0 --summary-only
FloraReplayFragmentProbe FRAME NEW_OUT 5598 618 1562 1492 32902 [DIAGNOSTIC_PS_DXBC]
```

These modes change output format, blending and resource identity. Their execution
can differ from the original half-float path. They are observation experiments,
not a capture fix or accepted replay result.

## Original shader output

Three independent-fixture repetitions of triangle 1613, one full native Draw and
four fresh full-prefix replay processes give the same sample-0 shader output at
(1562, 1492):

| Component | Raw uint32 | Float32 |
| --- | --- | --- |
| R | 1061556244 | 0.7736828327 |
| G | 1055032779 | 0.4424270093 |
| B | 1049847438 | 0.2878918052 |
| A | 1062752239 | 0.8449696898 |

The full native Draw and all four full-prefix replay processes agree on both
complete RGBA32F sample planes, not only this pixel. The isolated triangle leaves
sample 1 at the clear value. A separate skip-Draw control leaves both complete
private planes zero, proving that the observed float data require execution.

For the observed lower variant, converting the source output to half precision
first and applying the captured SRC_ALPHA / INV_SRC_ALPHA blend to the exact
initial destination reproduces all four stored half words:
`[14698, 13854, 13265, 15091]`. Performing this reconstruction directly from
unquantized float32 differs by one half-float step in three components. The
recorded development calculation is an exploratory reconstruction, not a newly
chosen acceptance rule. This is a consistency check for the lower
variant, not proof that the higher variant cannot originate in any particular
part of the original execution path.

Subsequent [original-format GPU controls](CAPTURE_GF2_HALF_SOURCE.md) show that
the stored unblended half values do not uniquely determine the blend input:
two constant sources store the same half words but blend differently. The CPU
reconstruction above must not be read as the hardware's rounding sequence.

## Interpolated input controls

Six diagnostic pixel shaders select original primitive 1613 and output one input:
TEXCOORD0.xy, TEXCOORD1, TEXCOORD2, TEXCOORD3, TEXCOORD6, or SV_IsFrontFace. They
retain the full VS/PS linkage signature, but replace the original PS calculations.
The same bytecode runs in the full native Draw and at the full replay boundary.
All nineteen used scalar components match bit for bit across the two paths;
padding components are recorded separately as zero in the four-word outputs.
The front-face value is true. All six pairs have clean scoped debug queues.

The first simplified probe omitted unused signature entries and failed native
VS/PS linkage validation (debug message 343). That run is explicitly incomplete
and excluded. A second source attempt failed HLSL compilation because of
system-value signature ordering. Both failures, source, logs and partial output
remain available. Corrected signatures precede the accepted comparisons.

This establishes input agreement only in the instrumented executions. It does
not establish that inputs remain unchanged during a higher-variant execution,
nor preserve the original shader's register allocation, derivatives or driver
optimization decisions. No higher variant was observed by these float/input
controls; the original half-float discrepancy remains unresolved.

The full replay prefix has 608 existing debug messages: 303 error-level notices
about a conditional comparison-resource format, 303 error-level notices about a
conditional comparison sampler, and two warnings about an unbound output. Their
messages explicitly condition invalid behavior on whether the shader uses those
bindings. They are retained without asserting that the branches execute or that
the prefix is clean. The selected observation scopes report zero messages.

## Original GPA GUI observation

Using the `computer-use` skill, the original Graphics Frame Analyzer 2025 R1
opened current-runtime equipment-scene file
`frame-23c72ea8-d6f0-4557-96e7-48513d0f27bd.gpa_frame` on Intel Iris Xe. The loaded
analysis displayed 637 events, a GPU-duration chart and the final current RTV0
image at 2560x1440 in R8G8B8A8_UNORM_SRGB. The screenshot and exact file/tool
identities are pinned in the baseline. The capture browser's resolution/host/GPU
metadata fields were unavailable, although the loaded texture had its format and
dimensions. This observation is separate from the variable frame used above.

Further event-filter interaction timed out twice, including a retry after
refreshing the returned window binding. Shader, selected Draw and non-final
resource panels were therefore not inspected and are not marked accepted. The
read-only GPA instance launched for this check and its satellite exited before
the final golden regressions. This is partial GUI evidence for one file, not
six-file GUI acceptance, a pixel-exact export comparison, or a completed gate.

## Verification

Nineteen positive diagnostic processes comprise nine native processes (eleven
restore/observation iterations, including one skip-Draw control) and ten replay
processes. All 88 native original-output restoration comparisons are exact;
scoped debug queues are clean. Thirteen refusal controls, four boundary/format
CTests, capture-only configuration, both native builds, and serial GF2/BF1
golden-frame comparisons pass. Production binary hashes are unchanged.

The [baseline](capture-gf2-fragment-baseline.json) records the distinct scopes,
reports, shader hashes, failed probes, arithmetic reconstruction, GUI evidence,
tool identities and an evidence manifest. Raw files are preserved outside Git at
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-fragment-20261014`.
Independent capture delivery, the current six-frame gate, complete original-GPA
inspection and the higher-variant cause remain unfinished.
