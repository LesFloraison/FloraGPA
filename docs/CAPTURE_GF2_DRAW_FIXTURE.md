# GF2 isolated native Draw fixture

This continues the [MSAA readback investigation](CAPTURE_GF2_READBACK.md). It
does not change the capture runtime, encoder, format, or production replay.
The current-runtime six-frame gate remains incomplete, with the exact-byte
comparison rule unchanged.

## Export and independent execution

`FloraReplayDrawExportProbe` exports settled native state immediately before a
restricted `DrawIndexed`. It requires shader model 4 VS/PS, no dynamic classes,
tessellation, geometry shader, stream output, or predication. It saves original
DXBC, the input layout's original signature, native descriptors, vertex/index
bindings, constant-buffer ranges, samplers, SRVs, MinLOD, raster/output state,
and every retained resource. Each MSAA output has separate initial and
post-draw sample planes. Payloads have byte lengths and SHA256 hashes; an
interrupted export remains explicitly incomplete.

The selected GF2 event 5598 produces a bundle with 19 resources and eight MSAA
sample planes across three color targets and one depth/stencil target. Reading
resources at this boundary changes scheduling; the exported post-draw images
describe this diagnostic execution, not a new authoritative capture.

`FloraNativeDrawFixture` consumes only the bundle. It has no Flora, GPA,
RenderDoc, or Qt headers or libraries. A separate CMake project builds it
without configuring the application or finding Qt. It creates a debug D3D11
device on the specified PCI vendor, restores resources, verifies every MSAA
sample byte, binds the saved pipeline, executes the original Draw, and compares
all output samples. Shader code is executed; expected images are used only for
comparison. All GPU work in this stage ran serially.

Color samples are restored through integer views. D32S8 depth and stencil are
restored separately. The development runner deliberately restricts input texture
formats to those exercised by this bundle, requires quality-zero single-layer
MSAA outputs, and rejects unsupported layouts. Payload names, lengths, hashes,
subresource pitches/ranges, descriptors, counts and output paths are checked.
It is a diagnostic fixture runner, not a general frame player or capture module.

```text
FloraReplayDrawExportProbe FRAME NEW_BUNDLE 5598 32902
cmake -S tools/native/draw_fixture -B NEW_BUILD -A x64
cmake --build NEW_BUILD --config Release
FloraNativeDrawFixture NEW_BUNDLE NEW_OUT 32902 3
FloraNativeDrawFixture NEW_BUNDLE NEW_NEGATIVE_OUT 32902 1 --skip-draw
```

Exit 0 means the diagnostic execution completed with exact comparisons; exit 3
means it completed with differences. Initialization, restore, device and debug
warning/error failures leave `completed` false. Neither exit establishes the
capture acceptance gate.

## Results and limits

Five successful native processes execute thirteen Draw repetitions, including
three repetitions from the independently configured build under System32-only
PATH. All 104 initial sample comparisons pass exactly. Of 104 post-draw sample
comparisons, 91 are exact. The thirteen remaining comparisons each differ only
at resource 618 / sample 0 / pixel (1562, 1492), by four bytes in three half-float
RGB components. Every native execution yields the previously observed lower
variant; the export execution yielded the higher variant. Alpha matches.

| Complete sample | SHA256 |
| --- | --- |
| Exported post-draw higher variant | `b7b48ed95b35542afa7a66b93d9d029be81debe11ecc77741b4b74330fbc8a50` |
| Independent native lower variant | `10bfd552c3acc038eab0bb6fcecf3c309680f224dbba938eccd0d73013e144b4` |

The separate skip-Draw process passes all eight restoration comparisons and
differs on six post-draw planes, affecting 11,749 to 11,814 pixels per plane.
This negative control confirms that the normal runner's result requires Draw
execution. Initialization and execution debug queues are empty for all six
completed processes. Their module inventories contain no capture, replay,
GPA, RenderDoc, Qt or Python runtime.

These results narrow the experiment to one Draw using original shaders and
settled inputs outside either player. They do **not** reproduce variation within
the independent runner, prove that all relevant execution history has been
eliminated, establish a driver root cause, or justify changing capture behavior.
The independent context/resource identities, restoration passes and readback
operations change execution conditions. Further reduction remains necessary.

## Preserved failures and validation

The first exporter attempt correctly refused unread input-layout bytes. The
layout contains a trailing length-prefixed DXBC signature; the exporter now
checks and exports it. Its incomplete directory is preserved.

The first two native executions stopped before Draw because stencil restoration
verification returned zeros. The readback copy had omitted the depth/stencil
bind flag. Preserving that flag on the private copy made both depth and stencil
byte-exact on this adapter; no production code was changed. Both failed runs,
the second mismatch dump, and build failures are retained. No tolerance or
canonicalization was introduced to make these comparisons pass.

Eleven refusal controls pass: invalid repeat/vendor/options, existing output,
incomplete/truncated JSON, traversal, hash mismatch, and an invalid subresource
pitch. Four boundary/format CTests pass. Capture-only configuration still skips
replay and Qt. The GF2 and BF1 original golden frames execute serially with
their unchanged exact RGBA hashes and `reference_pixels_used=false`.

The [baseline](capture-gf2-draw-fixture-baseline.json) pins reports, shaders,
bundle, sources, binaries, regression results and an evidence manifest. Raw
data remain outside Git under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-draw-fixture-20261014`.
Current original-GPA GUI inspection, the variable-frame cause, and the formal
six-frame acceptance are still pending.
