# GF2 DrawIndexed 5598 boundary investigation

The current-runtime six-frame gate remains **incomplete**, with the same exact
comparison rule. This follows [current GF2 revalidation](CAPTURE_GF2_CURRENT.md);
it does not replace its six files or turn diagnostic stability into acceptance.
Production capture, format and replay behavior is unchanged.

## Settled inputs and vertex outputs

On Intel Iris Xe, four fresh native executions per MSAA sample export resource
419 immediately before event 5598. Both depth/stencil samples are individually
byte-stable across their four runs. Each exported sample contains 53,747,712
bytes. This adds the depth/stencil input omitted from the earlier sixteen bound
buffer/texture comparisons; settled readbacks do not prove concurrent execution
is correct.

The excluded development target `FloraReplayPostVsProbe` now accepts
`--gpu-vendor DECIMAL` and `--all-outputs`. It reflects the VS output signature,
exports its components in declaration order, and records semantic, register,
type, component range and byte offset. The old position/TEXCOORD1 mode remains
available. Stream output from preceding-stage shader bytecode uses the
[documented D3D11 mechanism](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-creategeometryshaderwithstreamoutput).
It retains the existing restrictions on draw shape, stages, predication and SO.

Eleven full-signature executions (ten initial and one final-build check) export
the same 7,401 vertex references, 40 float components per vertex, 160-byte stride.
All values are finite. One legacy-mode export exactly matches the corresponding
position/TEXCOORD1 projection of the full export. SO statistics independently
verify 2,467 primitives and sufficient storage. This replaces rasterization with
stream output, so it does not demonstrate stable interpolation or pixel shading.

## Restoring a GPU checkpoint

The new excluded target `FloraReplayDrawRepeatProbe` copies all three bound color
resources and the depth/stencil resource before the selected DrawIndexed. After
the original draw, it reads sample output, restores the four resources with
CopyResource, and issues the same DrawIndexed again. Every iteration is hashed;
distinct complete outputs are retained. The tool refuses shader models other
than 4 and draws with tessellation, GS, SO or predication, instead of ignoring
possible additional shader side effects. It is a replay-side development tool,
never a capture dependency or packaged application component.

Eight processes produce 160 sample exports: eight initial draws and 152 restored
draws. Five ordinary sample-0 processes cover 122 restored draws; two debug-device
sample-0 processes cover twenty; one sample-1 process covers ten.

One initial sample-0 draw produces the previously observed higher RGB value at
(1562,1492). Its subsequent thirty restored draws all produce the lower value,
with exactly four differing raw bytes and no other changed storage. The other
ordinary initial draws and all 122 ordinary restored sample-0 draws are lower.
Debug sample-0 repeats are lower; sample 1 is stable in its control. The same
two half-float values and sample hashes match the earlier investigation.

This is **not** evidence that identical repeated draws still vary after checkpoint
restoration: the restored path is stable in these tests. Copies, completion
readbacks and reused objects change execution conditions. They may affect storage,
caching or scheduling; the experiment does not identify which factor matters or
justify inserting those operations into production as a fix.

The final debug build records messages separately before and within the selected
probe. Its prefix contains 608 messages, including conditional comparison-sampler
and format warnings on other DrawIndexedInstanced calls. No messages occur during
the selected checkpoint/draw/readback iterations. Prefix warnings are preserved;
neither a clean whole-frame debug run nor a cause for the variable texel is claimed.

## Reproduction and validation

```text
FloraReplayPostVsProbe FRAME NEW_OUT 5598 --gpu-vendor 32902 --all-outputs
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5598 618 0 32902 30
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5598 618 0 32902 10 --debug-device
```

Both targets build with VS2022 x64. Four boundary/container/codec/writer CTests
pass, and capture-only configuration succeeds without these replay targets.
Twelve refusal checks cover invalid/duplicate/missing options, repetition bounds,
existing outputs and selection of an incompatible target. Existing evidence is
unchanged; the incompatible target leaves an explicitly incomplete diagnostic
report. All GPU executions were serial with GF2 closed. No new golden sweep or
original-GPA GUI acceptance is claimed for this development-only change.

The [baseline](capture-gf2-boundary-baseline.json) records exact counts, binary and
source hashes, report identities and limitations. Raw evidence is preserved at
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-boundary-20261014`, with a manifest.
The next investigation must distinguish effects of output restoration/readback
from shader/raster execution, while preserving the original file and acceptance rule.
