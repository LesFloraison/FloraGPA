# GF2 triangle replay after the original prefix

The current frame's selected sample changes from the lower value after the
original complete Draw to the higher value after restoring outputs and drawing
again. This occurs for both the complete index range and original triangle 2443
alone. Four further processes retain that change when all sample readback is
postponed until after every Draw. Intervening sample-readback helpers are thus
not necessary for the observed selected-point change in these runs. The formal
six-frame gate remains **failed**; this is not a production fix or a claim that
all restored images are stable.

## Scoped diagnostic implementation

The existing `FloraReplayDrawRepeatProbe` still runs the original frame prefix
through DrawIndexed 5651. Immediately before that Draw it retains the three bound
color resources and depth/stencil resource in same-descriptor GPU copies. Its
first observation is **after the original complete Draw**, not the pre-Draw
checkpoint. Later iterations restore those four resources and issue the selected
Draw again on the same context, using the same original shaders and bindings.

`--indices FIRST COUNT` now restricts only these restored iterations to complete
triangles within the original Draw. The first invocation always retains original
arguments `(7401,0,0)`. Triangle 2443 becomes `(3,7329,0)`; an explicit complete
range remains `(7401,0,0)`. The tool records both argument sets, actual device
level/flags and native pipeline state. Getters verify identical pipeline bindings
after each Draw and, in the final binary, after each readback. The selected
pipeline matches the preceding native-pipeline checkpoint record.

`--defer-readback` allocates one same-descriptor selected-color snapshot per
observation before the initial Draw. It copies that surface immediately after
each Draw, and executes all restored Draws before calling the sample reader.
It then copies each retained snapshot back to the selected resource solely for
sampling. These ordered GPU copies preserve each earlier observation. The last
snapshot is restored last. No sample-reader shaders or CPU sample readbacks run
between Draws in this mode. Allocation, copies and output unbinding/rebinding
still change execution conditions and are not claimed to be invisible.

Deferred mode restricts the selected resource to a single layer/mip and bounds
the additional logical sample storage at 512 MiB. These experiments retain three
snapshots, totaling 322,486,272 logical bytes. This is not a bound on the whole
process or on driver allocation overhead. Existing shader-model-4, VS/PS,
no-GS/tessellation/SO/predication restrictions remain. Neither mode modifies
production capture, format or replay source.

## Selected sample and full-plane results

All processes use the unchanged frame SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`,
Intel 11.1/debug device, resource 411, sample 1, pixel `(1596,1519)`.
The original Draw gives `[9109,9233,8865,15151]` in all eleven processes. Every
restored Draw gives `[9127,9247,8891,15143]` at that point.

| Readback mode / restored range | Processes | Original complete Draws | Restored Draws |
| --- | ---: | ---: | ---: |
| Immediate / complete | 4 | 4 | 8 |
| Immediate / triangle 2443 | 3 | 3 | 6 |
| Deferred / complete | 2 | 2 | 4 |
| Deferred / triangle 2443 | 2 | 2 | 4 |

The 33 complete selected-sample planes are hashed and compared independently.
All eleven original complete-Draw planes have hash
`d9e2edda8025bbe17034e5c6000d8e06790476e1e061323836ab2b7b7f21f2aa`.
All twelve restored complete-Draw planes have hash
`39c8da69214c57299c813e83bb9e0b8e7c29bcd42d07a541e4cc416882a939c5`.
They differ from the original complete Draw in exactly four bytes at the selected
pixel, one byte in each half-float component. This is a prefix/Draw result, not
a new executed whole-frame acceptance result.

Eight of ten restored triangle-only planes exactly match the preceding
[independent native triangle](CAPTURE_GF2_CURRENT_PRIMITIVE.md) plane hash
`3b44459ed71aec47595e416ca241c00740fa181bf478e8971554c439e30a0603`.
Both restored iterations in `triangle-1` instead have hash
`d9105c1b71bc6853eb293fdd9450b867734662b4f4b8eb7bc53787cf871b427f`.
These two variants differ in exactly one byte at `(1571,1461)`: half words change
from `[11702,10154,6537,15359]` to `[11702,10154,6536,15359]`. The selected
`(1596,1519)` sample remains identical. Both variants differ from the original
complete-Draw plane in 45,812 bytes, as expected for a partial index range;
equal difference counts do not establish equal images.

That additional variant is unexplained and retained. Pre-Draw checkpoint planes
were not independently exported in these processes, so their byte equality at
the other coordinate is unproven. The data do not establish whether this comes
from the earlier prefix, output restoration or the isolated Draw. An initial
verifier incorrectly assumed every isolated plane matched the prior native
plane and failed. Its script, failing log and status are retained. The final
verifier explicitly reports the second exact hash and the one-bit discrepancy;
it does not relax a tolerance or classify the frame as accepted.

All selected Draw/readback debug queues are empty; deferred submission queues
are also empty. Each process separately retains the existing 608 prefix debug
messages. Native pipeline equality does not prove equality of all resource
contents or hidden execution conditions. The stable selected point must not be
used as proof of whole-plane determinism.

## Validation, storage and reproduction

The first six processes use the index-range binary. The final binary adds deferred
readback and post-readback binding checks; one additional immediate complete-range
process verifies the same selected result and complete-plane hashes. All GPU work
runs serially. Fourteen refusal cases cover unknown/duplicate/incomplete options,
negative/overflowing and non-triangle ranges, zero/out-of-bounds ranges, existing
output preservation and the deferred snapshot budget. The budget failure leaves
an incomplete report with no sample files. Source-boundary CTest passes.

Low disk space was handled by transparent filesystem compression of this stage's
22 saved sample files. SHA-256 was verified before and after every operation;
all paths and bytes remain intact. The two storage records report 455,127,040
bytes of reduced file allocation. No source frame, old workspace or unique
evidence was deleted. Full selected-plane bytes remain available, including the
unexpected variant.

```text
cmake --build --preset capture-full-release --target FloraReplayDrawRepeatProbe
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --indices 0 7401
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --indices 7329 3
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --indices 7329 3 --defer-readback
```

Raw reports, frozen binaries, scripts and sample planes are retained under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-prefix-triangle-20261014`.
The [baseline](capture-gf2-prefix-triangle-baseline.json) pins their manifest.
Production CLI hash remains
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
Legacy migration parity and the failed formal acceptance status remain unchanged.
