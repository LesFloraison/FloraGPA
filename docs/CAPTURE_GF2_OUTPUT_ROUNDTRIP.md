# GF2 initial GPU submission and output-copy controls

Completing the queued GPU prefix does not remove the selected current-frame
discrepancy in three explicit-completion runs. Copying the selected MSAA color
resource back from its pre-Draw checkpoint does change the first Draw's observed
plane in all five such controls. Two complete-output round trips also produce
the higher plane; two depth-only round trips retain the lower plane. One of
thirteen no-round-trip originals independently produces the higher plane.
These are diagnostic associations, not a proven cause or production fix.
The current six-frame acceptance gate remains **failed**.

## Submission controls

The development-only `FloraReplayDrawRepeatProbe` adds
`--initial-submit none|flush|event|wait`. The operation is immediately after the
existing pre-Draw output snapshots and output rebinding, before the original
Draw and before any optional CPU delay:

| Mode | Operation | Completion established by this mode |
| --- | --- | --- |
| `none` | No added submission operation | No |
| `flush` | One `Flush` | No |
| `event` | End a private EVENT query and Flush | No |
| `wait` | End a private EVENT query, Flush, then poll it | Yes, on successful query completion |

EVENT objects are created before replay begins, in both `event` and `wait`
modes. `wait` polls with `D3D11_ASYNC_GETDATA_DONOTFLUSH`, checks HRESULT and the
returned BOOL, checks device removal while pending, and has a five-second
pending-wait bound. Reports retain poll count, last HRESULT, elapsed steady-clock
time and whether completion was actually observed. Query errors, unexpected
results or timeout leave an incomplete report. The normal-device runs do not
exercise timeout or device removal; those branches are not claimed as tested.

Microsoft documents that [Flush submits work asynchronously](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush),
and that [EVENT completion requires S_OK and TRUE](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query).
Consequently, this probe distinguishes submission from observed completion;
neither Flush alone nor the preceding CPU-delay experiment proves completion.

The first eight processes run modes `none,flush,event,wait,wait,event,flush,none`.
Every first Draw produces the lower plane and every restored Draw the higher
plane. Both waits observe successful completion, after 12 and 14 polls and
123,722,400 and 128,104,400 ns respectively. A final-binary wait control also
observes completion (12 polls, 120,793,800 ns) and retains the same difference.
No sample-reader shaders or resource readbacks are added before these first
Draws. This narrows the current checkpoint experiment; the older
[per-command synchronization experiment](CAPTURE_REPEATABILITY.md#explicit-gpu-completion-experiment)
uses a different frame and remains separate evidence.

## Output round trips

`--initial-restore none|selected|depth|all` optionally copies outputs back from
the existing pre-Draw snapshots **before the first original Draw**. Source and
destination are distinct same-descriptor resources. The selected color resource
is 411; depth is 419. `all` restores three color resources and the depth resource.
The report records the mode and actual restored-output count (0, 1, 1 or 4).
A nonempty mode that selects no bound output fails explicitly.

This operation precedes the selected submission mode. The experiment does not
replace shaders, blend state or Draw arguments. It does add GPU copies and can
change execution conditions. Subsequent restored iterations retain the prior
behavior of restoring all four outputs before drawing again.

All 22 processes use the unchanged current frame SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`,
DrawIndexed 5651 `(7401,0,0)`, Intel feature level 11.1/debug flag 2, resource
411/sample 1 and pixel `(1596,1519)`. Results across submission, round-trip and
follow-up controls are:

| Initial round trip | Processes | Original lower / higher | Restored lower / higher |
| --- | ---: | ---: | ---: |
| None, including submission controls | 13 | 12 / 1 | 0 / 26 |
| Selected color only | 5 | 0 / 5 | 0 / 10 |
| Depth only | 2 | 2 / 0 | 0 / 4 |
| All outputs | 2 | 0 / 2 | 0 / 4 |

The initial round-trip sequence is `none,all,selected,depth,depth,selected,all,none`.
The last `none` process produces the higher original plane and is retained as a
counterexample to deterministic separation. Follow-ups `none,selected,selected,none`
give lower, higher, higher, lower originals. One selected-color follow-up uses
deferred sample readback and also gives the higher original plane. The latter
retains 322,486,272 logical bytes of additional GPU snapshots.

All 66 complete selected-sample observations have one of the existing hashes:

- Lower: `d9e2edda8025bbe17034e5c6000d8e06790476e1e061323836ab2b7b7f21f2aa`,
  selected half words `[9109,9233,8865,15151]`.
- Higher: `39c8da69214c57299c813e83bb9e0b8e7c29bcd42d07a541e4cc416882a939c5`,
  selected half words `[9127,9247,8891,15143]`.

Each plane is 53,747,712 bytes. The independently rehashed retained raw planes
differ in exactly four bytes at the selected pixel; every reported byte count,
offset and old/new byte agrees. All new runs use summary mode and do not save
duplicate raw planes. The probe itself never reads reference pixels.

Native pipeline checkpoints match the prior checkpoint in every process. Scoped
Draw/readback debug queues are empty, as is the deferred submission queue.
Each process separately retains 608 prefix messages, so these are not clean
whole-prefix debug runs.

## Limits and next discriminator

This stage does not independently sample the entire selected resource both
before and after its initial round trip. Copy semantics, post-Draw hash equality
and binding equality are insufficient to prove every initial resource byte or
internal GPU representation. Allocation, command history and scheduling still
vary. The data do not establish a driver compression bug, a shader compiler
bug, or a reason to insert copies into production replay/capture.

The next useful discriminator is to retain both initial selected-color snapshots
and compare complete samples after execution, without inserting sample readback
before the first Draw. It should also distinguish restoring the selected color
from restoring other color outputs. Neither a matching intermediate plane nor
modified execution replaces the two-player whole-frame acceptance gate.

## Validation and reproduction

The development target builds and `capture_boundaries` CTest passes. Eight
refusals cover missing, unknown, case-mismatched and duplicate mode options for
both additions, and fail before creating output directories. All GPU processes
run serially. The first frozen binary performs the eight submission controls;
the final binary adds initial round trips and performs fourteen remaining
processes plus refusals. Reports distinguish their scopes.

```text
cmake --build --preset capture-full-release --target FloraReplayDrawRepeatProbe
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --summary-only --initial-submit wait
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --summary-only --initial-restore selected
```

Raw scripts, reports and both binaries are retained under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-initial-submit-20261014`.
The [baseline](capture-gf2-output-roundtrip-baseline.json) records file identities
and per-process observations. Production Capture/Replay/Format source is unchanged;
the production CLI retains SHA-256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
No fresh whole-frame/golden acceptance sweep or runtime fix is claimed.
