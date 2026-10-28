# Typed pixel identity observations and history association

Implementation: `3b6f7b6199d5f2cc32321cb26a371fc3d9466c59` on
`feature/dx11-native-backend`. This P5 continuation decodes private color
predicates and associates a verified system ID with every matching geometry
candidate. It does not establish original capture invocation or fragment order.

## Execution and result semantics

`observeNativePixelIdentity` performs a uniform completion reference, 32 bit
predicates, a full-width equality, a full-width inequality, and a second uniform
reference. Each pass executes a fresh complete original draw on private writable
resources, retaining original shader work, alpha, depth and coverage. The RGB
marker words are `3f800001`, `40000001`, `40400001`. The selected private RTV is
cleared and its blend, logic operation and color write behavior overridden through
the existing completion executor. No additional UAV binding is required.

`decodeNativePixelPredicate` considers only stored RGB channels in the actual
typed RTV format. All zero channels mean false; all finite positive channels
mean true. Missing RGB, unsupported/typeless storage, mixed signs or nonfinite
values remain unknown. Alpha changes and unstored padding do not imply a match.
The callback must select the same texel and sample for every pass; the history
integration uses its existing bounded snapshot implementation.

The candidate is published only after the full equality is positive, the
inequality is zero and the repeated reference is positive. The resulting status
is `verified_conditional_replay`. This means the full-width replay witnessed
that ID; it does not prove identical scheduling across repeated original draws.
Frame, adapter, shaders and bound target are checked across passes. A single
deadline and cancellation check bound the observation. Retained report payload
and individual replay allocations have explicit limits; these limits are not
measurements of total process peak memory.

`collectNativePixelDrawHistory` exposes this through the optional
`observePrimitiveIdentity` setting. Its association scope is
`system_id_equality_only`. Every matching candidate ordinal and every unknown
candidate identity is retained. Multiple matches produce `ambiguous_candidates`.
No unique ordinal, instance or original fragment eligibility is manufactured.
Depth-only selections and draws without a PS are explicitly not applicable.
The default remains disabled for existing callers. The development history
probe accepts `identity`, which also enables the existing shader/merger joins.

## Validation

All GPU work ran serially with explicit WARP, NVIDIA 4318 and Intel 32902.
Drivers were 32.0.16.1742 and 32.0.101.8826 on Windows 10.0.26200.

| Final check | Result |
| --- | --- |
| Predicate and identity suite | 34 Qt checks, 16.239 s, no failures/skips |
| Completion regression | 129 checks, 18.839 s, no failures/skips |
| Geometry regression | 257 checks, 92.137 s, no failures/skips |
| Owned identity joins | 39 selected samples, 1,404 complete-draw passes |
| Real identity joins | 9 selections, 324 complete-draw passes |
| Typed decoder | 78 independently authored storage checks |
| Four independent validators | 8,934 corruptions rejected |
| Release build | GUI, CLI, Worker, history probe and three suites built |

The 30 owned draw cases cover missing/existing/packed system inputs, integer
outputs, discard, forced early discard, sample-frequency MSAA, high GS IDs,
and duplicate GS IDs. In each duplicate case all three candidate ordinals
remain associated with ID 7. There is no arbitrary candidate selection.

Recovery checks on every adapter cancel after five reader calls, reject a
mismatched readback format, retry successfully, and verify original storage.
A deliberately injected zero result for the full equality readback produces
`inconsistent`, retains candidate ID 2 for diagnostics, and leaves the published
ID null. This is fault injection, not a naturally observed driver error. A zero
sample mask yields `no_color_completion` after one pass. These recovery reports
contain another 219 complete passes, outside the 1,728 main history passes.

The Python validator independently decodes raw bytes, checks the complete pass
sequence and exact predicate shader rewrites, and verifies all association
matches. It also invokes the existing storage-history and real merger-history
validators, including exact readback accounting. Existing assertions remain
active. Four prior Intel completion query-contract deviations remain documented
in `DX11_NATIVE_PIXEL_COMPLETION.md`; this increment does not resolve or waive
those discrepancies.

## Real captures and performance

All nine probes match ordinary replay storage and restore original storage.
Each yields one matching candidate, with the following conditional system IDs.
The values are scoped to these selected draws and samples.

| Selection | ID / candidate ordinal | WARP identity time | NVIDIA | Intel |
| --- | --- | ---: | ---: | ---: |
| GF2 event 113, (799,480) | 0 / 0 | 422 ms | 138 ms | 284 ms |
| BF1 event 2010, (751,433) | 2044 / 2044 | 1,020 ms | 246 ms | 651 ms |
| Helldivers event 200, (168,183) | 0 / 0 | 6,387 ms | 1,026 ms | 1,079 ms |

End-to-end probe times range from 1,152 to 11,471 ms and include other analysis
and ordinary replay. These are observations, not controlled performance-baseline
comparisons. In particular, 36 complete draws can be expensive on WARP. Peak
memory, transient module loading and standalone packaging were not audited here.

## Reproduction and evidence

Build `FloraDxbcPixelPrimitiveMaskTests`, `FloraDxbcPixelCompletionTests`,
`FloraNativePixelGeometryReplayTests` and `FloraNativePixelDrawHistoryProbe` in
Release. Put the configured Qt directory on PATH and run GPU commands serially.
Use fresh output directories:

```powershell
$env:FLORA_PIXEL_PRIMITIVE_MASK_EVIDENCE = 'artifacts/dx11-native/repeat-identity'
& build/vs2022/Release/FloraDxbcPixelPrimitiveMaskTests.exe -o artifacts/dx11-native/repeat-identity.txt,txt
```

Run each real selection above with vendor 0, 4318 and 32902:

```text
FloraNativePixelDrawHistoryProbe FRAME EVENT 0 X Y 0 0 0 0 VENDOR NEW_DIRECTORY identity
python tools/validate_native_pixel_identity.py OWNED_DIRECTORY --real REAL_PARENT --out NEW_VALIDATION_JSON
python tools/validate_native_pixel_primitive_mask.py OWNED_DIRECTORY --out NEW_PREDICATE_VALIDATION_JSON
```

`dx11-native-pixel-identity-evidence.json` pins raw reports, logs, shader payloads,
storage, executable snapshots, driver inventory and implementation sources.
The raw evidence and original captures remain outside Git. Earlier successful
development runs are retained separately from the final acceptance matrix.

## Open gates

This diagnostic still requires an existing selected color output, a spare
temporary and compatible PrimitiveID input capacity. It does not resolve
depth-only transport, duplicate IDs, instance attribution, original quad inputs,
resource state at an original invocation, or cross-fragment atomic ordering.
It is not the VS/PS/CS execution engine. The independent C++ DXBC executor remains
the execution path required by the migration plan.

Complete fragment history, public PS selection/debugging, P1 debugger-engine
recovery, remaining P3/P4/P6 parity and P7/P8 removal/release gates remain open.
No reserved public action is unlocked and no RenderDoc-free release is claimed.
