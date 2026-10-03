# M2: Context1 resource transfers

Normalized immediate `UpdateSubresource1` (`0x255`) and
`CopySubresourceRegion1` (`0x256`) now execute through the native Context1 API.
The captured copy flags are preserved; these records are resource writes, not
auxiliary observations. Event disabling uses the existing experiment mechanism.
The API inspector exposes their arguments and flags. No new editor or UI layout
is introduced.

## Wire and execution evidence

Read-only Ghidra evidence is retained in `artifacts/m2-transfer1-wrapper.c`:
Context4 wrapper RVAs `13e840` and `13ddc0`, found through string RVAs `4bd770`
and `4bd6c0`. Update stores link/context/destination IDs, subresource, optional
six-uint box, GenData ID, original row/depth pitches, and copy flags. Payload
sizes are 49 or 73 bytes. Region copy stores link/context/destination IDs,
destination subresource and XYZ, source ID/subresource, optional box, and flags;
payload sizes are 57 or 81 bytes. Linked forms `0x3552` and `0x3551` remain
unsupported, as do nonzero links in the normalized immediate records.

`artifacts/m2-transfer1-player.c` contains original initialization/execution
RVAs `1f5f0`/`28fb0` for Update and `1c180`/`266e0` for Copy. They resolve
resource/data versions, query Context1 and invoke vtable offsets `0x3a0` and
`0x398`. This does not establish general version or deferred-list reconstruction.

The [CopySubresourceRegion1 contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-copysubresourceregion1)
adds same-subresource copying subject to `CopyWithOverlap`. Replay checks this
feature before such calls. The
[UpdateSubresource1 contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-updatesubresource1)
allows partial constant-buffer writes; replay checks 16-byte boundaries and the
device's `ConstantBufferPartialUpdate` capability. Unsupported devices receive
an explicit error. Unknown or mutually exclusive flags are rejected.

The [copy flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ne-d3d11_1-d3d11_copy_flags)
are forwarded unchanged. DISCARD does not promise retained contents outside
the written region; the deterministic DISCARD fixtures fully overwrite their
single-subresource destinations. This batch does not invent previous contents
for partial discards. Native format conversion is not substituted for byte copies.

Original GenData is tightly packed even when the application supplies padded
row/slice pitches. Replay calculates checked packed strides, validates the saved
byte length, and passes the packed asset to Context1. Full padded 2D, partial
padded 2D/3D and BC cases establish this with byte oracles. Bounded empty boxes
remain no-ops. The writer saves a 16-byte asset for the zero-width Update fixture;
replay validates its container but does not consume it as image data. Malformed
flags, trailing/truncated payloads, invalid references, subresources, box bounds,
misaligned constant-buffer updates and mismatched asset lengths fail explicitly.
Update semantic diagnostics include the destination resource ID.

## Self-owned, unmodified original captures

`tools/native/transfer1_probe.cpp` produces the following cases. Every frame
seeds the source and destination deterministically, performs the transfer and
compares all destination storage against a CPU-generated expected result.
NO_OVERWRITE fixtures finish the earlier seeding transfer before making that
promise. No game shader or captured record is patched for acceptance.

| Mode | Operation and coverage |
|---|---|
| 0, 1 | Full and offset rectangular 2D copy |
| 2, 3 | Full DISCARD and partial NO_OVERWRITE 2D copy |
| 4, 5 | Same-subresource overlapping and disjoint 2D copy |
| 6 | Empty copy box |
| 7, 8, 9 | Full, partial NO_OVERWRITE and full DISCARD buffer copy |
| 10, 11 | Full 2D update and partial update with padded rows |
| 12, 13 | Full DISCARD and partial NO_OVERWRITE 2D update |
| 14 | Partial ordinary buffer update |
| 15, 16 | Partial NO_OVERWRITE and full DISCARD constant-buffer update |
| 17 | Partial 3D update with padded rows and slices |
| 18 | 1D-array update to mip 1, layer 1 |
| 19 | Full BC1 update |
| 20 | Empty update box |
| 21 | Full NO_OVERWRITE 2D copy |
| 22 | Full constant-buffer update with flags zero |
| 23 | Same-buffer overlapping copy |
| 24 | Full 2D update with padded rows |
| 25 | Partial BC1 block update |

The final producer emits 26 captures in
`artifacts/m2-transfer1-originals-verified`, with 12 native and 12 shim-injected
verified frames per case: 624 frame checks. Each case has a mandatory exact
resource boundary in [transfer1-corpus.json](transfer1-corpus.json). Fourteen
cases directly present the transferred texture; twelve use marker images, which
cannot prove resource contents. All cases therefore require byte checks.

`Transfer1Tests` covers 52 hardware/WARP combinations with two unobserved/observed
replay pairs on each reused Replay object. Boundary callbacks must run exactly
once; checks cannot pass by observing no events. A separate disabled-command
control must change bytes except for the two empty-box modes. Malformed file
copies are rejection tests only, never accepted original fixtures.

## Retained capture-omission controls

The earlier producer directly copied to the backbuffer without clearing an RTV.
Its fourteen direct-image files omit the context resource record. They are
retained unchanged in [transfer1-context-corpus.json](transfer1-context-corpus.json),
with their original producer source preserved as
`artifacts/transfer1-probe-before-context-observation.cpp`. FloraGPA's existing
checked Map READ/Unmap recovery supplies explicit provenance for the immediate
context. This batch does not add a guessed context fallback.

The development original player fails to open those fourteen files. Bounded
error-object inspection in `artifacts/m2-transfer1-original-error-chain-0`
reports `File Open failed`, caused by `To load resource of unknown type.` in
`resourcemanager.cpp`. Error layout evidence is separately retained in
`artifacts/m2-transfer1-error-layout.c`. The final producer adds an ordinary
backbuffer clear before the transfer, prompting the writer to save the context;
the resulting captures open successfully in the original player. These are new
original captures, not edited versions of the failure controls. The error and
omitted record are directly observed; a complete audit of the original resource
loader is outside this batch.

The first general comparison also invoked an unpackaged CLI under the runner's
isolated PATH; its exit `0xc0000135` was a deployment error. Those outputs remain
in `artifacts/m2-transfer1-first-comparison`. Acceptance uses the complete
`out/FloraGPA-transfer1-20261003` replay package; the final package
`out/FloraGPA-transfer1-final-20261003` includes the separately verified GUI export
fix with identical CLI/Worker replay binaries. Failures are not replaced by stale images.

The context-omission corpus runner returns exit 1 for the original-player Open
failures. Its native successes and original failures are verified separately;
this exit is not relabeled as a fully successful cross-implementation comparison.
Three unchanged reconstructed counterparts also pass structural preflight,
checking that malformed-copy failures are not caused merely by rebuilding the
container (`artifacts/m2-transfer1-repack-controls/summary.json`).

## Final regression evidence

The [baseline](transfer1-baseline.json) records 310 files and 620 ordinary native
runs: 309 stable files plus known Helldivers variability. All 269 prior stable
hashes remain unchanged. All 349 diagnostic controls and 200 resource boundaries
(180 texture, 20 buffer) pass. The primary 26 originals have 52 successful original
image comparisons; the 14 omission controls retain 28 original Open failures.

Four goldens pass. Of 36 related CTest suites, 35 passed initially and the UI
suite failed. Follow-up investigation produced a separate deterministic export
cache counterexample. The [export fix](TEXTURE_EXPORT_FIX.md) retains the
clicked texture across refreshes in the save dialog; a corrected deterministic
counterexample distinguishes pre-fix and post-fix bytes. The complete Qt/Worker
suite passes after the fix with 56 cases; the transfer suite has 57 cases. Both
have no skips. Initial UI failures, a filename-selection error in the first test
harness and an incremental-build timestamp mistake remain documented in the
local evidence rather than counted as successful checks. Native matrices need
no repeat for this GUI-only fix; final CLI/Worker binaries remain identical.

## Reproduction and limits

Build `FloraTransfer1Probe` explicitly and run
`<fresh-output-directory> <mode> [<capture.gpa_frame> <shimloader64.dll>]`.
Capture runs require `GPA_LOCAL_INJECT=true`. For CTest, set
`FLORA_TRANSFER1_CAPTURES` to the verified numbered capture root, then run
`transfer1` serially. GPU validation is always serial. Captures, original DLLs,
Python comparison tools and generated outputs remain outside the runtime/Git.

Evidence maintenance issue: this batch's CTest wrapper accidentally reused
`artifacts/ctest-m2-clear-view.log`. The previous aggregate log could not be
recovered byte-for-byte. Its hash remains unchanged in the historical ClearView
baseline; per-suite Qt archives, binary/source hashes and corpus reports remain.
The current first aggregate is retained as `artifacts/ctest-m2-transfer1-first.log`,
and the wrapper's future destination is corrected. Details and the actually
executed script are pinned in `artifacts/transfer1-artifact-issue.json`; no old
log was reconstructed or represented as preserved.

This batch does not certify every format/usage combination, predicated transfers,
partial-discard retained contents, cross-adapter behavior, tiled resources or
linked/deferred execution. Existing copy format limits and planar-data provenance
remain in effect. P010/P016 missing chroma still requires an explicit complete
source; it is not fabricated. Original-player adapter/configuration equality is
unproven, so observed image equality is not strict matched-device equivalence.
M2 and M3–M6 remain incomplete; module migration totals are unchanged.
