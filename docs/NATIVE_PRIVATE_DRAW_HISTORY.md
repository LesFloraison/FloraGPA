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
| `--return-count` | Count returning samples at the selected pixel/layer/sample using a private additive target; retain the complete diagnostic report |
| `--shader-outputs` | Evaluate the original PS with the independent C++ DXBC executor, measured private quad inputs and before-primitive resource snapshots |
| `--output-merger` | Include shader evaluation and compare the selected-sample merger result with private GPU storage |
| `--primitive-identity` | Observe conditional system IDs through repeated complete original-PS draws and retain all matching candidate ordinals |
| `--primitive-presence` | Test every distinct candidate system ID twice using accumulated color predicates on complete private draws |
| `--primitive-tests` | With `--primitive-presence`, repeat full draws with depth, stencil, both depth/stencil, alpha-to-coverage, or the sample mask disabled independently |
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

Presence observation requires a floating-point PS output and a blendable selected
RTV. Each distinct known candidate system ID adds two complete draws and four
selected-pixel readbacks. It shares the history deadline and retains reports within
the payload limit. A depth-only selection or missing PS is reported as not
applicable; unsupported color formats fail explicitly. This diagnostic does not
change the supported formats of ordinary storage observations.

Test variants require presence observation and add ten complete draws per distinct
candidate ID, for twelve total presence draws and twenty-four texel readbacks.
Each variant starts from the same before-event storage and retains two repetitions,
the actual initial/effective test state, and a nullable comparison with preserved
tests. The variants share the existing deadline and payload limits. They can change
depth/stencil writes and interactions between primitives. A changed color predicate
is therefore counterfactual evidence, not an original fragment's failure reason.
No test is identified as a cause merely because disabling it changes the result.

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

`return_count_observation` reports `not_requested`, `not_applicable`, `observed`
or `unknown`. Depth selections with bound color outputs and color selections
without a PS remain not applicable and retain a null count. Depth-only draws can accumulate
counts through a private color transport, including draws without a PS. A
successful count belongs to the private diagnostic draw; it does not establish original fragment identities, instance,
order or resource modifications. The complete observation retains the GPA event,
frame hash, shaders, actual adapter, source view, color routing slots when
applicable, and raw count words. Absolute resource layers are translated to the selected RTV's relative
layer. No geometry-record count or occlusion query is substituted for this value.

Counting adds one complete draw for an applicable color or depth selection. It
shares the
history deadline and cancellation signal and bounds transport and retained
observation payloads with `--max-bytes`. Existing readback telemetry counts
selected original-storage snapshots; the count transport has its own report.
Color counts require a 2D RTV, including array and MSAA variants. Depth-only
counts use a 2D DSV and require no bound RTVs, no occupied UAV slot zero, no
original color export and disabled alpha-to-coverage. A missing PS is replaced
with an owned empty shader before adding the diagnostic output. The resulting
count is not an original PS invocation count. Depth transport records source
mip and first layer; its selected layer is relative to the DSV. See the
[depth count evidence](research/DX11_NATIVE_DEPTH_RETURN_COUNTS.md).
Integer RT0 alpha-to-coverage may use RT1 to preserve RT0 behavior; an application
UAV at slot 1 prevents that route and fails explicitly. Unsupported requests do
not publish a successful result or switch backend. See the
[Worker integration evidence](research/DX11_HISTORY_RETURN_COUNT_JOBS.md).

`primitive_presence_observation` uses RGB maximum accumulation, so a later
nonmatching return cannot erase an earlier positive marker. Repeated agreement
sets `color_completion_observed`; disagreement or uninterpretable storage leaves
it null. Each record references the observation for its system ID. Duplicate IDs
retain every matching ordinal, and unknown IDs remain unknown. A false result
does not identify a particular depth/stencil/discard failure. A true result does
not identify a unique original fragment, order, instance or original write: the
selected color output's blend and write mask were deliberately changed. Shader
alpha, original depth/stencil processing and the other target descriptors remain
in use. See [the presence study](research/DX11_NATIVE_PIXEL_PRESENCE.md).

`--history-scope events` continues to provide event snapshots for CPU writes,
clears, copies, resolves and draw/dispatch effects. Draw-only options are rejected
in that scope rather than ignored. Calling `history` without an explicit scope
and unscoped `debug-pixel` remain gated until complete fragment/debugger parity is
implemented. Explicit [private primitive debugging](NATIVE_PRIVATE_PIXEL_DEBUG.md)
is available through the CLI and Native Execution panel. Existing GUI fragment
history and full pixel debugger routing remain available during migration.
