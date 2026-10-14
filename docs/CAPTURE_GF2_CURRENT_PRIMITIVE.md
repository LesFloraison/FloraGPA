# Current GF2 mismatch reduced to one original triangle

Original triangle 2443 alone produces the selected sample-1 value from the
current independent full Draw. Three repeated isolated executions agree after
exact output restoration. Neighboring triangle 2444 writes sample 0 instead.
Unlike the older triangle-1613 example, the current pixel center is inside the
CPU-projected triangle. The current six-frame gate remains **failed**; the
isolated runner still has not reproduced the lower variant observed in full
replay, and no production rendering change is made.

## Boundaries and retained identities

This uses the [current independent bundle](CAPTURE_GF2_NATIVE_LEVEL.md), event
5651, resource 411, pixel `(1596,1519)`. The source frame remains SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
Original shader bytes, input resources, bindings, output initialization and
device settings remain the same. The native commands change only the selected
triangle-list index range. All nine processes use the frozen independent
runner from the preceding stage, Intel level 11.1 and debug flags, under
System32-only PATH. No Flora, Qt, GPA, RenderDoc or Python runtime is loaded.

The existing post-VS probe exports 7,401 references / 2,467 triangles, 160-byte
stride, using the original VS. Export SHA-256 is
`56278999a1800b84fa0635e6e8f31dae762fd4cdfcd298f63c12dc76caa39150`.
This replaces rasterization with stream output; its data are geometry-search
evidence, not a direct measurement of pixel interpolation.

## Native prefix and isolated controls

Triangle numbers and index ranges are zero-based. Let **I** be the initial half
words `[7759,8093,7731,15285]`, **A** the full Draw's sample-0 words
`[7444,7618,7269,15360]`, and **B** its higher sample-1 words
`[9127,9247,8891,15143]`.

| Executed indices | Repetitions | Sample 0 | Sample 1 |
| --- | ---: | --- | --- |
| Triangle 2412 only | 1 | I | I |
| Triangle 2432 only | 1 | I | I |
| Triangle 2443 only, `[7329,7332)` | 1 | I | B |
| Before triangle 2443, `[0,7329)` | 1 | I | I |
| Through triangle 2443, `[0,7332)` | 1 | I | B |
| Through triangle 2444, `[0,7335)` | 1 | A | B |
| Triangle 2444 only, `[7332,7335)` | 1 | A | I |
| Triangle 2443, separate process with sample-position query | 3 | I | B |
| Explicit complete range, `[0,7401)` | 1 | A | B |

All eleven Draw repetitions pass all eight initial sample-plane comparisons:
88 byte-exact restorations in total. Debug queues are empty. The explicit full
range also matches all eight post-Draw sample hashes from the preceding ordinary
full-Draw fixture execution. Partial ranges are diagnostic transformations and
are not claimed to reproduce whole images. Every execution reports completed
diagnostic comparison with exit 3, because output differs from the bundle's
full-Draw expected planes. No failing result is discarded or counted as an
accepted frame.

The selected-pixel byte report comes from each existing full-plane readback.
The sample-position query uses a private resource with the same dimensions,
format and sample descriptor, before restoration. It observes offsets
`(0.25,0.25)` and `(-0.25,-0.25)`; the CPU search consumes that identified report
instead of assuming a standard MSAA pattern.

## Geometry and reusable CPU search

`tools/inspect_draw_candidates.py` is a development-only, standard-library CPU
tool. It checks matching frame/event/shader identities, vertex byte hash and
length, output signature, complete triangle count, one viewport, finite values
and positive clip W. It rejects unverified layouts rather than silently dropping
unprojectable triangles. A sample-position report must identify the same bundle,
event, pixel and sample count. Output creation never replaces an existing file.

The tool emits sixteen bounding-box candidates within a 1.5-pixel search radius.
GPU range controls above select the relevant triangle; bounding boxes alone do
not establish coverage. Triangle 2443 projects approximately to:

```text
(1600.1287446, 1527.1808625)
(1596.0602428, 1518.4737587)
(1592.3664332, 1511.6781613)
```

Its unsnapped area is approximately 2.25724 square pixels. Pixel-center
barycentric weights are `(0.17772, 0.74558, 0.07670)`; sample-1 weights are
`(0.34949, 0.31694, 0.33357)`. Both sets are positive. Sample 0 has a negative
weight, consistent with the isolated GPU observation but not proof of GPU edge
rules. The tool does not implement clipping, culling, subpixel snapping, depth,
discard or raster coverage, and it does not compute interpolated shader values.

Running the same tool against the retained older bundle recovers triangle
1613's area `0.0916326` and outside-center weights
`(-2.73228,1.82146,1.91082)`. That older geometric condition is not shared by
this current CPU projection; it must not be assumed to explain both cases.
Both raw bundles and their distinct resource/pixel identities remain intact.

Seven CPU test groups cover analytic geometry, viewport/perspective transforms,
outside points, degenerate triangles, nonpositive/nonfinite W, malformed
identities/storage/signatures and bounds. Four CLI controls reject existing
output and mismatched/incomplete sample reports, preserving original bytes.
Source-boundary CTest passes. No production C++, capture dependency or runtime
Python backend is introduced.

```text
FloraReplayPostVsProbe FRAME NEW_POSTVS 5651 --gpu-vendor 32902 --all-outputs
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 3 --indices 7329 3 --pixel 411 1596 1519 --sample-positions --summary-only
python tools/inspect_draw_candidates.py POSTVS BUNDLE_JSON 1596 1519 NEW_JSON --sample-report NATIVE_REPORT
python -m unittest discover -s tools -p test_draw_candidates.py -v
```

Raw binaries, vertex output, native reports, verifiers and commands are retained
at `C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-current-primitive-20261014`.
The [baseline](capture-gf2-current-primitive-baseline.json) pins all identities
and the manifest. Production CLI hash remains
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
Legacy migration parity and the failed formal acceptance status remain unchanged.
