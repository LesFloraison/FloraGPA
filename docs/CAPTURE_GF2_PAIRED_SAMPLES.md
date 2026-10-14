# Pairing GF2 output with frame-end MSAA samples

The exact six-frame acceptance gate remains failed. This stage establishes a
more specific diagnostic result: repeated executions of one retained failing
file can produce different final pixels together with a different value in one
MSAA texel. Two new diagnostic captures also expose intermediate differences
even when their final backbuffers match. Neither observation identifies a root
cause or supplies a production fix.

## Same-execution probe

`FloraReplayFinalSamplesProbe` executes a frame once, saves its executed final
RGBA output, reads every sample/layer of selected RGBA8/RGBA16 MSAA resources,
then requires the final output to remain unchanged. It refuses reference-image
outputs and resources without executed native GPU objects. Integer views retain
storage bits; samples are not averaged or converted for display.

Build explicitly with:

```
cmake --build --preset capture-full-release --target FloraReplayFinalSamplesProbe
FloraReplayFinalSamplesProbe FRAME NEW_OUTPUT_DIRECTORY DECIMAL_PCI_VENDOR RESOURCE_ID...
```

The probe is excluded from the default build and distribution. It links Replay
and accepts wire resource IDs; it has no Capture model, sidecar schema or IPC
dependency. Capture does not call or load it. The external development comparison
maps runtime IDs to wire IDs through the recorded initialization annotation and
its CopyResource destination, as in the preceding diagnostic stage.

Input IDs are unique and bounded to 32 resources; the aggregate selected-plane
budget is 2 GiB. Unsupported texture families, samples, IDs, output conflicts and
invalid vendors fail explicitly. Reports remain incomplete on execution or I/O
failure, and never establish capture acceptance. Extra reads occur after full
application replay, but their timing and inspection path still limit causal
claims about unobserved native execution.

The reusable `tools/validate_capture_final_samples.py` exercises the probe against
the independent native RGBA8 CPU oracle. Twelve cases include eight startup
captures and four initialization/application-Draw removal controls. All final
images and 72 layer/sample planes match; the controls prove that reads reflect
executed commands. Eleven pre-execution refusals verify both the expected error
and absence of published output (existing evidence remains unchanged). A twelfth
test obstructs a plane file after the probe starts, verifying that a real output
write failure leaves an incomplete report after GPU execution.

The paired-observation binary is preserved with its own hash. A subsequent
exception-handler correction explicitly clears `completed` before saving an
error, including failures while writing the final report. The final validation
binary passes all twelve refusal checks; the correction does not change the
successful GPU execution/readback path. These binary identities are kept separate
in the baseline rather than retroactively replacing the observation binary.

## Retained failing file: paired variation

The original formal-gate file is
`frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`, SHA256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
Three fresh Intel probe processes use the same file and selected resources
411, 415 and 621. Runs 0 and 1 reproduce the previously recorded three-pixel,
three-channel alternate final image. Run 2 matches the capture reference.

All six sample planes agree between runs 0 and 1. Between runs 0 and 2, five
planes remain byte-identical; resource **411, sample 1** differs at exactly one
texel, **(1596, 1519)**. The four raw binary16 component words are:

| Paired final output | R | G | B | A |
| --- | ---: | ---: | ---: | ---: |
| Alternate, three final pixels differ | 9109 | 9233 | 8865 | 15151 |
| Capture-matching final image | 9127 | 9247 | 8891 | 15143 |

These are storage coordinates in a 3456 x 1944 texture, not backbuffer screenshot
coordinates. The old file has no application-side frame-end sample export, so
the matching final image does not prove that every intermediate resource matches
the original application. This evidence associates two observed outputs with
two observed sample values within the same executions. It does not prove which
Draw first diverges, why it diverges, or whether the earlier single-texel research
describes the same mechanism. A read-only writer inventory narrows subsequent
inspection to the 269 Draw snapshots targeting this texture; this is not a
confirmed first-divergence event.

## New captures: final-image equality misses internal differences

The unchanged diagnostic runtime from the preceding stage is launched once more
with Intel Iris Xe, the equipment scene and unchanged quality settings. Two
consecutive requests complete at Presents 1254 and 1396; the session continues
to Present 1662 and exits normally after disconnect. Target module auditing
finds no Qt, Replay/Core, original GPA, RenderDoc or Python dependency, interface
gap or lost diagnostic.

| File | SHA256 | Present |
| --- | --- | ---: |
| `70b72371-c8b6-4a2e-8a37-555bef59d74d` | `8c2444a671f835757c6f0ba57434ac773915c39eb8a4c8f989a37599d5b22a52` | 1254 |
| `8684c19a-72f3-452a-8be1-def9532e0e13` | `a46d5012f2aa57e7668f90b92e31c3212d877bad27c5b60e7d5502f8cc4d15ef` | 1396 |

Three ordinary executions per player per file match capture exactly (12 total).
The final probe also matches each backbuffer, but one plane per file differs from
the application's diagnostic export. Ten of the twelve final-probe plane
comparisons are exact. Both differences belong to wire resource 621 (runtime
resource 3750), RGBA16, with the remaining pixels unchanged:

| File index / sample / texel | Captured raw words | Replayed raw words |
| --- | --- | --- |
| 0 / 1 / (1583, 1505) | 14222, 12046, 9475, 15356 | 14223, 12047, 9475, 15356 |
| 1 / 0 / (1936, 993) | 7804, 8117, 8176, 15358 | 7803, 8115, 8174, 15358 |

Earlier exploratory executions of the probe before adding its explicit final
native-object guard had 24 exact plane comparisons. Their outputs and distinct
probe hash remain in `paired-results.json`; they are not counted as final-version
probe validation. The guard only inspects the replay object map. The final
version's data does not support marking these files fully resource-correct just
because the displayed final image matches. They do not replace any formal-gate
file, and no tolerance is added.

## Regression and evidence

The probe and validator pass the 12-case/72-plane/12-refusal checks above. Four
related CTests (boundary, texture inspection, texture edit replay and frame
output) pass. Serial GF2/BF1 golden replays and their Draw-suppression controls
also pass. Capture-only configuration still avoids Qt and Replay. Production
capture/replay implementation and the ordinary replay executable are unchanged.

Raw evidence is retained in:

- `C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-final-samples-20261014`
- `D:/CDXrepo/FloraGPA-Cpp/artifacts/capture-gf2-paired-variable-20261014`

The [baseline](capture-gf2-paired-samples-baseline.json) records hashes, source,
binary identities and explicit failed comparisons. NTFS compression was enabled
on the six prior formal captures and the new diagnostic captures to preserve
space. Content hashes and logical byte lengths remain exact; no capture was
deleted or substituted. Raw outputs and generated files remain outside Git.

The next diagnostic question is which application Draw first produces the
observed differing MSAA texel, using the retained paired runs and shader/resource
inputs as evidence. Independent capture delivery and formal GF2 acceptance
remain incomplete.
