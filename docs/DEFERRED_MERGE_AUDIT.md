# Deferred merge: replay fidelity and capture fidelity

Reviewed **2026-10-04**. This M3 batch adds 64 unmodified original captures from
two child deferred contexts merged into a third deferred context. It adds
development probes and acceptance evidence; production replay is unchanged
from `6bc3cca` / package `out/FloraGPA-cb-lifetimes-final-20261004`.

**All 64 files are expanded immediate streams. None establishes traditional
Command List resource/Execute support.** Thirty-two faithfully save the test
workload's resource results and image. Thirty-two instead save an already
incorrect workload produced under the original GPA shim. These outcomes must
not be conflated when reporting replay success.

## Reproducible workload

`FloraDeferredMergeProbe` is a development-only C++ target excluded from the
default build. It creates actual DX11 deferred contexts and native command lists.
Merging a child list onto another deferred context is a documented use of
[ExecuteCommandList](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).

Each child dispatch executes this single-thread recurrence on four uints:

```text
result = [result.x * factor + input + tag, result.y + 1, input, factor]
A: factor/tag = 3/1; B: factor/tag = 5/2
Parent list = A, B, A
Immediate input before three executions of parent = 2, 5, 11
```

The producer independently checks the results against:

```text
Parent execution 1: [    60, 3,  2, 3]
Parent execution 2: [  2817, 6,  5, 3]
Parent execution 3: [126996, 9, 11, 3]
```

The pixel shader draws green only when all four final words match. The producer
also saves every intermediate result, all 12 frame images and a separately
written/read sentinel `[101,103,107,109]`. An immediate dynamic-CB upload of
`17/99` precedes every parent execution to expose resource-version mistakes.

Mode bits independently select:

| Bit | Off | On |
|---|---|---|
| 0 | Child Finish restore FALSE | Child Finish restore TRUE |
| 1 | Execute child on parent restore FALSE | Restore TRUE |
| 2 | Parent Finish restore FALSE | Parent Finish restore TRUE |
| 3 | Execute parent on immediate restore FALSE | Restore TRUE |
| 4 | Record lists before frames | Re-record every frame |
| 5 | Shared dynamic CB, child WRITE_DISCARD versions | Separate immutable CB per child |

All 64 uninjected runs pass. The immutable variant isolates child Map/version
handling while retaining the same shader, inputs, three contexts and execution
order. It is a separate captured workload, not a patched copy of a failing file.

## Original capture-side discrepancies

In modes 0–31 the injected producer fails resource/image checks in **all 12
frames**, including frames before the capture request. The three observed
results are `[2,3,2,0]`, `[5,6,5,0]`, `[11,9,11,0]`. Thus all nine dispatches
execute, but the observed factor is zero. Each capture stores nine **complete
16-byte zero writes** for the child dynamic CB. The three immediate uploads
remain `[17,99,0,0]`. These are saved bytes, not missing data filled by FloraGPA.
The original player also reproduces the red image.

Modes 32–63 preserve the expected result bytes and green image. Both variants
retain independent native binding checks, which expose additional capture-frame
state discrepancies:

| Observation | Modes | Failures |
|---|---|---|
| Immediate post-Execute restoration | Bit 3 set | 3 in frame 6 per mode, 96 total |
| Parent post-merge default state | Bit 4 set, bit 1 clear | 3 in frame 6 per mode, 48 total |
| Parent post-Finish restored state | Same as above, bit 2 set | 1 in frame 6 per mode, 8 total |
| Child Finish checks | All modes | 0 |

The checks compare returned native CS/CB identities. They do not prove every
pipeline slot. Other frames and all uninjected state checks pass. A successful
image must not erase these recorded state failures.

The dynamic producer intentionally returns exit 1 for its data mismatch. Its
completed reports and delivered originals are retained as capture-side evidence;
this exit is not counted as a FloraGPA replay rejection or silently accepted as
a correct workload. The full producer harness also returns 1 for this reason.

## Static evidence and its limits

Read-only Ghidra inspection of the installed shim pins SHA-256
`cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31`.
Relevant RVAs:

| Address | Observation |
|---|---|
| `4cef00`, slot 92 / `2d9a10` | Deferred manager merge enumerates child command objects and forwards them through manager methods |
| `2d9a10`, case `30a5` | Forwards the child's saved Map structure to manager slot 72 |
| `2d86c0` | Map manager allocates a new watched-memory block for WRITE_DISCARD and replaces the supplied data pointer |
| `2d8b80` | Unmap manager handles the watched-memory entry and appends Unmap |
| `4cf848`, slot 92 / `2e4be0` | Immediate manager expansion, consistent with the preceding batch |

This forwarding/allocation sequence is consistent with losing child mapped
writes during merge. The exact internal data-loss mechanism remains an inference
from decompilation, independently supported by the native/injected/immutable
controls and serialized zero bytes. No original DLL was patched. The raw table
dump includes neighboring slots; entries past identified methods are not claimed
to be valid functions. No Execute case `30d1` was observed in the recovered merge
switch. Actual files contain zero list resources `9a` and zero Execute records
`41`/`30d1`.

## Acceptance

The [corpus](deferred-merge-corpus.json) pins each capture and explicitly marks
`native_workload_fidelity` separately from its captured-image reference. The
[gate](deferred-merge-gate.json) uses strict hashes; there is no tolerance change.
The [baseline](deferred-merge-baseline.json) pins tools and retained evidence.
Here `native_workload_fidelity` classifies resource/image results; the native
binding discrepancies above remain a separate dimension.

| Completed measurement | Result |
|---|---|
| Independent / original repeated replay | 128 / 128 runs; 64/64 captures match their captured output |
| Strict intermediate buffer exports | 384 |
| Immutable image / dynamic buffer negative controls | 288 / 18 |
| Producer frames / result readbacks / sentinel readbacks | 1,536 / 4,608 / 1,536 |
| New checker / existing compatibility-tool CPU tests | 8 / 18 pass |

This adds 64 unique captures to the preceding 406 registrations / 396 unique
hashes: **470 registrations / 460 unique**, with 448 replay-positive registrations
(438 unique) and 22 rejection files. Thirty-two new replay positives carry the
capture-side data mismatch. No old full GPU matrix, Qt regression or release
repackaging was repeated because production code is unchanged.

For each original, replay twice with FloraGPA and twice with the original kernel;
compare raw images with the **captured application frame**, and export all four
result words after each of the three outer executions twice. For the 32 immutable
cases, independently disable each of nine Dispatches: every image must become
red. Red is already the dynamic capture's normal output, so dynamic modes 0 and
31 instead disable each Dispatch and inspect the actual buffer: `[11,8,11,0]`
must replace `[11,9,11,0]`.

Equal observed pixels do not certify identical adapter/driver settings in the
original kernel. Its configuration remains unproven. The independent replay
dependency audit continues to prohibit GPA/Python runtime dependencies.

The new CPU checker tests reject altered CPU expectations, relabeled bad
captures, changed result words, truncated readbacks, image-only disagreement,
missing steps and state failures moved to the wrong flag/frame boundary.

## Reproduction and next boundary

Build `FloraDeferredMergeProbe`, then use:

```powershell
python tools/capture_deferred_merges.py --exe <probe.exe> --shim <shimloader64.dll> --out <fresh-producer>
python tools/catalog_deferred_merges.py --exe <FloraGPA.Cli.exe> --producer <fresh-producer> --reference-python <old-standalone-dir> --out <fresh-api> --corpus <fresh-corpus.json>
python tools/validate_corpus.py --exe <FloraGPA.Cli.exe> --manifest <fresh-corpus.json> --captures-root <producer-parent> --out <fresh-validation> --repeat 2 --timeout 90 --oracle-tools <old-tools-dir>
python tools/validate_deferred_merge_evidence.py --exe <FloraGPA.Cli.exe> --producer <fresh-producer> --corpus <fresh-corpus.json> --api <fresh-api>/api-evidence.json --validation <fresh-validation> --out <fresh-audit>
python -m unittest discover -s tests -p test_deferred_merge_evidence.py -v
```

GPU steps must remain serial. A new capture has a new hash and requires a new
catalog; never overwrite existing captures/manifests to fit an earlier baseline.
All binaries, captures and decompilation outputs remain outside Git.

This batch broadens verified **expanded-stream** coverage and isolates a real
capture boundary. It does not resolve the traditional list identity domain,
build order, resource-version ownership or event mapping. Those still require
an unmodified original retaining traditional list records and original execution
evidence. Production rejection of unresolved list layouts remains unchanged.
