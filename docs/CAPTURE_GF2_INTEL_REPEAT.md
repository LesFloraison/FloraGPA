# Three consecutive Intel GF2 capture candidates

One fresh startup-injected GF2 session produced three consecutive files across
the home and equipment-room scenes. **All 18 positive replays are byte-exact to
their respective captured RGBA references**: three fresh processes per file in
each of Flora and the original GPA player, explicitly selecting Intel Iris Xe.
All three files also opened in the original GPA GUI on Intel with selected event,
shader and resource inspection. These are three candidates, not completed formal
acceptance: the six-frame gate remains **0/6** pending the remaining checks.

## Session and output

The session used `-force-d3d11 -force-device-index 1`, swap chain 1 and a 2048 MiB
resource budget. No game quality setting was changed. The game continued after
each capture, reached Present 2335, disconnected cleanly (the HUD disappeared),
and exited normally before any replay GPU checks began. Status reported no
interface gaps or lost diagnostics. The final status module inventory contains
no Qt, Python, RenderDoc or original GPA player dependency in the capture process.

| Index | Scene | Present | File bytes | Positive runs | Different RGBA pixels |
| --- | --- | ---: | ---: | ---: | ---: |
| 0 | Home | 1414 | 1,006,223,068 | 6 | 0 in every run |
| 1 | Equipment room | 1766 | 1,340,781,186 | 6 | 0 in every run |
| 2 | Equipment room | 2017 | 1,347,588,982 | 6 | 0 in every run |

The original player's real executing contexts identify Intel Iris Xe through
public device/adapter getters. Each execution completes Open/Playback/Close;
Flora reports completed execution without reference-pixel substitution. The
comparison remains exact RGBA8 equality, with no tolerance or threshold change.

Six additional negative runs suppress Draw submission. The original audit
suppresses DrawIndexed (199, 234 and 235 calls respectively); Flora suppresses all
Draw calls. The two outputs agree for each file and differ from its captured
reference by 2,330,132, 900,035 and 970,781 pixels respectively. These controls
confirm execution contributes to the image; their API scopes are not identical.

## Selected inspection

Flora inventories contain 608, 966 and 970 resources. Eight native Intel pipeline
exports cover selected large indexed draws, the final draw, and the first compute
dispatch where present. Every export reports 1302 known fields and four explicitly
unknown fields. Fourteen selected shader exports contain DXBC and disassembly.

The original GUI opens each unmodified file through its registered command-line
file path, avoiding the previously unworkable folder picker. It displays 271,
637 and 638 events respectively and the actual rendered scene. Selected checks:

* Home: DrawIndexedInstanced event 34, vertex DXBC and input/output signatures,
  D32_FLOAT_S8X24_UINT 2x depth resource, depth/rasterizer state and constant-buffer
  data table.
* First equipment-room frame: Dispatch event 487, dimensions 54x31x1, compute
  DXBC (`cs_5_0`, 32x32x1 threads), CBV/SRV/sampler/UAV bindings and the 1728x972
  RGBA8 UAV image/histogram.
* Second equipment-room frame: DrawIndexed events 220/469, shadow-depth metadata,
  vertex/pixel DXBC, three pixel outputs, depth/stencil values and a 2048x2048
  BC1 sRGB texture with mip image/histogram.

GUI screenshots remain in the task transcript; the ignored observation record
states exactly which views were inspected. This is selected inspection, not an
exhaustive validation of every resource or state. No raw resource-byte comparison
is inferred from a visible data table or histogram.

## Remaining acceptance work

The second independent three-capture launch, selected resource-byte comparisons,
and final regression/current-source package checks remain pending. The historical
Intel capture's ten-pixel discrepancy and NVIDIA MSAA anomaly are still unresolved;
these new passing files neither fix those failures nor invalidate their evidence.
Existing explicit API limitations remain in effect.

No production code changed in this stage. All GPU checks ran serially, with GF2
and the original GUI closed before CLI checks. Thus the preceding code-stage
CTest/golden results are retained as historical evidence, not reported as new
runs. Captures, scripts and exports stay ignored under
`artifacts/capture-gf2-intel-repeat-20261014`. The
[baseline](capture-gf2-intel-repeat-baseline.json) pins frame/binary hashes, all
comparisons, selected inspections and the 263-file evidence manifest. Older
captures and the reference workspace remain intact.
