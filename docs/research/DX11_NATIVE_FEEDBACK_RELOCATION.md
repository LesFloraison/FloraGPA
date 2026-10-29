# DXBC feedback load relocation and unresolved mapping status

Native UAV inspection and relocation now walk the status-producing load operands
instead of rejecting every `ld_uav_typed_s`, `ld_raw_s` and `ld_structured_s`
instruction. This removes a parser barrier, including an erroneous rejection of
raw and structured SRV reads. GPU qualification remains incomplete: the strict
test has 42 passing entries and two failing NVIDIA scenarios. The failures remain
in the mandatory CTest entry and block feedback acceptance.

Implementation: `538c2670bac222238c74b72ff623508ddfb45977`.

## Instruction and resource boundaries

The operand walkers recognize four operands for `ld_s` (223),
`ld_uav_typed_s` (225) and `ld_raw_s` (226), and five for
`ld_structured_s` (227). They preserve the extra destination, addressing operands,
instruction extensions, resource swizzles and literal values. Only static UAV
register indices change. Existing missing-declaration, collision, device-slot and
reflected-range checks still apply. No DXBC execution semantics or residency
tracking is implemented by this change.

The [Direct3D functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#TiledResourcesTextureSamplingFeatures)
defines the status result as opaque and requires dedicated interpretation. It
also specifies fully mapped behavior for ordinary non-tiled resources. The final
fixtures use only `CheckAccessFullyMapped(status)` and compare its Boolean result;
they do not interpret or export the opaque token as a numeric result.

## Checked transformations

Twenty-four compiler fixtures cover typed, raw and structured inputs, SRV/UAV,
CS/PS, and retained/stripped reflection. Every fixture relocates input UAV zero
to seven and sixty-three and checks an exact instruction round trip. PS fixtures
also reserve a coverage target, move the output UAV, and restore the original
instruction stream. Truncated feedback operands, missing UAV declarations and
collisions remain errors. A literal resembling a UAV operand is preserved.

The independent Python verifier decodes the stored shader chunks and checks that
only the selected UAV indices change; all other instruction words must remain
equal. It does not load either RenderDoc or GPA. The previous implementation
rejected twenty of these layouts at feedback inspection; four typed-SRV controls
also exposed missing truncation validation for `ld_s`.

## GPU results and the open failure

The final matrix has eighteen CS scenarios: three buffer types, SRV/UAV and three
explicit adapters. Each executes the original shader, relocations to slots seven
and sixty-three, and independent HLSL compilations for those destination slots.
The ninety dispatches use ordinary initialized buffers. All data components,
constant sentinels and source-buffer preservation checks pass. D3D11 debug-layer
error checks also pass. This matrix does not cover tiled resources, PS execution,
texture feedback, out-of-bounds reads or full replay integration.

| Adapter | Passing scenarios | Failing scenarios |
| --- | ---: | --- |
| WARP, vendor 5140, device 140 | 6 | None |
| NVIDIA, vendor 4318, device 11352 | 4 | Raw UAV and structured UAV mapping status |
| Intel, vendor 32902, device 32103 | 6 | None |

All three devices report tiled-resource tier two. NVIDIA produces a false mapped
result in some ordinary-buffer executions. Independently compiled destination
shaders also produce false results, and the exact failing executions vary across
runs. A separate probe binding the complete 64-slot UAV array at once still fails
both scenarios. These observations do not isolate the cause to the rewriter,
compiler, shared D3D11 fixture, runtime or driver. In particular, this is not an
approved historical-failure exception and must not be used to close a release
gate. Further independent execution and status-lifetime probes are required.

The final CTest invocation exits eight. Its Qt log records 42 passed, two failed,
zero skipped, with both failures identified above. The evidence validator checks
24 layouts and eighteen GPU records, reports sixteen qualified scenarios and two
unqualified scenarios, writes `gpu_feedback_acceptance: false`, and exits one.
The pre-existing coverage and format suites pass five and sixteen entries.
GUI, CLI and Worker Release builds succeed.

## Preserved failed investigations

The first GPU fixture exported the opaque status directly and used a scalar
typed-buffer load that the compiler emitted with a `.wxxx` resource swizzle.
Its five failures are not evidence of valid scalar-load semantics. The corrected
fixture observes only the interpreted mapping Boolean and uses a vector typed
load, checked against the independent R32_UINT expectation `[102,0,0,1]`.
The two NVIDIA status failures persist after those corrections.

One corrected-fixture Intel run returned `DXGI_ERROR_WAS_STILL_DRAWING` from
nonblocking Map after query completion. The probe now retries that result within
the original five-second readback deadline; other errors and expiry still fail.
The complete final run passes all six Intel scenarios. Initial compiler, fixture
and runtime failures remain in the evidence directory rather than being replaced
by the final logs.

## Reproduction

Build `FloraDxbcFeedbackRelocationTests` in Release. Run GPU tests serially, use
fresh evidence directories, and read Qt logs only after the process terminates.

```powershell
$env:PATH = 'D:\Qt\6.11.2\msvc2022_64\bin;' + $env:PATH
$env:FLORA_FEEDBACK_RELOCATION_EVIDENCE = 'artifacts/dx11-native/repeat-feedback'
& build/vs2022/Release/FloraDxbcFeedbackRelocationTests.exe -o artifacts/dx11-native/repeat-feedback.txt,txt
python tools/validate_feedback_relocation.py artifacts/dx11-native/repeat-feedback --out artifacts/dx11-native/repeat-feedback-validation.json
```

`FLORA_FEEDBACK_BATCH_BIND=1` selects the complete-array binding research control;
it does not change product behavior or relax assertions. The default CTest entry
is serial, has a ninety-second process timeout, and retains the full GPU matrix.
The [evidence manifest](dx11-native-feedback-relocation-evidence.json) pins the
sources, logs and shader/result artifacts. No P5/P6 parity or P7/P8 removal and
release gate is closed by this increment.
