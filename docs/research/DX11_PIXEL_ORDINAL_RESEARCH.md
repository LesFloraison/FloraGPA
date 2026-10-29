# Draw sample ordinals and original GPA counter state

This P1/P5 research checkpoint establishes a bounded stencil selection technique
on owned workloads and recovers the original GPA diagnostic blend descriptor.
It does not add a product history backend or qualify original GPA fragments.
The runtime remains unchanged. P5/P6 parity and P7/P8 acceptance remain open.

## Original GPA evidence

The read-only target is `dx11_player.dll`, version `25.1.26603.47871`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
`Invoke-PixelCountStateResearch.ps1` checks the file before and after exporting
from the pinned Ghidra project. The exporter retains actual instructions,
references and constants separately from decompiler hypotheses.

| Evidence | RVA and verified interpretation |
| --- | --- |
| Descriptor constants | `0x17d230`: uint32 `{0,0,1,2}`; `0x17d220`: `{2,1,1,1}` |
| Descriptor construction | `0x4e0a2..0x4e0cc`: copy these constants to the descriptor and set alpha operation 1 and write mask 15 |
| Creation | `0x4e21c..0x4e22a`: output pointer is manager `+0xf8`, descriptor starts at stack `RBP+0x100`, device slot `+0xa0` |
| Color-disabled variant | `0x4e250..0x4e2d8`: clear blend enable and write masks, create into manager `+0x100` |
| Diagnostic draw | `0x48851..0x48884`: bind `+0xf8`, install a cached PS, execute ERG slot `+0x70`, read diagnostic target |
| Result | `0x43ca8` converts a mapped float to an integer; `0x48889` stores the returned low 32 bits at manager `+0x174` |
| State wrapper | `0x6b959` and `0x6b983` tail-call context slot `+0x118` |

The local Windows SDK identifies device slot `+0xa0` as `CreateBlendState` and
context slot `+0x118` as `OMSetBlendState`. Decoding the first render-target
descriptor gives enabled RGB `ONE + ONE`, operation `ADD`, alpha `ZERO + ZERO`,
operation `ADD`, and all color channels writable. Alpha-to-coverage and independent
blending are disabled. These are additive diagnostic state settings; they do not
encode an individual fragment ordinal. The cached replacement PS body and the
general meaning of every accumulated value are not established by this export.

The prior independent workload in `DX11_PIXEL_HISTORY_DIAGNOSTICS.md` supplies
the runtime cross-check: the same pinned original player reports diagnostic 1
for one triangle and 2 for two overlapping triangles, with a single event row.
That existing evidence does not validate arbitrary large counts, all MSAA modes,
or a separate row for every fragment. No original private DLL is loaded by the
new probe or by the product as a result of this checkpoint.

## Independent ordinal probe

`FloraNativePixelOrdinalTests` executes owned draws directly through public D3D11.
Each pass clears private color and D24S8 storage, disables depth testing, and
sets both stencil faces to `EQUAL` with `INCR_SAT` on stencil failure, depth
failure and pass. The comparison reference selects the requested zero-based
ordinal. Ordinary color replacement retains the selected shader's raw uint4
output. A separate compute readback extracts every MSAA sample at pixel `(1,1)`;
no UAV is bound during the draw.

The 144 groups cover three explicit adapters, three geometry layouts, one/four
samples, four shader modes and one/eight render targets. The layouts are ordinary
Draw, instanced Draw with repeated primitive ID zero, and a GS that emits two
triangles per input and explicitly assigns every output primitive ID seven.
Owned no-interpolation input tags independently identify their expected order.
They are fixture data, not a method for recovering arbitrary application instance
IDs. The modes are plain output, late discard of odd tags, the same discard with
explicit early depth/stencil, and sample-dependent `SV_Coverage` output.

Each group checks 8 and 260 overlapping triangles at references 0, 1, 2, 6, 7,
8, 127, 253, 254 and 255, plus an unselected reference draw. This yields 3,168
draws and 7,920 sample vectors. The verifier uses closed-form expectations rather
than the producer's incremental stencil model, verifies GS/PS register linkage,
and checks byte-identical shaders across adapters. It rejects 900 altered
scope, adapter, record, sample, validation-message and query observations.

Observed boundaries:

- References 0 through 254 select the expected sample in these workloads, even
  when primitive IDs repeat or eight render targets occupy all OM slots.
- Reference 255 aliases later samples after stencil saturation. For 260 plain
  overlapping triangles it returns tag 260, not tag 256. This reference cannot
  be used as an unrestricted ordinal selector.
- Late discard and coverage output filter the sequence that advances stencil.
  Explicit early depth/stencil advances it before the discard. These populations
  must remain distinct; a surviving-output index is not a pre-shader sample index.
- The technique changes depth/stencil state. It does not establish original
  depth/stencil eligibility, helper-lane inputs, UAV side effects, original blend
  order in general workloads, or the captured execution's GPU schedule.

The relevant public contracts are Microsoft's
[stencil operations](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_stencil_op)
and [early depth/stencil attribute](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil).
The probe is evidence for a future diagnostic path, not permission to impose a
255-sample limit on the replacement backend. A verified path for larger overlaps,
original state/resource isolation and real-capture validation remain required.

## Retained query failures

The final strict Qt run records **140 passes, six failures, no skips** in 32.630 s.
All 144 groups pass pixel extraction and validation-layer checks before the
strict occlusion assertion. The six failures are Intel 32902, eight render
targets, plain shader mode, all three layouts and both sample counts. Across those
groups, 102 nonzero observations are eight times the expected covered-sample
count. Zero observations remain zero. NVIDIA and WARP match the sample model.

For example, eight full-screen triangles on a 4x4 single-sample target yield
128 samples with one RTV on all adapters. With eight RTVs, Intel reports 1,024;
NVIDIA and WARP still report 128. The public
[occlusion query contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
counts samples passing depth/stencil tests. No division by target count, adapter
normalization, golden replacement or skip is applied. The independent verifier
writes `strict_contract_passed: false` and exits 1. The Qt executable exits 6.

The installed drivers are Intel `32.0.101.8826` and NVIDIA `32.0.16.1742`.
Earlier native presence research already retained differing Intel whole-output
query values. This focused reproduction identifies an RTV-count dependency in
the tested configurations; it does not establish a general driver correction or
waive a P8 failure. It is a new research case, not a newly established P0 exception.

## Reproduction and evidence

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelOrdinalTests --parallel 4
$env:FLORA_PIXEL_ORDINAL_EVIDENCE = '<new-output-directory>'
build/vs2022/Release/FloraNativePixelOrdinalTests.exe -o <new-log-path>,txt
tools/ghidra/Invoke-PixelCountStateResearch.ps1 -Headless <Analyze-Headless.bat> -ProjectDirectory <research-project> -OutputDirectory <new-static-directory>
python tools/validate_pixel_ordinals.py --root <new-output-directory> --ghidra <new-static-directory> --sdk-header <Windows-SDK/um/d3d11.h> --out <new-verification.json>
```

Qt's runtime directory must be on PATH. GPU cases run serially; CTest specifies
a 600-second process deadline and GPU completion waits are bounded to five
seconds. GUI, CLI, Worker and the new probe build in Release. Product source is
unchanged; no portable package or clean-environment acceptance is claimed.

Raw evidence is under `artifacts/dx11-native/pixel-ordinal/`. The first build
failed on an ambiguous evidence writer overload. The initial GPU run had 48 GS
linkage errors caused by the probe's scalar signature packing. Both failures
remain recorded. Using a full uint4 payload aligns the independently compiled
GS and PS signatures; this repairs the fixture, not a backend capability.

The subsequent extraction run passed 146 Qt checks before adding the strict
query assertion. Its 720 evidence files are byte-identical to the final run's
files. The final source preserves the query failures. The compact manifest is
`dx11-pixel-ordinal-evidence.json`; original DLLs, shader binaries, full exports,
logs and executables remain outside Git.

The next implementation boundary is acquisition from an actual GPA draw on
private resources, with explicit sample-population provenance, a non-saturating
path and verified restoration. It must not silently join different ordinal
populations to private shader traces or promote this probe to original-fragment
qualification. Existing history/debugger functionality remains available while
those gates are open.
