# Preserve successful READ Map synchronization

Reviewed 2026-10-06. This M4 correction restores a CPU/GPU ordering dependency
that was incorrectly classified as read-only metadata. It neither supplies
missing resource bytes nor modifies captured shaders.

## Reproduced problem

An unmodified original capture contains this sequence:

1. Reset a dynamic buffer with WRITE_DISCARD.
2. Copy it to a staging buffer and successfully Map that destination for READ.
3. Unmap it, then sparsely change the source using WRITE_NO_OVERWRITE.
4. Use the earlier copied bytes in later GPU work.

The previous replay skipped the READ Map. Its earlier GPU copy could consequently
read the later CPU writes. The first failing C++ test finds the old-copy storage
changed after the second Map on both hardware and WARP. A subsequent producer
renders an 8x8 triangle whose color comes from that earlier copy: old FloraGPA
produces a gradient instead of the uniform expected RGBA `(17,30,43,56)`.
Both saved full Map payloads exactly match the application's independent byte
oracles; the captured READ synchronization record is present. This is an
execution gap, not missing captured texture data.

The local original private playback path also reproduces the wrong gradient.
After the correction FloraGPA matches the uninjected hardware/WARP producer and
the GPA-injected producer, while that original-player path differs. Its selected
adapter is not positively identified, and the original GPA GUI was not tested;
this observation must not be generalized to every original configuration.

## Execution contract

A checked unlinked successful `0x34ec` READ now performs a native READ Map and
immediate Unmap of the saved resource/subresource before replay continues. This
preserves resource availability before later NO_OVERWRITE writes. No captured
pointer is dereferenced and no CPU read result is invented. The recorded Unmap
continues to be a pairing observation, avoiding a second native Unmap.

A saved DO_NOT_WAIT success is replayed with a blocking READ Map: readiness must
hold at this point on the replay device, whose scheduling can differ. Captured
failed Maps still do not map, wait, create missing resources or write storage.
This reproduces the saved ordering requirement, not the original application's
polling timing or CPU control flow. Microsoft's [Map contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
and [DO_NOT_WAIT behavior](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map_flag)
distinguish a ready resource from a busy-resource failure.

The implementation retains the existing envelope, pairing, context, HRESULT,
flags and subresource checks. Payload edits to observation records still reject.
Native failures identify the event and resource. `map_read_synchronizations`
counts successful waits separately from historical `map_read_observations` and
captured-write `Map` counts. Coverage classifies READ as `execute`; Unmap remains
`metadata`. Offline preflight still does not assert native Map success.

## Original captures and independent evidence

`tools/native/sparse_map_probe.cpp` and `tools/capture_sparse_maps.py` create
unmodified original GPA 2025 R1 captures using the existing pinned primary-chain
selector. They are development tools, never production dependencies. The final
batch contains 8 modes x 3 backends x 12 frames = **288 producer frames**, with
**576 complete before/after readbacks**, frozen sources/binary and adapter IDs.

| Mode | Resource / write path | Image evidence |
|---|---|---|
| 0 | 13x7 RGBA8 staging Texture2D, WRITE then sparse READ_WRITE | Marker only |
| 1 | 1025x19 Texture2D with padded mapped rows | Marker only |
| 2 | 13x7x5 Texture3D with mapped rows/slices | Marker only |
| 3 | 13x1 Texture2D | Marker only |
| 4 | 64 KiB staging buffer, WRITE then READ_WRITE | Marker only |
| 5 | 64 KiB dynamic vertex buffer, DISCARD then NO_OVERWRITE | Marker only; earlier-copy boundary detects the defect |
| 6 | Dynamic buffer plus rendering of the earlier copy | Full CPU image oracle |
| 7 | Same, with real DO_NOT_WAIT READ polling | Full CPU image oracle |

All modes require complete resource-byte comparisons. Modes 0..5's green marker
images alone prove no Map correctness. Modes 6..7 additionally pass 72 complete
producer image checks. The final original files remain unchanged in
`artifacts/m4-read-map-sync-originals/`.

The initial discovery batch failed because the probe treated the helper's
"already primary" return as failure. A later image prototype had incompatible
VS/PS signature register placement; the native D3D11 debug layer identified it.
The final producer includes matching signatures and a debug control with no
messages. Failed discovery files/logs remain separate from accepted captures.
A build wrapper also named a nonexistent test target; the corrected build and
all actual suite results are retained. None of these failures was fixed by
altering a capture or relaxing an image/byte assertion.

## Verification

- Map tests: **60 passed**, no skips. Eight original modes on hardware and WARP
  cover repeated full replay, before/after sparse-write boundaries, both copy
  targets, and disabled-write controls. Saved payloads are checked against the
  producer's complete bytes. Existing malformed lengths, references, flags,
  overlapping maps and failed-call tests remain active.
- Seven related CTest suites pass, including **65 Qt rows** without skips. The
  new GUI workflow opens both synchronization image captures, navigates to Draw
  and returns to Final, checking the complete expected image each time.
- **484** existing unique files retain complete preflight JSON and exit codes,
  except the intended READ coverage reclassification in **472** files. No other
  finding, count, location or status changes.
- Focused original comparison passes 16 independent runs, 16 original-player
  completions and **80 strict resource exports**. Six marker-image cases agree;
  two image-dependent cases expose the original-player discrepancy described
  above. Original-player image equality is not the resource oracle.

- The full 32-suite gate passes **502 registrations / 492 unique captures**:
  **476 native completions (466 unique)** and **26 located rejections**, with
  1,004 ordinary attempts, 684 controls and 808 resource exports. All preceding
  494 cases retain their status and deterministic pixels. Existing execution
  counters are identical after excluding the new READ synchronization counter.
  Only the already documented Helldivers images vary. The 33 known capture
  limitations remain separate: completion does not certify application fidelity.
- The first gate invocation stopped before the new suite because its registration
  hash was stale. After correcting that hash, the recovery script verified the
  unchanged executable and all 31 existing report hashes, reran every gate
  assertion over their evidence and executed the remaining eight cases. The
  interrupted report/log are preserved. This is a resumed, fully checked gate,
  not a claim that the interrupted invocation exited successfully.
- Four isolated-PATH GF2/BF1 golden and disabled-Draw checks pass. A relocated
  44-file package plus Qt test support passes all three Qt harness rows, with
  both new originals checked at Final, Draw and Final again. The offscreen font
  warning is retained. This is same-host deployment evidence, not independent
  clean-machine acceptance. An explicit file-output frame-validation rerun
  retains all 173 passing rows after CTest did not retain stdout row details.

Producer: `3868e84`; replay: `15ddac1`; UI regression: `6d07974`.
Package: `out/FloraGPA-read-map-sync-20261006/`. The
[pinned baseline](read-map-sync-baseline.json) identifies the package and complete
verification evidence. Captures, binaries and generated reports remain outside Git.

## Differential texture boundary remains open

The preceding 484-file inventory contains 332 differential Map writes, all to
buffers in GF2, BF1 and Helldivers. Both sparse-texture discovery batches and the
final eight captures save full GenData, including NO_OVERWRITE buffer changes.
No newly observed original texture GenDataDiff supports translating capture
mapped offsets into tight rows/slices. The player disassembly at RVA 0x27080
applies differential offsets directly to mapped addresses, unlike its full-data
row/slice path. Unsupported 2D/3D differential layouts therefore still reject;
we do not infer capture pitches from the replay device or manufacture padding.

This correction is about a saved ordering dependency, not eliminating arbitrary
GPU workload races. Helldivers' intra-dispatch sharpening variability retains its
separate policy. Missing pitches/data, retained-list semantics, complete M4/M5
acceptance and independent clean-machine deployment remain open. Module ledger:
72 ported / 117 partial / 15 pending.
