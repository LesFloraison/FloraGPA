# Successful writable Map readiness

Reviewed 2026-10-08. Implementation `ebfbc48` fixes a saved-success Map replay
failure in eight untouched GPA 2025 R1 legacy DX11 captures. Production remains
native C++; GPA and Python are development-only. M3/M4/M5 remain incomplete.

## Original trigger

The application queues 64 copies into a staging resource, flushes, polls a
WRITE or READ_WRITE Map with DO_NOT_WAIT until it succeeds, writes the resource,
then copies and reads back the complete result. The eight cases cover Buffer
and Texture1D/2D/3D for both write modes, including padded rows/slices. Each
original file contains two failed polling attempts and one successful write.
The buffers use real GenDataDiff records; the textures use full tight GenData.

The previous FloraGPA package passed structural preflight for every file but
failed all 16 ordinary replay attempts at the saved successful Map with
`HRESULT 0x887a000a` (DXGI_ERROR_WAS_STILL_DRAWING). Replaying the saved flag
literally asks whether the resource is ready under the replay device's current
scheduling. The captured CPU polling delay is not replayed, so that answer can
differ even though the file explicitly records a successful mapping.

Microsoft's [Map contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
and [Map flag definition](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map_flag)
distinguish this busy response from a successful map and prohibit DO_NOT_WAIT
with WRITE_DISCARD/WRITE_NO_OVERWRITE. The earlier
[READ Map correction](MAP_READ_SYNCHRONIZATION.md) already preserves a saved
successful read's readiness; this increment closes the writable counterpart.

## Execution and diagnostics

After the shared audit validates the saved successful write and its data,
native Map uses blocking readiness on the replay device. Map type, resource,
subresource and saved full/differential bytes are unchanged. The report counts
these saved DO_NOT_WAIT writes as `map_write_readiness_waits` in addition to
the existing `Map` count. The original captured flags remain visible in API
inspection; no file is edited and no CPU branch is reconstructed.

Failed captured Maps still return without native Map, resource materialization,
waiting or writing. A failed invalid flag/type combination is also left failed;
it does not become a retry. Successful DO_NOT_WAIT plus DISCARD/NO_OVERWRITE is
now explicitly rejected before normalization, with event/resource diagnostics.
Other HRESULTs, flags, missing references and malformed data retain their checks.
Native failures other than readiness still propagate as located errors.

This establishes the recorded resource-availability boundary. It does not
reproduce the application's number or timing of busy polls, recover absent
initial bytes, or restore CPU control flow.

## Capture-side layout investigation

Fresh read-only Ghidra analysis of the pinned `shimd3d64.dll` follows Map tracking
at RVA `0x314850`, buffer snapshot `0x3140b0`, and Unmap serialization `0x314b80`.
The imported program's SHA-256 matches the frozen capture producer's DLL;
`program-identity.txt` records that check and the `0x180000000` image base.
The tracking structure holds native RowPitch/DepthPitch transiently. In the
examined ordinary writer, only resource type `0x83` (Buffer) reaches the
GenDataDiff (`0x100`) serialization branch. Ordinary textures are repacked into
full GenData through `0x347900`; the private texture branch also stays in the
full-data envelope. The 48-byte saved Map command itself has no row/slice pitch.

The hash-checked survey covers the preceding 603 unique registered files plus
these eight originals: **611 files, 7,066 successful writes**. It finds 6,706
full Buffer writes, 334 Buffer differences, four full 1D, 16 full 2D and six
full 3D writes, with no unresolved survey rows or texture GenDataDiff records.
Only the eight new files have successful writable DO_NOT_WAIT records; their
16 failed polls are counted separately. This explains the previous coverage gap.

These results do not prove that every producer version or private path excludes
texture differences. Unverified 2D/3D differential layouts still reject; no
capture pitch is guessed from the replay device. The investigation redirects
the next compatibility work toward demonstrated ordinary-path gaps rather than
treating an unobserved texture-difference layout as recovered.

The decompiler outputs, survey and console logs remain local under
`artifacts/m4-map-layout-research/`; they are not shipped as recovered GPA source.
The player-side mapped-write evidence in the reference workspace remains
`analysis/mapped_textures/execution.c` and `analysis/MAPPED_TEXTURES.md`.

## Independent evidence

`FloraMapNowaitProbe` and `tools/capture_map_nowait.py` freeze producer sources,
binary, GPA DLL hashes, adapter IDs and output hashes. Hardware and WARP run
with the D3D11 debug layer; injected runs use the original shim. The accepted
corpus contains **24 producer runs / 288 frames**, each with a complete resource
byte check; all 16 native debug runs have no messages. READ_WRITE additionally
checks all pre-write mapped bytes before modifying four locations. WRITE fills
the complete resource. Python independently generates the expected byte formulas.

`tools/catalog_map_nowait.py` verifies the saved full data or applies the saved
Buffer ranges to the independently known before-data and checks the complete
after-data. Its first full-data-only assumption rejected the actual Buffer
differences; the corrected catalog validates both layouts. Original captures
remain unchanged. The [corpus manifest](map-nowait-corpus.json) requires exact
before/after resource exports, in addition to a constant green present marker.
**That marker alone provides no Map correctness proof.**

The pinned original private player was also attempted 16 times. It reports
playback failure with error callbacks and supplies no usable reference images.
Its adapter is unidentified. These observations do not establish the behavior
of every original GPA UI/device configuration or a matched-device equivalence.
The independent application bytes, saved capture data and native replay
readbacks are the correctness oracles for this increment.

| Validation | Observed result | Local evidence |
|---|---|---|
| Before repair | 16 native failures at saved successful Map, zero preflight errors | `artifacts/m4-map-nowait-before-verified/validation.json` |
| Map NOWAIT tests | 28 passed, no failures/skips | `artifacts/m4-map-nowait-tests/map-nowait-Release.txt` |
| Related CTest | Six suites passed: Map records, planar writes, texture copies, preflight, Query completion and API commands | `artifacts/m4-map-nowait-related-ctest.txt` |
| Relocated Windows UI and recovery | Five navigation rows and eight recovery rows passed, no failures/skips | `artifacts/m4-map-nowait-portable QA 中文/validation.json` |
| GF2/BF1 | Two exact goldens and two suppressed-Draw controls passed | `artifacts/m4-map-nowait-goldens/validation.json` |
| Focused originals | Eight repeat-stable cases; 16 full replays and 32 strict before/after resource exports | `artifacts/m4-map-nowait-after/validation.json` |

The 16 original test combinations run on hardware/WARP. Each performs eight
full replays without inspection between the queued copies and saved Map, then
checks the written resource and its downstream copy; separate before/after
prefixes and disabled-write controls verify the retained original contents.
Nine malformed counterexamples cover invalid flag/type pairs, extra flags,
truncated/trailing envelopes and data, missing data identities and invalid
Buffer ranges. A failed-call fixture proves that normalization cannot retry an
original failure. Setup/cleanup are included in the 28 top-level Qt rows.

The candidate is `out/FloraGPA-map-nowait-20261008/`. A separate 1,209-file Git
archive of `ebfbc48` builds and packages successfully with a system-only PATH;
the Map and UI test binaries build from that same archive without changing
production files. The rebuilt package is relocated to a path containing spaces
and Chinese characters and passes 28 Map rows, five Windows UI rows and four
strict GF2/BF1 golden/control checks with system-only PATH. Its golden hashes and
execution counts match the incremental candidate. This is same-host acceptance,
not independent-machine certification.

The incremental candidate passes **42 suites / 621 registrations / 611 unique
captures**: 582 completions / 572 unique completions and 39 located refusals.
It retains 1,242 ordinary attempts, 828 diagnostic controls and 1,448 resource
exports; these counts include explicit refusals and separately scoped controls.
All 613 preceding registrations retain their full preflight reports, execution
counts, refusal diagnostics and deterministic images; the known Helldivers
variation stays separately classified. Capture-fidelity assessments are 150
passed, 32 capture-side mismatch, 16 information missing and 423 unassessed;
35 completed registrations retain known capture limitations. Completion is not
complete application fidelity. Full-matrix evidence is from the incremental
candidate; the separately rebuilt package received the focused checks above.
See the [pinned baseline](map-nowait-baseline.json) for source, package and local
evidence identities.

New long-soak and independent clean-host acceptance have not been performed.
Captures, GPA DLLs and generated artifacts remain outside Git; the module ledger
remains 72 ported / 117 partial / 15 pending.

## Reproduction

With the original local corpus available:

```powershell
cmake --build --preset release --parallel 4 --target FloraMapNowaitTests
$env:FLORA_MAP_NOWAIT_CAPTURES = 'D:/CDXrepo/FloraGPA-Cpp/artifacts/m4-map-nowait-originals'
ctest --test-dir build/vs2022 -C Release -R '^map_nowait$' --output-on-failure
```

An absent external corpus produces explicit fixture skips, not acceptance.
Regeneration uses the development-only `FloraMapNowaitProbe`, installed debug
layers, the pinned original shim, `capture_map_nowait.py` and then
`catalog_map_nowait.py`, with fresh output paths. No production binary requires
those development tools.
