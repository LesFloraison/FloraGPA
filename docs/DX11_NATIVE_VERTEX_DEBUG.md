# Native vertex execution debugging

The native analysis Worker now accepts `debug-vertex` and exports an independent
DXBC execution journal, the effective shader and debug metadata. The native
debugger tab supports VS and CS selection. Existing legacy debugger features
remain available while full parity and removal gates are open.

```text
FloraGPA.Cli analyze <frame.gpa_frame> debug-vertex --gpa-event <event> --vertex <reference> --instance <instance> --gpu-vendor <vendor> --out <new-directory>
```

`--vertex` is the zero-based draw input reference before cut-index removal;
`--instance` is relative to the draw's instance range. Both default to zero.
The actual index, base vertex, input buffer offsets, instance offsets and input
conversions come from the effective prepared GPA draw. Direct and indirect
draws use the existing native vertex input observation path. Experiments apply
inside that observation scope. Cut references, disabled/skipped draws and
out-of-range selections fail explicitly.

`--index`, when present, overrides the per-vertex attribute index after all draw
offsets. System IDs and per-instance attributes retain the original selection.
Version 2 results distinguish the observed index from the override, while old
version 1 sessions retain their index-assertion contract. See
`DX11_VERTEX_INDEX_OVERRIDE.md` for validation and remaining cut/missing-index
limits. Use `--warp` for an explicit software adapter. Input, step
and journal budgets are controlled by `--max-bytes`, `--max-steps` and
`--max-trace-bytes`. Worker cancellation and timeouts retain the existing job
isolation and completion-publication protocol.

## Journal and session identity

The common `flora.dxbc-execution-trace/1` journal has a VS header with scope
`one_vertex` and execution model `independent-vertex-invocation/1`. Its only lane
is zero. It records the input reference and instance without synthetic compute
group/thread coordinates. Initial raw register bits and validity, constants,
private resources, source metadata, instruction identities and every reversible
step use the same project-owned records as CS. No original GPU invocation order
or schedule is claimed.

The reader checks stage-specific header invariants and rejects shared group
storage, barriers or pixel discard state in a VS journal. The debug model checks
the exact shader digest, program stage, instruction words and source mappings.
VS terminal state has no invented source instruction. The same source navigation,
breakpoints, watches and register presentation are used for VS and CS, with
separate native configuration identities. Older CS journal/configuration formats
remain accepted under their existing checks.

`flora.native-vertex-debug/1` metadata binds the eligible prepared draw to the
journal and effective shader. Session loading verifies artifact digests, selected
reference/instance, draw bounds, shader, frame and adapter identity before
presenting a model. A stage or selection change invalidates the displayed session
and rejects stale completion. Native session archives retain all six existing
entries and can be loaded independently of the capture for inspection.

## Validation

Nine affected suites pass 296 checks with no failures or skips. They retain the
existing CS tests and add VS source/configuration round trips, exact output bits,
direct/indirect Worker and CLI jobs with experiments, malformed VS journals,
selection/budget rejection, and VS/CS main-window cancellation and retry on
NVIDIA, Intel and WARP. An initial compute preparation regression was detected
by the unchanged CS Worker and GUI tests and fixed before the final run.
The eight added malformed-journal rows ran after the common suite pass.

All nine selected GF2/BF1/Helldivers jobs match the previous independent probe's
complete final register state and original-shader native output components.
They retain the same frame, shader, adapter and invocation identities. The
validator independently applies and reverses register changes, then checks
session export and C++ reload. All nine background-operation archives also
pass independent reload. Loaded-module reports for these jobs contain neither
RenderDoc nor the original GPA private modules.

These real shaders have zero mapped source steps. Source/watch/configuration
behavior is covered by the debug-enabled synthetic VS fixture; these captures
cannot prove source-level parity for arbitrary shaders.

Regenerate the input reference on the current adapter with
`FloraDxbcVertexInputProbe --compare-native` as described in
`DX11_DXBC_PRECISION.md`, then validate the completed native job:

```powershell
python tools/validate_native_vertex_debug.py <job-directory> <input-reference.json> `
  --session-probe build/vs2022/Release/FloraNativeDebugSessionProbe.exe `
  --out artifacts/dx11-native/vertex-validation.json
```

Single-run measurements, including the whole replay Worker:

| Selected event | Steps | Journal MiB | Worker analysis ms | CLI wall seconds | Peak working set GiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| GF2 113 | 12 | 0.54 | 132–446 | 1.20–1.50 | 0.18–0.30 |
| BF1 876 | 58 | 5.40 | 876–1483 | 10.73–12.55 | 1.61–1.75 |
| Helldivers 200 | 42 | 37.03 | 1175–1592 | 11.23–11.69 | 1.61–1.74 |

CLI wall time includes input validation and Worker coordination outside the
analysis timer; the difference is not attributed solely to serialization.
Background probe terminal/rewind operations finish below 4 ms and export below
101 ms, with observed heartbeat gaps below 2.6 ms. In those short real traces,
rewind finished before the 5 ms cancellation request, so those particular runs
do not establish cancellation behavior. Synthetic main-window and operation
tests cover cancellation. These are scoped measurements, not baseline performance
acceptance or repeated-run memory-growth evidence. Exact timings, identities,
artifact hashes and intermediate failures are in
`research/dx11-native-vertex-debug-evidence.json`.

## Remaining gates

The input helper now submits only the selected point and retains whole-draw
acquisition for comparison; complete bound buffer snapshots remain budgeted.
See `DX11_SELECTED_VERTEX_INPUTS.md`. Index overrides are available for valid
selected references as described in `DX11_VERTEX_INDEX_OVERRIDE.md`; complete
cut/missing-index compatibility,
remaining DXBC semantics and precision combinations, complete legacy debugger
configuration migration and broad source/behavior parity are not established by
this checkpoint. Full PS input/quad discovery, fragment history, P7 dependency
removal and P8 standalone packaging/acceptance remain required.
