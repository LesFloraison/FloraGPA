# Helldivers 2 capture compatibility — 2026-10-03

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

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
# Run from the repository root; set external paths for your environment.
$CaptureRoot = 'C:/captures'
./out/FloraGPA-helldivers-20261003/FloraGPA.Cli.exe replay `
  "$CaptureRoot/helldivers2_2026_04_02__18_02_58.gpa_frame" `
  --out artifacts/helldivers-local

$env:FLORA_TEST_HELLDIVERS_CAPTURE = "$CaptureRoot/helldivers2_2026_04_02__18_02_58.gpa_frame"
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
Follow-up investigation below localizes the observed variation to an in-place
sharpening dispatch. The UI test checks successful replay and resource navigation
rather than asserting an arbitrary first-run hash.
GF2 and BF1 retain their existing exact hash baselines. This fix establishes that
this Helldivers frame opens and replays, not pixel-perfect reconstruction or
universal compatibility with all Helldivers captures and analysis features.


### Follow-up: original GPA comparison and localized cause

The installed original GPA 2025 R1 `dx11_player.dll` was replayed twice through
our previously recovered private ABI adapter (`tools/replay_frame.py` in the
Python research workspace), in separate worker processes. Its pinned SHA-256 is
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
Both opens/playbacks returned success, cleanly closed, and exported a newly
rendered framebuffer. Their image hashes differ:

- `f39dbb39b9e7ccd2a3adece45a4d4f02ac0b67da53b00ab64f269204bdd36595`
- `d1d0493b1930704fd99ecfe26655a0bb675c29f603001700c7c3395496faa413`

86,549 / 2,073,600 pixels differ (4.174%); mean absolute RGB error is
0.0371965 on the 0–255 scale, maximum channel difference 29, alpha identical.
This confirms repeat variation in the **original replay kernel** for this capture.
It does not measure full GUI behavior: the adapter requests single-threaded host
operation, no initial warmup playbacks, no GT query manager, and one explicit
playback, using the kernel's default adapter selection. The precise selected
adapter was not recorded by that adapter, so this is not an assertion of
numerically identical driver/device conditions between the two implementations.

Native boundary experiments identify the responsible captured shader path:

| Boundary / control | Repeated result |
|---|---|
| Resource 18325 after Dispatch 18344 | Two raw RGBA16F images byte-identical |
| Resource 18325 after Dispatch 18401 | 357,759 half-float components differ; maximum absolute difference 0.017822265625 |
| Resource 18325 after Dispatch 18431 | 353,985 half-float components differ; maximum absolute difference 0.037353515625 |
| Final input resource 18734 (MSAA UI texture, resolved) | Two readbacks byte-identical |
| Full frame, only Dispatch 18401 disabled | Three complete output hashes identical |

Dispatch 18401 binds CS resource 18393 and UAV 18324, backed by texture 18325.
It dispatches 120×68×1 groups with 8×8×1 threads per group. The captured DXBC
contains `sharpen_amount` / `sharpening_amount` constants and an in-place
neighborhood sharpening filter: read center/up/down/left/right from `u0`, then
write the filtered value to `u0`. There are no `sync` instructions in this shader.
Neighboring invocations therefore read locations that other invocations overwrite.
The observed outcome depends on scheduling and visibility of those writes; it is
not explained merely by floating-point arithmetic being approximate.

For comparison, HLSL synchronization is explicitly scoped: see Microsoft's
[RWTexture2D documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-rwtexture2d)
and [group synchronization documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/groupmemorybarrierwithgroupsync).
An API boundary wait cannot impose a deterministic ordering on competing reads
and writes inside one dispatch. A deterministic neighborhood filter normally
reads an unchanged input and writes to separate output storage.

The earlier MSAA initialization lead is not supported as the cause here:
ClearRenderTargetView event 18733 clears RTV 18736 / texture 18734 before UI
rendering, and its resolved final input repeats exactly in the tested pair.

Disabling only event 18401 reduces Dispatch count from 130 to 129; draws remain
607. Three full-frame runs yield
`22f2d66bdbca7a3b1aa868c5a68e39ebc7669e070596eec6b8f0e6cc922e2605`, with
`reference_pixels_used=false`. This is a **diagnostic control**, not an application
fix: omitting sharpening changes the captured workload and its intended image.
The production replayer still executes the original captured shader unchanged.

Evidence resides in `artifacts/helldivers-variance/`: original-kernel reports and
TGA exports, shader disassemblies, raw stage readbacks, and the three disabled-
sharpening reports. The tested data strongly localizes the observed native repeat
variation; it does not prove that every possible source of nondeterminism in the
game or every configuration of the GPA GUI has been exhausted.
