# GF2 initial Draw CPU-delay controls

A one-second CPU delay before the selected original Draw does **not** reliably
remove the current discrepancy. In three delayed fresh processes, one original
Draw produces the higher selected-sample plane and two produce the lower plane.
Three paired zero-delay processes all produce the lower plane. Every restored
Draw produces the higher plane. This small diagnostic experiment does not prove
that waiting causes the change or identify an asynchronous compiler/driver bug.
The current six-frame acceptance gate remains **failed**.

## Diagnostic scope

`FloraReplayDrawRepeatProbe` adds `--initial-delay-ms 0..1000`. After copying the
four original output resources and rebinding outputs, the before-Draw observer
waits on the CPU, then returns to the original replay Draw. It adds no GPU query,
flush or readback during the wait. Requested milliseconds and actual steady-clock
nanoseconds are recorded. This is a scheduling perturbation, not proof that the
preceding GPU work has completed. Production capture and replay are unchanged.

`--summary-only` keeps full-plane SHA-256, byte counts, complete difference counts,
the first sixteen differing offsets and their byte values, native binding checks
and debug messages. It omits plane files and explicitly reports that omission.
The ordinary mode still writes each distinct complete plane. Neither mode reads
reference pixels. Offline verification checks every reported plane hash against
previously retained, independently rehashed complete planes; no new unknown hash
appears in this batch. This does not substitute hash-only summaries for the
formal final-image acceptance tests.

All runs use the unchanged frame
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`,
DrawIndexed 5651 `(7401,0,0)`, original shaders, Intel feature level 11.1 with
debug flag 2, resource 411/sample 1 and pixel `(1596,1519)`. Each fresh process
executes the original Draw followed by two complete restored Draws.

## Results

| Control | Processes | Original lower / higher | Restored lower / higher |
| --- | ---: | ---: | ---: |
| Zero delay, immediate summary | 3 | 3 / 0 | 0 / 6 |
| 1,000 ms delay, immediate summary | 3 | 2 / 1 | 0 / 6 |
| Default options, full raw planes | 1 | 1 / 0 | 0 / 2 |
| 1,000 ms delay, deferred summary | 1 | 1 / 0 | 0 / 2 |

The six paired runs execute in order `0,1000,1000,0,0,1000` milliseconds. The
delayed original higher result is the second process. Measured delayed intervals
range from 1,001,061,900 to 1,010,008,400 ns across all four delayed controls.
No positive result was selected for a new acceptance file.

The 24 observations have only the two existing complete-plane hashes:

- Lower: `d9e2edda8025bbe17034e5c6000d8e06790476e1e061323836ab2b7b7f21f2aa`,
  selected half words `[9109,9233,8865,15151]`.
- Higher: `39c8da69214c57299c813e83bb9e0b8e7c29bcd42d07a541e4cc416882a939c5`,
  selected half words `[9127,9247,8891,15143]`.

Each plane contains 53,747,712 bytes. Independent comparison of retained planes
confirms exactly four differing bytes, all at the selected pixel. Every reported
offset, old byte, new byte and difference count agrees with those planes. The
default-mode new raw planes also compare byte-for-byte with them. The delayed
higher original Draw and its restored Draws therefore have identical complete
selected-sample planes, not merely the same selected pixel.

All native pipeline checkpoints match the prior checkpoint. Selected Draw/readback
debug queues are empty, as is the deferred submission queue. Each process retains
608 prefix messages separately; these are not counted as a clean whole-prefix
debug run. Deferred mode retains the existing 322,486,272-byte logical snapshot
budget. Output copying, allocation and binding operations still perturb execution.

## Verification and limits

The development target builds and the `capture_boundaries` CTest passes. Ten
refusal cases cover missing, negative, fractional, nonnumeric, excessive and
overflowing delays, duplicate zero-valued delay options, duplicate summary mode,
an unexpected summary argument and an unknown option. Every refusal exits with
an error before creating its output directory. All GPU processes run serially.

The production CLI SHA-256 remains
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
No production source changes or fresh whole-frame/golden acceptance results are
claimed. Waiting is not introduced into the application as a workaround. These
results neither establish runtime ISA identity nor exclude other timing or
command-submission effects.

```text
cmake --build --preset capture-full-release --target FloraReplayDrawRepeatProbe
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --summary-only --initial-delay-ms 1000
ctest --preset release -R ^capture_boundaries$ --output-on-failure -j1
```

Raw scripts, eight reports, the tested binary, refusal logs and the two newly
saved complete planes remain under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-initial-delay-20261014`.
Transparent filesystem compression reduces allocation for those two planes by
41,369,600 bytes; hashes were checked before and after. Original frames, earlier
evidence and the old workspace remain intact. The
[baseline](capture-gf2-initial-delay-baseline.json) records file identities and
per-process results. Capture implementation and formal acceptance remain partial.
