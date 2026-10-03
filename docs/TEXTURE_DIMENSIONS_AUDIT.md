# M2: creation-time Texture1D and Texture3D replay

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Subsequent [texture view creation](VIEW_CREATION_AUDIT.md) and [buffer view creation](BUFFER_VIEW_CREATION_AUDIT.md) cover additional view paths; their layouts and combinations remain explicitly bounded.

This extends the accepted Device5 creation path to Texture1D (`0x3579`) and
Texture3D (`0x357b`), including their explicit/default SRVs. The profile remains
GPA 2025 R1 legacy DX11. Previously these creation records were decoded but
rejected at execution. The decoder now validates the matching saved resource
kind, device, descriptor, initial-data records and complete storage, then uses
the existing native dimension-specific texture creation implementation.

Resource identities remain unavailable before their creation event. Calls with
no initial data do not preload a later first-use blob. Failed and validation-only
calls do not allocate storage. Repeated identities and unresolved normalization
remain explicit failures. No shader changes or missing-byte synthesis are used.

## Original fixtures

[texture-dimensions-corpus.json](texture-dimensions-corpus.json) registers 17
unmodified original captures from `tools/native/texture_creation_probe.cpp`:

| Scenarios | Coverage |
|---|---|
| 10–16 | Texture1D array: two layers and two mips; upload, initial-only, overwrite, default SRV, failed creation, validation-only and immediate overwrite |
| 30–36 | Texture3D: 16x8x4 volume with two mips; the same seven call patterns |
| 7, 37 | Texture2D/Texture3D source rows overlap in caller memory |
| 38 | Texture3D source slices overlap in caller memory |

The producer's native and GPA-injected runs check all resource bytes and a
texture-dependent image on each of twelve frames: 408 frame checks. Array/mip
contents differ, volume slices differ, and overlap scenarios additionally use
nonuniform rows and pixels. Expected storage is gathered independently from the
caller's row/slice addresses. Each manifest pins capture, source, executable,
oracle and image hashes. Producer device/vendor IDs and feature level are saved;
the original player's adapter selection remains unidentified.

Production C++ tests check creation-time and final bytes on hardware and WARP,
including immediate overwrite with no intervening read. The shared creation
suite also reruns all seven prior Texture2D fixtures. Synthetic tests cover
truncated/trailing wire data, missing initial storage, pre-creation access,
dimension-specific pitch rules, invalid flags and descriptor observations.

## Pitch correction

The previous `rowPitch >= packedRowBytes` check was too strict. A native probe
creates a four-pixel-wide RGBA8 row with an eight-byte source stride successfully;
the source rows overlap. A volume can likewise use a slice stride smaller than
one packed image. These strides describe caller memory, not the saved texture's
packed storage length. The old check would reject a valid captured initializer.

The corrected audit ignores both pitch observations for Texture1D, requires a
nonzero row stride for Texture2D, and nonzero row/slice strides for Texture3D.
It does not impose disjoint source rows/slices. Replay still reads only the
complete saved resource blob and supplies native tight strides; original process
pointers are never dereferenced. Invalid zero-stride controls and successful
overlap calls are retained in `artifacts/m2-creation-pitch-results.csv` with their
native probe source. The overlap captures verify actual bytes, beyond HRESULT.

## Related metadata

Ten strictly sized observation layouts are added:

| Interface | QI | AddRef | Release | GetDevice | GetDesc |
|---|---|---|---|---|---|
| Texture1D | `3134` | `3135` | `3136` | `3137` | `313e` |
| Texture3D | `3007` | `3008` | `3009` | `300a` | `3011` |

QI retains HRESULT/IID/returned identity, reference-count calls retain a uint32,
GetDevice retains a returned identity, and GetDesc uses a checked presence flag
followed by 32 or 36 descriptor bytes. None of these observations releases replay
storage or recreates CPU control flow. Optional flags, every truncated prefix and
trailing bytes have negative tests; API inspection exposes the same checked wire.

Read-only Ghidra evidence is retained in `artifacts/m2-next-creation-wrappers.c`
and its log/index. Texture1D function RVAs are `2a3500`, `2a3c00`, `2a4160`,
`2a46c0`, `2a71c0`; Texture3D RVAs are `2abb10`, `2ac210`, `2ac770`, `2accd0`,
`2af7d0`, in the column order above. The GetDesc writers explicitly serialize
`0x20` and `0x24` bytes. Original capture record sizes independently agree.

## Package acceptance

`out/FloraGPA-texture-dimensions-20261003/` contains the verified package. All five
application binaries match Release. Across 148 registered files, 296 independent
runs complete: 147 files repeat exactly and Helldivers retains its diagnosed
variability. All 130 previously stable image hashes are unchanged.

The 34 new original-player runs, 61 corpus controls, four isolated golden/negative
checks and 42 new resource-boundary exports pass. Each new boundary export
matches the independent producer byte oracle. All 296 ordinary-run module audits
pass. Fourteen related CTest suites pass; creation and main UI each have 55
passing Qt cases, texture edit replay has 34, with no skipped cases in those
three suites. The shared original creation test covers 24 captures on hardware
and WARP (48 runs). Two repeated runs are not an M5 long-duration certification.

## Remaining work

The final package/results are pinned in
[texture-dimensions-baseline.json](texture-dimensions-baseline.json). Comparisons
include original playback, raw storage at creation/upload boundaries, strict
images, diagnostic upload omissions, prior registered captures and golden frames.
Observed original/native image agreement does not assert matched-device equivalence.

This batch accepts the observed RGBA8 array/volume creation paths; it does not
certify every special format, usage, device interface, mip normalization or
cross-device combination. Buffer SRV creation, other view creation, identity
versioning and deferred/list execution remain separate work. Missing data and
original workload races remain diagnosed separately. M2 and M3–M6 stay open;
the next ordinary-path work is creation of RTV/DSV/UAV views with real captures.

## Subsequent view creation acceptance

The following M2 batch accepts captured texture RTV/DSV/UAV creation; see
[VIEW_CREATION_AUDIT.md](VIEW_CREATION_AUDIT.md) for its twelve original fixtures,
metadata evidence distinctions and final 160-file matrix. This does not extend
the earlier dimension fixtures to every view shape, or accept buffer views.
