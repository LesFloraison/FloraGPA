# GF2 single-triangle reduction

The [standalone draw fixture](CAPTURE_GF2_DRAW_FIXTURE.md) now supports bounded
triangle-list index ranges and selected-pixel byte reporting. This isolates the
previously variable pixel to one original triangle. It does not resolve the
difference between the exported higher variant and native lower variant, and
does not change production capture/replay or the current six-frame gate.

## Native controls

The original event 5598 has 7,401 indices / 2,467 triangles. All controls use the
same exported original VS/PS, input resources, state and initial output samples
on Intel Iris Xe. Triangle numbers below are zero-based. The selected pixel is
resource 618, coordinate (1562, 1492).

| Executed original indices | Sample 0 RGBA half words | Sample 1 RGBA half words |
| --- | --- | --- |
| Before triangle 1613: `[0, 4839)` | 12486, 11038, 8056, 15360 | 12486, 11038, 8056, 15360 |
| Through triangle 1613: `[0, 4842)` | 14698, 13854, 13265, 15091 | Initial value |
| Through triangle 1614: `[0, 4845)` | Same lower variant | 14651, 13021, 10297, 15360 |
| Only triangle 1613: `[4839, 4842)` | Same lower variant | Initial value |
| Only triangle 1614: `[4842, 4845)` | Initial value | 14651, 13021, 10297, 15360 |
| Complete original Draw | Same lower variant | 14651, 13021, 10297, 15360 |

An explicit full index range also matches the ordinary full Draw's eight sample
hashes. A fresh process repeats the isolated triangle 1613 three times, restoring
all outputs before each execution; all three give the same lower value. Across
nine processes / eleven Draw executions, all 88 complete initial sample comparisons
are byte-exact and all debug queues are empty. The modified range runs are
diagnostic transformations, not equivalent full-frame replays or accepted captures.

The GPU `Texture2DMS.GetSamplePosition` query on a private resource with matching
dimensions, storage format and sample descriptor reports offsets `(0.25, 0.25)`
and `(-0.25, -0.25)` for samples 0 and 1. It does not infer quality-zero sample
locations from a presumed standard pattern. The query runs before restoration
and only when explicitly requested. See Microsoft's
[GetSamplePosition reference](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-texture2dms-getsampleposition).

## Geometric evidence and next diagnostic

The previously verified full post-VS export identifies triangle 1613's projected
vertices approximately as `(1563.7082, 1493.2315)`, `(1565.4511, 1493.9967)` and
`(1561.4146, 1492.1192)`. Its area is about 0.09163 square pixels. All three clip
W values are positive. CPU projection gives sample-0 barycentric weights near
`(0.2128, 0.2099, 0.5773)`; the pixel center instead gives
`(-2.7323, 1.8215, 1.9108)`, outside the triangle. This projection is a search aid,
not an implementation of subpixel snapping, edge rules or the GPU rasterizer.

The captured PS uses ordinary linear inputs and pixel-frequency execution.
Direct3D documents per-sample coverage with shared per-pixel shading under MSAA;
see [Rasterization Rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules).
The thin triangle and extrapolated pixel-center inputs provide a focused next
experiment for shader interpolation and calculations. They do not establish
undefined application behavior, a driver defect, or a capture implementation fix.
Variation within the standalone runner remains unreproduced.

## Tool boundaries and verification

```text
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 1 --indices 4839 3 --pixel 618 1562 1492 --summary-only
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 3 --indices 4839 3 --pixel 618 1562 1492 --sample-positions --summary-only
```

`--indices FIRST COUNT` selects complete triangles inside the original draw and
retains its base vertex and original buffer bindings. Reports preserve both
original and applied Draw arguments. Incomplete, duplicate, overflowing,
out-of-range and incompatible options fail explicitly. `--pixel` extracts bytes
from the existing full-sample readback and adds no GPU pixel read. `--summary-only`
omits large mismatch dumps but retains full-sample comparisons/hashes, differences
and selected-pixel bytes; it does not reduce the comparison scope. Default Draw
execution and mismatch dump behavior remain intact.

The independent project and full-project target build with VS2022 x64. System32-only
PATH runs and module inventories confirm no Qt, Python, GPA, RenderDoc, capture
or replay runtime. Twenty-three refusal controls and four boundary/format CTests
pass. A final run without any optional flags matches all eight full-range sample
hashes and retains its mismatch dump. Production CLI, controller and capture runtime hashes are unchanged from
the previous stage; its exact GF2/BF1 golden checks remain the latest rendering
regression evidence. No production renderer was modified here.

The [baseline](capture-gf2-primitive-baseline.json) pins source and tool identities,
all nine reports, exact initial-state checks, CPU candidate projection, refusal
results and the raw evidence manifest. Raw outputs are preserved under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-primitive-20261014`.
The formal six-frame gate, original-GPA GUI inspection and root-cause work remain
incomplete; no comparison threshold was relaxed.
