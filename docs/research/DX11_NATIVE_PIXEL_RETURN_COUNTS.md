# Native pixel return counts on private float targets

This records the `/1` increment. The subsequent
[integer-output increment](DX11_NATIVE_PIXEL_INTEGER_COUNTS.md) adds bounded
integer support and documents an RT0 alpha-to-coverage incompatibility.

Implementation: `20892ca4fc2a907e97d2188e390e2faa4d056423`.
Previous branch head: `de70fd7e5498a6dde8313ecff2f96b80df57fad1`.

## Implemented behavior

`replayNativePixelReturnCount` executes the original draw and shader stages on
private writable resources, redirects one existing float PS output to an owned
RGBA32F target, and accumulates one RGB unit per surviving return. It reads the
requested pixel, RTV-relative array layer and sample directly from that storage.
No application UAV slot is consumed. It does not derive the result from an
occlusion query, serialize primitives or infer an invocation's original identity.

The original PS executable is retained through the existing return-marker
transform. RGB is set to one before its main-entry returns; original alpha,
discard, depth and coverage outputs remain. RGB blending is `ONE + ONE / ADD`;
alpha replaces the previous diagnostic alpha. The selected target's write mask
and logic operation are overridden for diagnostics. Other targets retain their
private original storage and blend settings. The complete draw retains its
depth/stencil state, sample mask, alpha-to-coverage, predication, original UAV
bindings and class instances. The diagnostic target starts at zero even when
the application draw is predicated off.

The transport matches the selected 2D RTV's effective mip dimensions, view layer
count, sample count and quality. Original first-layer/mip identity and the
relative selection remain in the report. Single-sample, array, MSAA and MSAA-array
transports use the existing native readback paths. Allocation and shader payloads
remain bounded by the request; existing cancellation, deadline and private-state
restoration apply during setup, draw and readback.

Reports use `native-pixel-return-count/1`. `return_count` includes all four raw
component words, validity, a decoded count and its validity reason. RGB channels
must agree and describe a finite nonnegative integer strictly below 2^24.
At or above 2^24, the count is unknown: repeated unit additions can stop changing
float32 storage. Negative, fractional, nonfinite and inconsistent values are also
unknown, never rounded or clamped into a count. Diagnostic stored alpha words
remain available independently of RGB count validity. They follow float blending
rules and are not guaranteed to preserve original shader alpha bits.

The [Direct3D floating-point rules](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
specify arithmetic precision for fixed-function blending as well as shader
execution. Exact unit accumulation within the representable integer range is
the arithmetic basis of this transport; the retained GPU fixtures independently
exercise its actual behavior on the three selected adapters.

`passing_samples` and pipeline statistics remain raw whole-draw observations.
They are not replaced with the selected-pixel count. For example, an Intel
eight-MRT fixture retains an occlusion value of 2080 while the private target
records the expected 260 completions. This addresses acquisition of a diagnostic
count without pretending that the earlier query discrepancy was corrected.

## Scope and remaining gates

This increment supports an existing float-component PS output on a bound 2D RTV.
Its storage need not originally be RGBA32F: UNORM, sRGB and FP16 source targets
are covered. Integer-component shader outputs are explicitly rejected rather
than reinterpreting their alpha contract. Depth-only/no-color outputs, 1D/3D RTVs,
forced sample counts and counts beyond the exact float32 range require additional
work. A selected output with disabled original color writes still counts diagnostic
completions; it does not prove an original resource modification.

`original_fragment_eligibility` remains unknown, and original GPU scheduling is
not claimed. The result describes this diagnostic replay's surviving sample
population. It supplies neither original primitive/instance identity nor an
ordered fragment list, and does not establish a fragment's individual failure
reason. It is an internal P5 capability and a research-probe mode, not a public
history/debugger backend cutover. The analysis and executor identities remain
`dx11-native/12` and `dxbc-gpu/31`; RenderDoc removal and independent package
acceptance remain open.

## Research provenance

The prior [GPA counter study](DX11_PIXEL_ORDINAL_RESEARCH.md) pins
`dx11_player.dll` SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b` and its
additive RGB descriptor construction at `0x4e0a2..0x4e0cc`, blend-state creation
at `0x4e21c..0x4e22a`, diagnostic draw at `0x48851..0x48884`, and mapped-float
conversion at `0x43ca8`. The owned implementation uses public D3D11 operations
and the project's independent DXBC transform. It preserves original alpha and
coverage rather than reproducing GPA's alpha-zero diagnostic descriptor.

These earlier Ghidra observations motivate the additive transport but do not
establish the body or full semantics of GPA's cached replacement shader. This
increment performs no new GPA decompilation, loads no private GPA API in its
implementation, and does not close the debugger host `+0x3c0` engine chain.

## Validation

GPU processes ran serially on WARP, NVIDIA and Intel. The final run contains:

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Native pixel return count | 189 | 0 | 0 |
| Native pixel ordinal replay | 164 | 0 | 0 |
| Native pixel presence | 53 | 0 | 0 |
| DXBC pixel completion, including depth completion | 222 | 0 | 0 |
| Native pixel geometry replay | 257 | 0 | 0 |
| Total | 885 | 0 | 0 |

GUI, CLI and Worker Release builds also pass. The new suite includes 144 count
configurations covering direct, instanced and GS draws, repeated primitive IDs,
one/eight MRTs, one/four samples, ordinary output, late discard, early depth/stencil
with discard, and coverage output. The 260 overlapping fragments cross the
eight-bit stencil saturation boundary. Closed-form expectations are 260 ordinary
completions and 130 surviving completions in the filtering cases.

Additional fixtures cover UNORM/sRGB/FP16 storage, a nonzero mip and first array
slice, MSAA arrays, alpha-to-coverage zero, sample masks, disabled draws, depth and
stencil interactions, and original write masks. Selected layer/sample coordinates
and original depth storage are checked. Unselected MRT bytes match an ordinary
replay. Original color storage and OM/PS/predicate bindings are restored after
diagnostics. Append/counter UAV data and hidden counters match an ordinary draw
inside the private replay and retain their initial values afterward.

Recovery controls cover exceptions before and after GPU execution, cancellation
at both observer boundaries, timeout, insufficient budget, invalid coordinates,
unsupported integer outputs, predicate pass/skip and successful retry. Decoder
controls include the largest exact accepted integer, the precision boundary,
nonfinite/fractional/negative input and inconsistent RGB words.

The independent verifier checks 450 owned observations and rejects 1,800 altered
count, raw-bit, transport, blend and provenance records. Fifteen ordinary
eight-MRT Intel sample reports retain the known whole-output query discrepancy;
their independently expected selected-pixel counts are correct. No query values
are divided, rewritten or hidden. The default ordinal suite's original fixtures
and shader identities remain unchanged across all 144 configurations after their
builder was moved into a shared header. Historical golden files were not edited.

## Real-capture controls

The research probe now accepts `--return-count`. At the same selected event/pixel
on all three adapters, its diagnostic counts are:

| Capture | Event | Pixel | WARP | NVIDIA | Intel |
| --- | ---: | --- | ---: | ---: | ---: |
| GF2 | 113 | 799, 480 | 1 | 1 | 1 |
| BF1 | 2010 | 751, 433 | 2 | 2 | 2 |
| Helldivers | 200 | 168, 183 | 1 | 1 | 1 |

All nine controls preserve the selected original storage and reproduce an
ordinary replay's pixel through a separate unmodified private draw. Source and
marked shader bytes are saved and independently hashed. These checks demonstrate
execution and restoration on real captures; agreement across adapters does not
establish original fragment identity or serve as a complete ground truth for
their fragment populations.

The probe retains whole-draw query differences between adapters. In particular,
Helldivers records 6240 on WARP/NVIDIA and 24960 on Intel, alongside a selected
diagnostic count of one on each. These domains must not be conflated.

Measured real-probe durations range from 176 to 1766 ms, including setup and the
additional restoration/reference controls. They are not isolated counter timings,
P0 performance comparisons or peak-memory measurements. Payload limits exclude
driver overhead; the full P8 performance and module audits remain necessary.

## Reproduction and retained evidence

```text
cmake --build build/vs2022 --config Release --target FloraNativePixelReturnCountTests FloraNativePixelOrdinalReplayTests FloraNativePixelPresenceTests FloraDxbcPixelCompletionTests FloraNativePixelGeometryReplayTests FloraNativePixelCompletionProbe --parallel 4
python tools/run_pixel_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-directory>
python tools/validate_pixel_return_count.py --root <new-directory> --out <verification.json>
FloraNativePixelCompletionProbe <frame> <event> <slot> <x> <y> 0 <vendor> <new-directory> --return-count
```

The research CLI currently restricts its real-capture comparison to plain mip-zero
non-MSAA RTVs; the native API's wider layer/mip/MSAA support is exercised by the
owned tests. The runner retains process exits and input hashes and copies the
executables. Owned frames and all real-probe outputs stay outside Git under
`artifacts/dx11-native/pixel-return-count/`.

The initial build request mistakenly included the nonexistent target
`FloraNativeDepthCompletionTests`; MSBuild reported MSB1009 after building the
preceding targets. Its log is retained. Depth completion is actually covered by
`FloraDxbcPixelCompletionTests`, which passes in the final run. Intermediate GPU
runs also remain available: 150 initial checks, 41 transport/UAV checks and 189
preservation checks all passed. The [evidence manifest](dx11-native-pixel-return-count-evidence.json)
pins implementation sources, terminal logs, retained fixtures, raw reports,
shader bytes, executable copies and prior fixture identities.
