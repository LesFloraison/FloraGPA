# Native private draw history

The native analysis Worker can export selected-texel observations for one GPA
draw, including private geometry checkpoints, optional C++ PS evaluation,
output-merger comparisons and conditional system-ID observations. This path
reads a `.gpa_frame` directly and does not load RenderDoc or Intel GPA code.

```text
FloraGPA.Cli analyze frame.gpa_frame history --history-scope private-draw --gpa-event 113 --resource 80 --x 799 --y 480 --gpu-vendor 4318 --out draw-history
```

Choose `--warp` for software execution or `--gpu-vendor` for explicit hardware
selection. Normal adapter-selection rules apply; an unavailable selection fails.
The selected GPA resource must be bound as an RTV or DSV at that draw. Mip,
layer, slice and sample default to zero and can be supplied explicitly.
`--typed-format`, when present, must agree with the actual bound view.

The output directory must be new or empty. `history.json` contains the versioned
`private_pixel_draw_history` result. `result.json` binds it to the frame digest,
effective experiment, actual adapter, executable identity and request options.
It also records elapsed time, peak working set and loaded modules at completion.
The parent verifies artifact sizes and hashes before accepting a result.

Optional analysis:

| Option | Effect |
| --- | --- |
| `--shader-outputs` | Evaluate the original PS with the independent C++ DXBC executor, measured private quad inputs and before-primitive resource snapshots |
| `--output-merger` | Include shader evaluation and compare the selected-sample merger result with private GPU storage |
| `--primitive-identity` | Observe conditional system IDs through repeated complete original-PS draws and retain all matching candidate ordinals |
| `--experiment project.json` | Apply the saved experiment before analysis; its digest and effective settings participate in result identity |
| `--max-bytes N` | Bound individual readback/replay payload and shader retention budgets; this is not a total process-memory limit |
| `--max-records N` | Bound retained candidate records; overflow fails instead of truncating history |
| `--max-steps N` | Bound global private shader execution; requires shader outputs or output-merger evaluation |
| `--timeout N` | Bound the isolated Worker lifetime in seconds; the draw analysis also retains its 30-second internal deadline |

Candidate observation currently has an additional fixed 2,048-record capacity.
Identity observation can require 36 complete draws. Expensive draws may therefore
exhaust the internal deadline even with a longer Worker timeout. Cancellation,
timeout or failure does not publish a successful `result.json`; retry with a
fresh output directory. Unsupported diagnostic inputs or shader instructions
fail explicitly. There is no fallback backend.

## Interpretation

Records have kind `private_primitive_storage` and geometry-storage order.
They are observations of isolated replays, not a recovered original fragment
schedule. `original_fragment_eligibility`, unavailable instance identity and
unknown write/failure information remain null. Unchanged bytes do not prove that
no write occurred. `fragment_history_available` remains false.

Optional shader results use measured pre-depth raster inputs and resource
snapshots from private before-primitive checkpoints. Conditional system-ID
equality can associate one or more candidates; duplicate IDs remain ambiguous.
A successful identity predicate does not establish original quad identity,
execution order, helper invocation or instance. Read the scope and validity
fields before consuming these observations.

`--history-scope events` continues to provide event snapshots for CPU writes,
clears, copies, resolves and draw/dispatch effects. Draw-only options are rejected
in that scope rather than ignored. Calling `history` without an explicit scope
and public `debug-pixel` remain gated until complete fragment/debugger parity is
implemented. Existing GUI fragment-history/debug routing is not switched by
this incremental CLI integration.
