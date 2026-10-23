# Native DX11 pixel value interpretation

Date: 2026-10-24. This is an independent D3D11 probe of storage interpretation,
not evidence that the GPA debugger or fragment-history engine has been recovered.
The original GPA binaries were not loaded by these probes.

## Observed B5 conversion difference

The first native pixel matrix had 158 passing cases and four failures. On NVIDIA
and Intel, B5G6R5 and B5G5R5A1 texture loads did not equal ideal component
normalization within the original float-rounding tolerance. WARP agreed with the
ideal normalization. The failure log is retained; the tolerance was not widened.

| Format | Component | Ideal storage math | WARP Load | NVIDIA / Intel Load |
| --- | --- | --- | --- | --- |
| B5G6R5 | R | 20/31 = 0.6451612903225806 | 0.6451612710952759 | 0.6470588445663452 |
| B5G6R5 | G | 44/63 = 0.6984126984126984 | 0.6984127163887024 | 0.6980392336845398 |
| B5G5R5A1 | R | 9/31 = 0.2903225806451613 | 0.29032257199287415 | 0.29019609093666077 |
| B5G5R5A1 | G | 12/31 = 0.3870967741935484 | 0.3870967626571655 | 0.38823530077934265 |

The two hardware implementations' observed values match conversion through an
8-bit normalized component for these inputs. This is an empirical observation,
not a claim about all driver versions or a required conversion algorithm. The
[DXGI format definitions](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
identify storage layouts; the
[D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines conversion rules and sRGB accuracy. Neither source is used to assert that
this observed B5 implementation is universal.

## Implementation consequence and independent checks

Keep raw storage and ideal mathematical values separately from a measured native
texture load. B5 and BC1-BC7 decoding uses a private texture on a separately created
D3D11 device. The helper device must match the replay adapter's LUID and feature
level; WARP is used only when explicitly selected. A mismatch or creation failure
fails the operation. There is no fallback adapter or external replay backend.

A one-thread compute shader writes the four Load result components as raw uint32
bits. Tests independently compile a different reference shader, create their own
texture on the original device and compare the four bits exactly. BC tests cover
all 16 positions of a compressed block on WARP, NVIDIA and Intel. Known BC1 red
and green endpoints provide an additional analytic check. BC6H and BC7 use valid
mode-00 and mode-6 payloads respectively; this is not exhaustive mode coverage.

An active pipeline-statistics query on the original device records zero CS and PS
invocations from helper decoding. Tests retain the original constant buffer,
predicate, counted UAV binding and hidden counter value 37. These checks motivate
the separate device: restoring bindings on the original context would not remove
diagnostic dispatches from an already active query.

Ordinary typed formats use project-owned CPU decoding. Integer values are exact;
normalized values represent ideal storage math, not a claim of observed hardware
float32 rounding. Nonfinite values use explicit strings and original raw storage
remains available. Typeless storage needs a compatible typed selection or an
unambiguous command/bound view; an absent interpretation stays unknown.

## Evidence and reproduction

`research/dx11-pixel-decoding-evidence.json` records source and evidence hashes,
adapter observations, test totals and selected real-frame checks. Raw artifacts
remain outside Git under `artifacts/dx11-native/`. The evidence retains the initial
four failures alongside the final successful implementation; no golden output or
skip was changed to hide the difference.

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelTests FloraNativeHistoryTests FloraNativeAnalysisTests FloraGPA.Cli
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(native_pixel|native_history|native_analysis)$' --output-on-failure
```

The final matrix has 207 pixel cases, 91 event-history cases and 40 native-analysis
cases, with no failures or skips. The 207 cases include 52 ordinary typed formats
on three devices, depth/stencil checks, 14 BC variants on three devices, scalar
edge cases and query/binding isolation. This does not prove full format, fragment,
debugging, performance or release parity. Packed/video interpretation, remaining
format/layout combinations and comprehensive BC mode coverage remain open.
