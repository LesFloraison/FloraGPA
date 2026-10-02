# Helldivers 2 capture compatibility — 2026-10-03

## Root cause

`helldivers2_2026_04_02__18_02_58.gpa_frame` is a valid 1,581,399,564-byte
IGPA v3 DX11 container with 21,392 entries and 737 state blocks. Its SHA-256 is
`97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139`.
The initial inventory succeeds. Opening the capture automatically starts replay,
which previously failed at event 1 (`Unknown (13725)`, type `0x359d`).
This was a command compatibility failure, not a corrupt container or file-size limit.

Read-only Ghidra inspection of the existing `dx11_playback.dll` project, RVA
`0x113c20`, identifies `GETIMMEDIATECONTEXT` and its `ppImmediateContext` field.
The observed wire payload is exactly three little-endian uint64 values:
record link 0, device reference 3, returned context reference 4. The returned
reference is captured inspection data; this getter does not mutate GPU state.

After accepting that record, replay stopped at event 18,
`Device5.CreateQuery` (`0x358d`). The API inspector already decoded query metadata,
and the original Python engine already handled it, but the C++ replay path did not.
The observed CreateQuery records contain a zero returned object reference.

## Changes and boundaries

- Name and inspect `Device5.GetImmediateContext`; preserve its exact uint64
  returned reference in API details and validate the 24-byte wire layout.
- Validate the recovered CreateQuery, CreateQuery1, GetData, GetDataSize and
  GetDesc wire variants before accepting them as captured query metadata.
- Count these separately as `query_metadata_records`. Captured returns do not
  become newly measured query results, replayable query objects or CPU branches.
- Reject nonzero CreateQuery identities, invalid presence flags, truncated
  records and trailing bytes. Unknown commands retain the existing explicit failure.
- Add synthetic truncation/reference/replay tests and an optional real-capture Qt
  regression covering open, full replay, event selection and resource navigation.

This patch does not implement general query object restoration or widen support
for unverified command layouts. It does not change the original capture or Python
source and introduces no GPA/Python dependency into the application.

## Reproduction and validation

Production package: `out/FloraGPA-helldivers-20261003/`.
Local evidence: `artifacts/helldivers-review/` (excluded from Git).

```powershell
./out/FloraGPA-helldivers-20261003/FloraGPA.Cli.exe replay `
  D:/CDXrepo/FloraGPA/helldivers2_2026_04_02__18_02_58.gpa_frame `
  --out artifacts/helldivers-local

$env:FLORA_TEST_HELLDIVERS_CAPTURE = 'D:/CDXrepo/FloraGPA/helldivers2_2026_04_02__18_02_58.gpa_frame'
ctest --test-dir build/vs2022 -C Release -R '^(stream_output|api_commands|ui|draw_resources|core)$' --output-on-failure -j 1
```

On the NVIDIA GeForce RTX 3070 Laptop GPU, full replay produces a 1920×1080
image from live backbuffer resource 21362. It executes 607 draws (144 Draw,
62 DrawIndexed, 401 DrawIndexedInstanced), 130 Dispatch, 2,241 Map,
30 UpdateSubresource, 37 RTV clears and 5 depth/stencil clears. It accepts one
inspection record and eight query metadata records. `reference_pixels_used` is
false. The disabled-draw control completes with zero draws and 130 Dispatch
and produces a different output.

### Regression results

- Release production build succeeds; all four EXEs and `FloraGPA.Metrics.dll`
  in the delivered package match the Release files byte for byte.
- Five relevant CTest suites pass: `core`, `api_commands`, `stream_output`,
  `draw_resources` and `ui`. The final UI run has 55 passed, zero failed and
  zero skipped, including the Helldivers open/replay/navigation case.
- `draw_resources` has 10 passed with the real BF1 fixture; `stream_output`
  has 19 passed, including strict query layouts and inspection tails.
- GF2/BF1 exact golden images and both disabled-draw controls pass using the
  portable package with only system paths in the child environment.
- Helldivers full replay and disabled-draw module inventories contain no
  Python/Tk/GPA/RenderDoc DLLs; Qt modules resolve inside the package.
- Rechecked the original capture hash after validation; it is unchanged.

The first UI run had one pre-existing texture-export test timing failure: it
triggered the next export while the action was disabled during a queued refresh.
The test now waits for the action to be enabled between exports. The complete UI
suite subsequently passes. Initial failure evidence is retained in
`artifacts/helldivers-review/ui-initial.txt`; final results are in
`ctest-ui-final.log` and the normal CTest UI report.

### Image accuracy boundary

The sample does **not** have a stable exact replay hash. Two independent native
runs and two Python runs all completed but produced different hashes. The Python
comparison used the original engine with only `0x359d` added to its auxiliary
set in memory; no original source file was changed. Therefore this is a useful
comparison of replay behavior, not an entirely unadapted oracle.

| Comparison | Changed pixels / 2,073,600 | Mean absolute channel error (0–255) | Maximum error |
|---|---:|---:|---:|
| Native repeat | 116,002 | 0.038218 | 24 |
| Python repeat | 123,765 | 0.040731 | 24 |
| Native vs Python | 131,788 | 0.043554 | 24 |
| Native vs captured reference | 144,975 | 0.047957 | 20 |

The means include all RGBA channels; alpha is identical in all four comparisons.
These are observations from two runs per implementation, not tolerance guarantees.
The source of the variation remains unresolved. The UI test checks successful
replay and resource navigation rather than asserting an arbitrary first-run hash.
GF2 and BF1 retain their existing exact hash baselines. This fix establishes that
this Helldivers frame opens and replays, not pixel-perfect reconstruction or
universal compatibility with all Helldivers captures and analysis features.
