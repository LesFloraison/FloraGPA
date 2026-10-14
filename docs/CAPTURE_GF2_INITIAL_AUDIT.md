# GF2 initial MSAA color snapshot audit

The selected color resource's complete two-sample initial contents remain
byte-identical across the tested output round trips. Nevertheless, its first
Draw output still changes between controls. This rules out a difference in
the observed initial color bytes as a necessary explanation in these runs;
it does not establish the cause or prove equality of every Draw input or
internal GPU representation. The current six-frame gate remains **failed**.

## Deferred initial-state observation

The development-only `FloraReplayDrawRepeatProbe` adds `--audit-initial`.
Before the original Draw it retains the existing selected-color checkpoint
and a second same-descriptor snapshot taken after optional output restoration.
It performs no sample readback at that point. After all original/restored Draw
observations, it preserves the last output and reads every sample from both
initial snapshots through the existing integer-bit MSAA reader. It compares
the complete byte arrays and records hashes, lengths, difference counts and
the first sixteen differing byte values.

The reader's RGBA16 typeless/float family uses RGBA16_UINT views for selected
samples, preserving component bits instead of converting half-float values.
Sampling each snapshot temporarily copies it into the selected resource.
After the audit, the saved last output is restored; the selected final sample
is rehashed and required to equal the last recorded Draw plane. Native pipeline
bindings must also match. This final check covers the selected output sample;
it does not independently rehash every other final output/sample.

`--initial-restore other-color` restores bound color outputs except the selected
one. Here it restores resources 415 and 621, while `selected` restores 411 and
`depth` restores 419. Existing original shader and Draw arguments remain intact.
This separates the selected-color round trip from adding copies of other colors,
but the operations still change allocation, command history and scheduling.

Supplemental snapshot storage has a combined 512 MiB logical bound and requires
one layer/mip. In these runs, immediate audit reserves two textures' worth of
additional storage (214,990,848 bytes); deferred audit reserves 429,981,696 bytes
and reuses its last pending output snapshot for preservation. The bound excludes
the original checkpoints, other replay resources, reader temporaries and driver
allocation overhead. It is not a limit on total process/GPU memory.

## Initial contents and observed Draw results

All twelve processes use the unchanged frame SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`,
DrawIndexed 5651 `(7401,0,0)`, Intel feature level 11.1/debug flag 2, and resource
411/sample 1 at `(1596,1519)`. Ten processes audit initial contents; two retain
unaudited controls. Twenty complete before/after sample pairs (forty initial
plane observations) compare byte-for-byte equal. Their hashes also match the
independently rehashed [native fixture](CAPTURE_GF2_NATIVE_LEVEL.md) initial files:

| Sample | Bytes per plane | Initial SHA-256 |
| --- | ---: | --- |
| 0 | 53,747,712 | `a1ab425473351b59ba36f1d4b7a8a9b0c4cc3ca4d8a6dcf2375e4a54340ee6e1` |
| 1 | 53,747,712 | `4c38dcaea62b4c7d7bfc5ee198a7454e1eee3246be4da8cb89bb6227ed91c780` |

The twelve processes make 36 post-Draw observations: twelve original Draws and
twenty-four restored Draws. Results are:

| Initial restoration | Processes | Audited processes | Original lower / higher / additional variant |
| --- | ---: | ---: | ---: |
| None | 3 | 2 | 3 / 0 / 0 |
| Selected color | 4 | 4 | 0 / 4 / 0 |
| Other colors | 3 | 2 | 2 / 0 / 1 |
| Depth | 2 | 2 | 2 / 0 / 0 |

Every restored Draw has the existing higher plane hash
`39c8da69214c57299c813e83bb9e0b8e7c29bcd42d07a541e4cc416882a939c5`.
Seven original Draws have the lower hash
`d9e2edda8025bbe17034e5c6000d8e06790476e1e061323836ab2b7b7f21f2aa`;
four have the higher hash. Their selected half words remain
`[9109,9233,8865,15151]` and `[9127,9247,8891,15143]` respectively.
The selected-color controls include one full raw export and one deferred-readback
process. All ten audits restore the last selected plane exactly, and native
pipeline checks pass. Scoped Draw/readback/audit debug queues are empty; each
process separately retains 608 prefix messages.

## Additional other-color variant

`audit-2-other-color` has original plane hash
`d5ac6de67dac7c0138b27c45694ef098b5929de1d28f09f7d9ef6aafc2486481`.
Its two restored Draws both produce the known higher plane. The runtime reports
exactly twelve differing bytes, all of which fit in its retained difference list:

| Pixel | Additional original half words | Higher restored half words |
| --- | --- | --- |
| (1596,1519) | 9109, 9233, 8865, 15151 | 9127, 9247, 8891, 15143 |
| (1597,1521) | 9428, 9476, 9295, 15129 | 9413, 9457, 9281, 15128 |
| (1598,1523) | 9572, 9621, 9441, 15111 | 9541, 9580, 9404, 15111 |

This summary-mode process did not save a direct raw plane. The offline
`reconstruct.py` starts from the retained, rehashed higher plane, checks every
reported restored byte, and applies all twelve original byte values. The resulting
53,747,712-byte file hashes exactly to the original runtime plane hash. It is
preserved as `reconstructed-initial-plane.bin` with
`derived_not_direct_readback: true`. It is a verified reconstruction, not a new
GPU execution, a direct raw readback, or an accepted capture. Neither these
derived bytes nor prior reference pixels enter the replay process.

Both complete initial color samples in this anomalous process still match the
before/after and native-fixture hashes. The selected point alone would miss its
other two differing pixels. The variant is retained as unexplained evidence;
no tolerance or acceptance classification changes.

## Verification, storage and remaining scope

The development target builds and source-boundary CTest passes. Three new
refusals cover duplicate audit flags, an unexpected audit argument, and the
combined deferred/audit storage bound. The first two fail before output creation.
The budget case runs the prefix, leaves an incomplete report with no selected-Draw
observations or plane files, and rejects before supplemental snapshot allocation.
All GPU processes ran serially and completed before the interrupted turn resumed;
the continuation only verifies existing results and prepares documentation.

The full raw selected-color control writes three unique planes (two initial,
one post-Draw). They compare byte-for-byte against retained files from the earlier
export and checkpoint stage.
Summary-mode hashes and all reported byte differences are checked against raw
or explicitly reconstructed planes. Transparent compression preserves these
three direct planes plus the reconstructed variant, saving 82,763,776 bytes of
allocation with hashes verified before and after. No old evidence or source
frame was deleted.

Raw scripts, reports, the tested binary and the reconstructed plane remain under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-initial-audit-20261014`.
The direct raw control is under
`D:/CDXrepo/FloraGPA-Cpp/artifacts/capture-gf2-initial-audit-raw-20261014`.
The [baseline](capture-gf2-initial-audit-baseline.json) records both locations,
their manifests and each process's scope.

```text
cmake --build --preset capture-full-release --target FloraReplayDrawRepeatProbe
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --summary-only --audit-initial --initial-restore selected
FloraReplayDrawRepeatProbe FRAME NEW_OUT 5651 411 1 32902 2 --debug-device --summary-only --audit-initial --initial-restore other-color
```

Production Capture/Replay/Format source is unchanged. The production CLI hash
remains `215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
No fresh whole-frame/golden gate is claimed. This stage proves the observed
initial contents of resource 411, not all color/depth/input resources in these
same processes, runtime ISA, GPU representation, or a production repair.
Formal six-frame acceptance and overall delivery remain incomplete.
