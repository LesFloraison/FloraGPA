# Six Intel GF2 capture candidates from two launches

Two independent startup-injected GF2 sessions each produced three consecutive
captures, covering the home and equipment-room scenes. All **36 positive
dual-player replays** match their captured final backbuffer bytes exactly. All
six unmodified files open in the original GPA GUI on Intel with selected event,
shader, resource and state inspection. This candidate checkpoint precedes the
final package/scope audit; the current result is recorded in
[CAPTURE_GF2_ACCEPTANCE.md](CAPTURE_GF2_ACCEPTANCE.md).

## Second launch

The second session is `b289e9d6-b388-4e8c-9983-ae2554050895`, a new process launched
with `-force-d3d11 -force-device-index 1`. Its log identifies Intel Iris Xe. It
uses swap chain 1 and a 2048 MiB resource budget, with no game-quality changes.

| Index | Scene | Present | File bytes | Positive replays | Different RGBA pixels |
| --- | --- | ---: | ---: | ---: | ---: |
| 0 | Home | 1748 | 1,007,085,589 | 6 | 0 in every run |
| 1 | Equipment room | 2197 | 1,340,621,274 | 6 | 0 in every run |
| 2 | Equipment room | 2403 | 1,340,893,610 | 6 | 0 in every run |

The game continues to Present 2642, remains animated after capture, disconnects
with its HUD removed, and exits normally before replay checks. No interface gaps
or lost diagnostics are reported. The capture process module inventory contains
no Qt, Python, RenderDoc or original GPA player dependency. No capture requests
were discarded or selected retrospectively based on their pixel results.

Each player uses three fresh processes per file. The original execution audit
obtains the actual Intel adapter from the executing D3D11 context. Six further
Draw-suppression runs change both players' output identically: 2,293,597,
1,112,903 and 978,680 pixels respectively. The original audit suppresses 205,
229 and 236 DrawIndexed calls; Flora's control suppresses all Draw calls.

## Resource and reference checks

The second group's 33 bound-buffer comparisons (24 unique buffers) and three
BC1 full-mip texture comparisons match exactly, using the same method as the
[first resource-byte stage](CAPTURE_GF2_RESOURCE_BYTES.md). Across both launches
there are **66 buffer-binding and six texture checks**. All twelve original
executions with these diagnostic snapshots preserve exact final output.

A separate bounded, read-only container check locates each capture's dedicated
final-output Texture2D record and its data blob. For all six files, these RGBA
bytes equal the independently decoded bottom-up BMP preview, original-player
captured TGA, and all six positive executed outputs. This confirms that the
comparison is against the captured resource payload, not merely a displayed
thumbnail. Draw negatives establish that executed output is not substituted
from that payload.

The checks cover selected draw inputs and full final RGBA output. They do not
prove every intermediate resource, floating-point operation or hidden state.
Exact equality was retained throughout; no failing result caused a threshold
change or replacement of an earlier frame.

## Selected inspection and remaining gate

Eight native Intel pipeline exports and fourteen shader DXBC/disassembly exports
cover selected indexed draws, final draws and available compute dispatches.
Pipeline exports preserve four explicitly unknown fields alongside 1302 known
fields. Original-GUI event counts are 281, 632 and 639. Inspected views include:

* Home: DrawIndexed 131, VS/PS DXBC, CBV/VBV/SRV/MRT bindings, 2x RGBA16 float
  output image, and depth/stencil state.
* First equipment-room frame: Dispatch 480, 54x31x1 arguments, compute DXBC with
  32x32x1 threads, CBV/SRV/sampler/UAV bindings and RGBA8 UAV image/histogram.
* Second equipment-room frame: DrawIndexed 474, VS/PS DXBC, resource bindings,
  2x D32S8 depth image/histogram and depth/stencil state.

These observations supplement the native export and byte checks; GUI selection
is not exhaustive. Screenshots remain in the task transcript and the ignored
observation JSON identifies the inspected views.

The [baseline](capture-gf2-intel-second-baseline.json) records this session, exact
comparisons, selected inspections, serial CTest/golden results and evidence
hashes. Raw data remains in `artifacts/capture-gf2-intel-second-20261014` and
`artifacts/capture-gf2-resource-bytes-second-20261014`. Production code did not
change in this stage.

The full regression initially passed 261/263 suites. Two test fixtures required
an explicit append-counter reset and waits for asynchronous geometry exports.
Their corrected four-suite rerun and four golden checks passed. The initial
failures and diagnostic attempts remain in this evidence directory.

Current-source package validation and final scoped acceptance are recorded in
the acceptance document linked above. This baseline retains its candidate-stage
count of zero formally accepted files.
The older Intel ten-pixel mismatch and NVIDIA MSAA anomaly remain unresolved;
passing these six Intel files is not evidence that those failures are fixed.
The broader explicit API limitations also remain. The active implementation goal
is not complete.
