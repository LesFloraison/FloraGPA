# Full-complement verification of conditional pixel identities

This corrects an overstatement and implementation gap in the earlier
[identity-join study](DX11_NATIVE_PIXEL_IDENTITY_JOINS.md). It strengthens an
internal P5 observation; it does not establish original fragment eligibility,
instance identity, original execution order or complete Pixel History.
Implementation: `8a8c3471c1022cacabbbc47f3630ec3a706e17e0`.

## Defect and correction

Version `native-pixel-identity/1` constructed a candidate from 32 bit predicates,
tested equality with that candidate, and tested equality with `candidate ^ 1`.
The latter was called `different` and described as a full-width inequality. It
only checked one neighboring value. A different ID separated by other bits could
escape that control, especially if the repeated draws or readbacks were
inconsistent. The equality pass still witnessed its candidate in its own replay,
but the recorded negative control did not cover the candidate's full complement.

`PixelPrimitiveMask::notEqual` now requests `(SV_PrimitiveID & mask) != value`.
The independent DXBC transformation emits `INE` for that complete comparison;
it does not flip a candidate bit. Original instructions, alpha, depth, discard
and return behavior retain the existing transformation contract. New comparison
shaders and observations use version 2 and record `primitive_comparison` as
`not_equal`. Existing equality predicates keep their version 1 identity and bytes.
The shared MAX-presence transport also distinguishes the complement version.

`native-pixel-identity/2` uses mask `ffffffff`, the unchanged candidate value and
the complement predicate for its `different` pass. Its 36-pass sequence and
bounded private-resource execution remain unchanged. Results are still called
`verified_conditional_replay`: a positive equality and negative complement in
separate draws do not prove original scheduling or identify a unique instance.

The validators understand historical version 1 records according to their actual
neighbor-only semantics. `--require-complement` refuses to count them as new
full-complement acceptance. No historical golden or raw report was changed.

## Independent checks

The GPU fixture retains all 34 previous predicates and adds two true inequalities:
one against the actual returning ID (false) and one against that ID XOR 2 (true).
Thirty fixtures cover three explicit adapters and ten modes, including missing,
existing and packed inputs, integer outputs, discard, forced early depth,
sample-frequency MSAA, high GS IDs and duplicate GS IDs. The independent Python
validator reconstructs expected instructions and checks actual output words and
preserved alpha. All 1,080 predicate observations pass.

On each adapter a targeted fault injection fabricates candidate 4 in the bit
readbacks and fabricates a positive equality. The actual draw returns ID 2.
An actual GPU equality check against the old neighbor, 5, is zero. The new GPU
inequality against 4 is positive, so the result is `inconsistent` and the published
ID stays null. This is deliberate readback fault injection, not a naturally
observed driver error. It demonstrates the distinction that the old neighbor
control could not test. Cancellation, wrong-format rejection, retry, no-color
completion, the previous equality fault injection and resource restoration remain
in the suite.

The final Qt suite passes 34 entries in 17.258 seconds without failures or skips.
It contains 39 selected-sample history joins; nine real capture joins bring the
total to 48 histories and 1,728 complete-draw identity passes. The shader validator
rejects 6,480 corruptions, and the identity validator rejects another 288, including
changed comparison arguments. Seventy-eight typed decoder checks remain active.
The explicit legacy acceptance command fails with the expected full-complement
gate error, after validating the old records as version 1 observations.

A comparison with the original identity evidence manifest verifies the recorded
hash of every old payload before comparing bytes. All 1,080 old original,
reference and equality-predicate shader payloads remain byte-for-byte identical.
The new inequality instructions are additional coverage; old predicates were
not removed to make the suite pass.

## Real captures

All jobs run serially and use the explicitly selected adapter. No adapter fallback
or external capture backend is involved. Each history agrees with ordinary
reference replay storage and restores the original storage. These are the same
selection points as the earlier study; they do not establish whole-frame parity.

| Selection | Conditional system ID | WARP identity time | NVIDIA | Intel |
| --- | --- | ---: | ---: | ---: |
| GF2 event 113, (799,480) | 0 | 356 ms | 66 ms | 226 ms |
| BF1 event 2010, (751,433) | 2044 | 960 ms | 66 ms | 286 ms |
| Helldivers event 200, (168,183) | 0 | 5,797 ms | 93 ms | 156 ms |

These times are observations, not controlled performance comparisons. The pass
count remains 36. No peak-memory, clean-profile packaging or module audit is
claimed by this increment. GUI, CLI, Worker, predicate tests and the history probe
build successfully in Release.

## Reproduction

Use fresh directories, the configured Qt runtime and serial GPU execution:

```powershell
$env:FLORA_PIXEL_PRIMITIVE_MASK_EVIDENCE = 'artifacts/dx11-native/repeat-complement'
& build/vs2022/Release/FloraDxbcPixelPrimitiveMaskTests.exe -o artifacts/dx11-native/repeat-complement.txt,txt
python tools/validate_native_pixel_primitive_mask.py artifacts/dx11-native/repeat-complement --out artifacts/dx11-native/repeat-complement-mask.json
python tools/validate_native_pixel_identity.py artifacts/dx11-native/repeat-complement --require-complement --out artifacts/dx11-native/repeat-complement-identity.json
```

For each capture/selection above, run the history probe once with vendor 0
(explicit WARP), 4318 and 32902. Resource zero selects bound color target zero:

```text
FloraNativePixelDrawHistoryProbe FRAME EVENT 0 X Y 0 0 0 0 VENDOR NEW_DIRECTORY identity
python tools/validate_native_pixel_identity.py OWNED_DIRECTORY --real REAL_PARENT --require-complement --out NEW_VALIDATION_JSON
```

The [evidence manifest](dx11-native-pixel-identity-complement-evidence.json) pins
1,225 files covering reports, original and transformed shaders,
reference storage, logs and validators. The original legacy manifest remains the
source for old shader hashes. Full fragment history, original instance mapping,
debugger parity, RenderDoc deletion and standalone release remain open gates.
