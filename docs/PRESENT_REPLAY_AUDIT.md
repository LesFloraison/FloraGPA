# M2: checked Present boundaries and native binding semantics

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Subsequent [buffer creation](BUFFER_CREATION_AUDIT.md) and [pipeline setters](PIPELINE_SETTER_AUDIT.md) address additional paths. The unresolved presentation statuses, rotation and export-comparison limits below are not automatically closed.

`SwapChain.Present` (`0x3257`) now has a checked production path instead of the
unchecked auxiliary fallback. This recovers the observed single-frame submission
boundary and in-frame TEST calls. It does **not** implement general multi-frame
buffer rotation, resize, every presentation flag/status, or a display swap chain.

## Production behavior

The decoder requires the exact 28-byte call (link, swap-chain identity, HRESULT,
sync interval and flags) and 88-byte x64 swap-chain resource. Linked calls,
unsupported statuses/flags, invalid intervals/descriptions and wrong-category or
missing identities are rejected. Flip presentation also requires one unambiguous
live Texture2D with the captured swap-chain parent and matching description.

| Accepted case | Effect on independent replay |
|---|---|
| S_OK, TEST (`1`) | Checked call, no presentation or binding mutation; later commands may execute |
| S_OK, ordinary flags (`0`), discard/sequential | Submission boundary; bindings remain intact |
| S_OK, ordinary flags, flip sequential/discard | Selectively unbind backbuffer RTVs and CS/OM UAVs |
| S_OK, ALLOW_TEARING (`0x200`) | Same flip transition; require captured tearing-capable, windowed chain and interval zero |

Other RTVs, depth binding, shader SRVs, unrelated UAVs and UAV counters are retained.
This uses actual native view-to-resource identity, not the last draw's snapshot or
a blanket ClearState. Reports count `Present`, `present_tests` and each kind of
binding removal. No saved HWND is used; no GPA DLL or Python is involved at runtime.

The independently rendered storage is retained as the **submitted frame image**.
It is not advertised as the next physical backbuffer after rotation. After an
ordinary Present, only checked CPU observations are currently accepted; later
execution is rejected with the offending event and swap-chain ID. This deliberate
diagnostic replaces the former silent acceptance of unresolved rotation/content
semantics. TEST can occur before subsequent clear/copy work and is covered by an
unmodified original capture. No reference framebuffer pixels are copied into replay.

Non-S_OK results, DO_NOT_SEQUENCE, DO_NOT_WAIT, RESTART, stereo flags, linked Present,
multiple live buffer identities, post-submission execution, resize and multi-frame
rotation remain gaps. Failed/status-only calls are not automatically treated as
successful or assumed to preserve all state. These remaining paths prevent a claim
of complete Present or M2/M4 support.

## Evidence and reproduction

The capture layout agrees with the existing native inspector and original shim
serialization in `D:/CDXrepo/FloraGPA/analysis/api_commands/queries.c`: the Present
record saves the returned HRESULT, interval and flags as three four-byte fields.
The parent/GetBuffer(0) identity evidence is retained in the research workspace's
`analysis/PRESENTATION_TARGET.md` and decompiler exports.

The implementation follows the conditional flip transition described by Microsoft's
[Present documentation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
and [presentation flag requirements](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).
The precise writable-binding behavior is measured independently:

- `FloraPresentBoundaryProbe`: 60 hardware/WARP observations across ordinary, TEST
  and DO_NOT_SEQUENCE calls; single/MRT outputs, all six shader SRV stages, CS UAV
  and OM UAV. Ordinary flip removes matching RTV/UAV bindings but retains SRVs and
  other RTVs. TEST retains bindings. DO_NOT_SEQUENCE returns `0x887a0001` in this
  composition setup; it provides no successful-call acceptance evidence.
- `FloraPresentCaptureProbe`: five self-owned HWND workloads, each run natively and
  with the original shim for 12 frames. Every frame records real before/after RTV
  binding and checks main-buffer GPU bytes against a green CPU image. Modes cover
  in-frame TEST, secondary flip sequential, discard, flip discard and tearing.
  The secondary buffer is independently cleared red. Original CaptureNextFrame
  outputs are never rewritten. Ordinary Present ends the capture on that secondary
  buffer; TEST remains in the stream before a later clear/copy.
- `PresentRecordTests`: production event-boundary state on all five original
  captures, on both hardware and WARP; synthetic counterpart/negative cases cover
  every strict call prefix, trailing bytes, invalid flags/status/interval, wrong or
  ambiguous identities, conflicting descriptors and post-Present execution. Four
  CS/OM UAV tests verify unrelated counter value 17 survives the transition.

These development-only producers are excluded from default builds, CTest and the
deployed package. Build and reproduce with fresh output directories:

```powershell
# Run from the repository root; set external paths for your environment.
$ReferenceRoot = 'C:/reference/FloraGPA'
cmake --build --preset release --target FloraPresentBoundaryProbe FloraPresentCaptureProbe FloraPresentRecordTests
./build/vs2022/Release/FloraPresentBoundaryProbe.exe artifacts/new-present-bindings
python tools/capture_present.py --producer build/vs2022/Release/FloraPresentCaptureProbe.exe --out artifacts/new-present-originals
python tools/validate_corpus.py --manifest artifacts/new-present-originals/manifest.json --captures-root artifacts/new-present-originals --exe out/FloraGPA-present-20261003/FloraGPA.Cli.exe --out artifacts/new-present-validation --oracle-tools "$ReferenceRoot/tools" --timeout 60
$env:FLORA_PRESENT_CAPTURES = "$PWD/artifacts/new-present-originals"
ctest --preset release -R '^present_records$' --output-on-failure
```

The registered five-file corpus is [present-corpus.json](present-corpus.json); use
`--captures-root artifacts/m2-present-five-originals` for these exact local files.
Regeneration has fresh captured pointer identities and hashes and must use its new
manifest. Capture files and raw outputs remain outside Git.

## Acceptance on 2026-10-03

| Check | Observed result |
|---|---|
| Prior 91-file corpus | 182 native runs completed; 90 stable image hashes unchanged; Helldivers variability retained |
| Five new original captures | 10 independent replay runs, all repeat-stable and equal to their strict CPU image references |
| Native/shim producer oracle | 120 frames, all expected binding and main-buffer pixel checks passed |
| Original player | 10/10 kernel runs opened, replayed and closed successfully |
| Original exported image comparison | TEST case matched twice; four dual-chain cases need multi-DDS export identity adaptation |
| Present Qt tests | 25 passed, zero failed/skipped; original cases checked on hardware and WARP |
| Delivery tests | Seven CTest suites; 55 main UI cases without skips; four packaged golden/negative controls |
| Corpus tool tests | Nine CPU tests passed, including incomplete kernel-result evidence rejection |

The original dual-chain player writes numbered TGA/DDS outputs. Its existing
single-TGA wrapper subsequently fails to locate `framebuffer_orig.tga`. The serial
validator now preserves the kernel result and hashes every raw export separately,
classifying this as `export_adapter_failed` instead of conflating it with kernel
failure. It does not pick whichever DDS matches the expected image or claim an
unverified export-to-chain mapping. Original adapter configuration remains unknown.

Within the original 99 observed API families, classification is now 46 execute,
39 metadata, 12 snapshot, 2 auxiliary-unverified. The 91 old preflights remain
review_required with zero errors; warning occurrences fall from 561 to 491.
These labels locate implementations; they are not independently certified feature
percentages. The remaining observed auxiliary families are Buffer.SetPrivateData
and Device.CreateBuffer. Resource validation gaps and M3–M6 remain open.

Compact results and evidence hashes are in
[present-replay-baseline.json](present-replay-baseline.json). Local logs and raw
images reside in `artifacts/m2-present-corpus`, `m2-present-five-originals`,
`m2-present-five-validation`, `m2-present-bindings-uav`, `m2-present-ui` and
`m2-present-golden`. The package is `out/FloraGPA-present-20261003/`; module migration
totals remain 72 ported, 117 partial, 15 pending.
