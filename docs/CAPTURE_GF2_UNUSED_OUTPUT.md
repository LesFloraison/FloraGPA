# GF2 shader output without an extra attachment

Three complete diagnostic replays with a count-only shader output written to
unbound slot 3 produce the capture-matching image. Three interleaved recreations
of the original shader produce one matching image and two known alternate
images. An extra render-target allocation or binding is therefore not necessary
for the observed matching instrumented result in these runs. This small control
does not establish a root cause, deterministic behavior, or a production fix.
The formal six-frame acceptance gate remains **failed**.

## Controlled change

`FloraReplayTexelProbe` adds `shader-count-unused`. It uses the existing pinned
count-only DXBC transformation, retaining every original executable instruction
and adding only an output declaration and constant write. Immediately before
Draw 5651 it creates and binds that PS through `ScopedPixelShader`, then restores
the original PS immediately afterward. RTV3 must be unbound. No private RTV is
allocated; render targets, blend state, factors and sample mask are untouched.
The existing color, input-resource, depth and native-pipeline observations remain
in place. Diagnostic copies and shader recreation still change execution timing.

The original PS SHA-256 remains
`fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95`;
the changed PS is
`36e81f84589bff2faedefa6aaf92befc50d62cbb1532b6e02bba69e20f0f5199`, identical
to the prior count-only attachment experiment. The six runs use the same probe
binary, Intel adapter, input frame and event. Both shader paths use the existing
scoped recreation helper. The frame SHA-256 is
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.

| Mode | Runs | Capture-matching | Known alternate |
| --- | ---: | ---: | ---: |
| Added output, no attachment | 3 | 3 | 0 |
| Original shader recreation | 3 | 1 | 2 |

Independent PNG decoding confirms the unchanged final matching/alternate hashes
and the same three differing pixels/channels. The selected pre-Draw half words
remain `[7759,8093,7731,15285]`. Post-Draw words are
`[9127,9247,8891,15143]` for matching runs and
`[9109,9233,8865,15151]` for alternate runs. All fifteen input resource records,
native pipeline observations, depth state and both selected depth samples agree
with the prior verified observations. No reference pixels are used for execution,
and no comparison threshold changes.

## Debug messages and independent controls

Each added-output scope emits exactly one warning: the pixel shader expects an
RTV at slot 3, and writes to that unbound slot are discarded. The tool retains
the full message. Only this mode permits the exact warning ID, warning severity
and slot-3 description; other warnings/errors still fail. The original shader
scopes have no warnings/errors. The existing 608 prefix messages remain separate.
This is not reported as a warning-free replay.

The native test covers WARP and Intel, 2x/4x MSAA and the prior attachment modes:
**24 cases, 744 exact texel checks and 144 refusal checks**. Four cases exercise
the extra output without an attachment. They verify that the currently applied
pipeline differs only in PS identity, the original three output targets match
their CPU oracle, and scope exit restores the original pipeline. Each has the
one expected warning; all cases have zero unexpected warnings/errors. Classifier
controls reject error severity, a different message ID and a different slot.
The final texel validator passes 28 cases and 25 refusals. Source-boundary CTest
and capture-only configuration pass. All GPU executions are serial.

An initial native build failed under `/WX` on a shadowed local name, which was
renamed before execution. The first passing native report labelled its debug
count as zero while retaining expected messages separately. The final binary
and report correct that field to include the four expected warnings, with a
separate zero-unexpected count. Both versions are preserved and distinguished.

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraFragmentCheckpointTests
FloraFragmentCheckpointTests NEW_REPORT
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 shader-count-unused 5651 5651 7
```

## ISA follow-up and storage limits

The preceding stage exported the original shader's ISA. A proposed comparison
would export the count-only shader from a diagnostic copy of the frame. Both
copy attempts failed with disk-full errors before any shader/header/index
modification. No completed derivative or modified-shader ISA export exists, and
neither attempt counts as an experiment result. Their failed temporary files
contained only exact source prefixes; lengths, hashes and errors were recorded,
then those reconstructible duplicate prefixes were removed. Original captures
and unique experiment data remain intact.

To recover working space, 35 existing bundle files, 18 native output files and
75 raw RGBA files received transparent filesystem compression. Each file was
hashed before and after; contents are unchanged. Some bundle files have hard
links, so directory-entry attributes and summed logical sizes are not a reliable
measure of freed space. The raw records retain these operations and failures.
The failed copy scripts are development evidence, not a supported frame editor.

No production Capture, Format or Replay source changes. The production replay
binary remains SHA-256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
Raw reports, frozen binaries, scripts and logs are under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-unused-output-20261014`.
The [baseline](capture-gf2-unused-output-baseline.json) pins their identities and
manifest. Legacy migration parity remains unchanged.
