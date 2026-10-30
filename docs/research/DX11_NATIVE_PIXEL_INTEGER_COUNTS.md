# Native pixel counts with integer shader outputs

This records the `/2` increment. The subsequent
[auxiliary-target path](DX11_NATIVE_AUXILIARY_PIXEL_COUNTS.md) preserves integer
RT0 coverage when output-merger slot 1 is available.

## Implementation and explicit boundary

`native-pixel-return-count/2` accepts integer color outputs when private float
transport can retain the tested coverage contract. A dedicated
`instrumentPixelReturnCount` transform writes full-precision float-one bits to
RGB at main-entry returns. It does not inherit minimum-integer precision from
the original output declaration. Original executable instructions, declaration
precision and alpha writes remain intact; only RGB declaration masks may expand
as in the previous completion transform. The ordinary completion transform keeps
its existing marker behavior.

The new transform identifies itself as `native-pixel-return-count-shader/1`.
It retains the original signature component type and explicitly reports
`marker_precision: full`, `coverage_output_declared` and
`alpha_storage_is_original_bits: false`. Diagnostic alpha storage follows float
blend arithmetic: retaining original alpha instructions does not imply that
denormals or NaN payloads survive the transport unchanged. No numeric conversion
of original integer alpha is inserted.

When the selected output is RT0, the shader component type is integer,
alpha-to-coverage is enabled, and the shader does not declare an explicit
coverage output, the operation rejects the request. This guard applies to every
adapter, including single-sample targets. It does not depend on vendor IDs or a
guessed conversion rule. Explicit shader coverage disables alpha-to-coverage;
those cases and nonzero selected targets remain available. Rejection uses the
existing private-state recovery path and does not return a fabricated count.

This is an expansion of the previous float-only count capability with a retained
unsupported boundary. It does not close original fragment identity, complete
Pixel History or P6 debugger parity. Public analysis backend selection is
unchanged. RenderDoc removal and standalone packaging remain open.

## Why the boundary is required

The [Direct3D 11.3 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 7.6, 7.8, 7.20 and 17.18, describes output-format interpretation,
untyped register bits, minimum-precision operand annotations, and
alpha-to-coverage. These rules motivated preserving original instructions and
using full precision only for the inserted counter operation. They were not
treated as proof that changing the target format preserves driver behavior.

An initial corrected 480-configuration GPU experiment retained all 1,200 sample
observations. It produced 458 passing Qt checks and 24 failures, with no skips.
All 24 failed groups were NVIDIA RT0 integer alpha-to-coverage cases, containing
58 mismatching samples. For example, original unsigned integer alpha bits `1`
covered all four samples on NVIDIA, while the float-target diagnostic covered
none. Bits `0x3f000000` covered all original samples but only two diagnostic
samples. WARP and Intel original outputs matched the float-bit interpretation
for these cases. Nonzero selected targets retained the original RT0 format and
did not exhibit this difference.

These observations are consistent with format-dependent integer alpha
interpretation on the tested NVIDIA path, but do not establish its internal
driver implementation or justify vendor-specific emulation. Original GPU
references, rather than a mechanical reinterpretation or conversion formula,
determine the comparison. The complete failing experiment remains retained;
the guard reports the unresolved boundary instead of weakening the comparison.

The first attempted baseline was invalid: constant-buffer resource 300 used
resource type `0x81` instead of `0x83`. Its 12 failures occurred during resource
creation, before the former integer-output guard. Those logs and the executable
remain available, but are not backend-regression evidence. A subsequent focused
run of 12 corrected cases passed. An intervening shell command also used an
incorrect executable directory and did not launch a test process.

## Owned coverage model and reproduction

The fixture draws 260 overlapping fullscreen triangles into original integer
RTVs. Two separate one-triangle replays of the unmodified shader measure even
and odd primitive phases. The expected diagnostic count is 130 times the number
of reference phases that wrote the selected sample. Reference RGB must be
exactly `(7,11,13)` or the untouched zero vector; arbitrary nonzero values are
not accepted as coverage. Source frame files for both reference phases and the
260-triangle draw are retained, along with actual adapter identities.

Configurations span WARP, NVIDIA and Intel; uint/int/min16uint/min16int outputs;
one/four samples; one/eight bound targets; ordinary returns, discard, original
alpha-to-coverage, and explicit sample masks with alpha-to-coverage enabled.
Alpha input patterns include zero, one, float-half/one/negative-one bit patterns
and a NaN bit pattern. Minimum precision remains adapter dependent; the test
uses actual original output rather than imposing a particular truncation rule.

The guarded suite distinguishes 336 supported configurations with 840 count
observations from 144 unsupported configurations with 360 rejection/restoration
checks. It does not label the rejected combinations as implemented features.
Six additional DXBC transform checks compare every original instruction and
declaration against the transformed executable, allowing only inserted RGB
markers and the established RGB mask expansion.

## Terminal validation

The guarded suite finishes with 482 passes, zero failures and zero skips in
134.4 seconds. Independent validation checks 840 supported observations and
360 explicit rejections, rejects 1,968 corrupted result/boundary records, and
confirms unchanged source frames and original reference values against the
pre-guard experiment. That experiment remains 458 passes and 24 failures;
its failures have not been relabeled as historical product failures.

Five regression suites finish with 891 passes, zero failures and zero skips:
return counting 189, ordinal replay 164, presence 53, DXBC completion 228 and
geometry replay 257. The separate retained CPU transform run passes eight Qt
checks, including setup/cleanup; its six cases are already included in the
228-check suite and are not counted twice. Original and transformed DXBC bytes
from those six cases are retained.

The regression validator confirms 450 owned observations, rejects 1,800
corruptions and validates nine real-capture controls. GF2 event 113 at (799,480),
BF1 event 2010 at (751,433), and Helldivers event 200 at (168,183) retain counts
one, two and one respectively on all three adapters. Raw queries remain separate;
the Intel Helldivers query still reports 24,960 against 6,240 on WARP/NVIDIA.
GUI, CLI and Worker Release builds succeed. No golden values or skip ranges
were changed.

This establishes the stated incremental scope, not full application parity or
P8 performance acceptance. The guarded suite time includes fixture compilation,
ordinary reference draws, diagnostic attempts and restoration checks. No new
peak-memory or complete-package audit is claimed. The
[evidence manifest](dx11-native-pixel-integer-count-evidence.json) records source
blobs, build logs, executable identities, original/reference frames and raw data.
The manifest audit verifies 4,549 retained files, 12 implementation source blobs,
six original/transformed shader pairs and 186 unchanged prior fixture identities.

```text
cmake --build build/vs2022 --config Release --target FloraNativePixelIntegerCountTests FloraNativePixelReturnCountTests FloraNativePixelOrdinalReplayTests FloraNativePixelPresenceTests FloraDxbcPixelCompletionTests FloraNativePixelGeometryReplayTests FloraNativePixelCompletionProbe FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
python tools/run_pixel_integer_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --out <new-integer-directory>
python tools/validate_pixel_integer_count.py --root <new-integer-directory> --out <verification.json>
python tools/run_pixel_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-regression-directory>
python tools/validate_pixel_return_count.py --root <new-regression-directory> --out <regression-verification.json>
```

GPU processes run serially. The integer runner bounds the process to 900 seconds,
copies its executable and records input hashes and terminal status. An optional
`--pre-guard` argument to its validator compares the retained failing experiment
with the guarded run, including unchanged original reference bytes and frame
digests. Raw frames, binaries and logs stay outside Git under
`artifacts/dx11-native/pixel-integer-count/`.
