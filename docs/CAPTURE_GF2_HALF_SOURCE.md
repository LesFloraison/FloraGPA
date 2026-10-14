# GF2 original-format shader output and blend controls

This continues the [fragment observations](CAPTURE_GF2_FRAGMENT.md). The current
six-file GF2 gate remains incomplete. Production Capture, Format and Replay
binaries are unchanged; these are excluded development tools and experiments.

## Original-format observation

`FloraNativeDrawFixture --unblended-rt0` preserves the restored original RGBA16
resource, FLOAT view and initial contents, and disables only RTV0 blending.
`FloraReplayFragmentProbe --unblended-rt0` makes the same change at the selected
Draw boundary without replacing or clearing RTV0. Its bit reader supports both
RGBA16 and the existing private RGBA32 observation. Optional diagnostic PS
bytecode remains explicit. The modes require the correct bound resource/view and
independent blending; the native float and original-format modes are exclusive.

```text
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 3 --pixel 618 1562 1492 --unblended-rt0 --summary-only
FloraReplayFragmentProbe FRAME NEW_OUT 5598 618 1562 1492 32902 --unblended-rt0
```

For the original PS887 and full DrawIndexed 5598, three native repetitions,
four fresh replay processes and a final independently built native run agree on
both complete sample planes. At pixel (1562,1492), sample 0 stores the half words
`[14896,14100,13467,15042]`. The first word differs from the prior CPU
round-to-nearest conversion of the RGBA32 observation, which produced 14897.
Changing blend enable can still change driver optimization; this is not proof
of the original blended shader's exact intermediate values.

A skip-Draw control preserves all eight original output sample planes exactly.
The unmodified native Draw again produces the known lower variant, and the
existing RGBA32 mode retains its two complete sample hashes in both tools.

## Fixed-source GPU controls

Two diagnostic PS4 shaders return constants at original triangle 1613, using
the original VS, initial resources, raster/depth state and native blending.
They replace the original pixel calculations and are not equivalent captures.
The first returns the four raw float32 words measured in the earlier experiment;
the second returns exact float32 representations of the stored half words above.

| Constant source | Blend disabled: stored half words | Original blend: stored half words |
| --- | --- | --- |
| Observed float32 output | 14896, 14100, 13467, 15042 | 14698, 13854, 13265, 15091 |
| Exact values of the stored half words | 14896, 14100, 13467, 15042 | 14697, 13854, 13265, 15091 |

The first GPU blend result matches the known lower original variant. These
controls show that two distinct sources can have identical unblended stored
values yet different blended results on this host. Consequently, the unblended
half values cannot uniquely determine the blend input. The earlier CPU
reconstruction remains a numerical consistency result, not evidence of the
hardware's internal rounding sequence. Neither experiment reproduces or explains
the higher original variant, and no comparison tolerance has changed.

## GPA inspection retry

The original GPA again loaded equipment file
`frame-23c72ea8-d6f0-4557-96e7-48513d0f27bd.gpa_frame`, displayed 637 events and
the final image, and completed performance collection. A click then failed with
`computer-use request timed out: click`. After recovering the returned window
binding, keyboard navigation failed with `computer-use request timed out:
press_key`. Detailed shader/resource/pipeline GUI acceptance remains incomplete.
No new screenshot was saved or new GUI gate claimed. The GPA instance and its
satellite exited before the diagnostic GPU runs.

## Verification and boundaries

* Fifteen completed diagnostic processes: nine native and six replay; eleven
  native restore/observation iterations with all 88 initial sample comparisons
  exact. Every selected observation scope has a clean debug queue.
* Full replay prefix messages remain recorded separately; the existing 608
  messages are not reclassified as a clean prefix.
* Twelve refusal controls pass, including conflicting/duplicate options,
  incorrect formats/views, wrong output selection, missing pixel, unsupported
  shader stage, and preservation of an existing output directory.
* Full and independent native builds pass. Native runtime module inventories
  exclude Qt, Capture, original GPA, RenderDoc and Python. Four boundary/format
  CTests pass. No production replay change or new golden run is claimed; prior
  golden results retain their existing binary identities.

The [baseline](capture-gf2-half-source-baseline.json) pins reports, both build
identities, source hashes, constant shader inputs, unchanged production hashes
and an 80-file raw evidence manifest. Raw scripts, shaders, reports and logs
remain outside Git under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-half-source-20261014`.

The current six-frame gate, higher-variant cause and detailed original-GPA GUI
inspection remain open. This stage establishes no production fix or newly
accepted frame.
