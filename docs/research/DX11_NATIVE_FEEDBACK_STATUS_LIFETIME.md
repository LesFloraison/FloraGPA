# Feedback status destination lifetime

The NVIDIA raw and structured UAV failures from the
[relocation study](DX11_NATIVE_FEEDBACK_RELOCATION.md) depend on the previous
contents of the feedback destination. Initializing that destination to zero gives
a false mapped result in all ten affected executions; initializing it to all ones
gives true in all ten. The corresponding WARP, Intel, typed-UAV and SRV cases are
independent of the seed. This narrows the failure to status production or its
subsequent interpretation on the selected NVIDIA execution path. It does not
establish which compiler, runtime or driver operation is responsible.

Probe implementation: `e07652ecaf94156fc55abe46d01b3a805eef0046`.
No product shader is seeded and the mandatory acceptance test still fails.

## Controlled shader changes

Each seeded shader adds exactly one `mov` immediately before the feedback load.
Its destination is copied from the load's second destination operand, including
its component mask and register index. The source is immediate zero or UINT32_MAX.
Every original instruction remains byte-for-byte unchanged. The load should
replace the initialized destination before `CheckAccessFullyMapped` consumes it.
The probe does not directly interpret or export the opaque status token.

The independent verifier checks the insertion position, destination and value for
all 180 shader variants. It rejects 180 wrong-seed controls and eighteen changed
mapping observations. It also checks that the source data, output sentinels and
all original instructions remain intact. This distinguishes initialization of
the feedback destination from changing the load address, resource or data output.

There are eighteen cases per seed: three buffer types, SRV/UAV and three explicit
adapters. Each case executes the original binding, two relocated bindings and two
independent HLSL compilations for the destination slots, for 180 dispatches total.

| Cases | Zero seed mapped result | All-ones seed mapped result |
| --- | --- | --- |
| NVIDIA raw UAV, all five executions | False | True |
| NVIDIA structured UAV, all five executions | False | True |
| Other sixteen adapter/input cases | True | True |

All ordinary buffer data and sentinels match exact authored expectations; source
storage remains unchanged and the debug layer reports no errors. The NVIDIA
device is vendor 4318/device 11352 with driver 32.0.16.1742. Intel is vendor
32902/device 32103 with driver 32.0.101.8826; WARP is vendor 5140/device 140.
All three report tiled-resource tier two. The evidence does not generalize to
other devices or driver versions.

A separate source-order control places `CheckAccessFullyMapped` before either
output store. Decoded instruction indices verify the change from check seven,
first store six to check six, first store nine. Both NVIDIA cases still fail.
Together with the earlier complete-array binding control, this excludes those
two specific fixture changes as sufficient remedies. It does not prove that all
status-lifetime or binding interactions are irrelevant.

## Acceptance remains failed

The zero-seed Qt run has eighteen passes and two failures; the all-ones research
run has twenty passes. Neither replaces the unmodified mandatory fixture. CTest
explicitly removes all three research environment variables. With every research
variable set in the parent process, the mandatory run still records 42 passes,
two failures, zero skips and CTest exit eight. Its 174 shader artifacts are
byte-identical to the previous unmodified acceptance run.

The ordinary acceptance verifier rejects seeded, reordered or batch-binding
research fixtures. The lifetime verifier reports
`evidence_valid_semantics_failed`, `gpu_feedback_acceptance: false` and
`product_seed_workaround: false`; successful verification of the observed seed
relationship is not successful GPU semantic qualification. The
[manifest](dx11-native-feedback-status-lifetime-evidence.json) pins all probe
sources, shaders, logs, driver inventory and verification reports.

## Consequences for native execution

The [Direct3D specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#TiledResourcesTextureSamplingFeatures)
requires ordinary non-tiled accesses to report fully mapped status and treats
the raw status token as opaque. The independent DXBC executor therefore needs
mapping provenance and status interpretation in its own model; this affected
GPU path cannot be used as an unquestioned status oracle.

Implementation must distinguish a known mapping result from known raw register
bits. A known Boolean does not justify inventing a GPU-specific opaque token.
Status propagation, register moves and trace serialization must preserve that
distinction. Resource provenance must be captured before private resource copies;
a non-tiled diagnostic copy does not prove the original resource was non-tiled
or fully resident. Missing mapping information must remain unknown. Tiled
resources, out-of-bounds accesses and helper-lane suppression require their own
semantic validation. These requirements are pending implementation, and P5/P6
parity plus P7/P8 removal and release acceptance remain open.

## Reproduction

Build `FloraDxbcFeedbackRelocationTests` in Release and run the two commands
serially with the configured Qt directory on PATH. Use fresh output locations.

```powershell
$env:FLORA_FEEDBACK_STATUS_SEED = '0'
$env:FLORA_FEEDBACK_RELOCATION_EVIDENCE = 'artifacts/dx11-native/repeat-status-zero'
& build/vs2022/Release/FloraDxbcFeedbackRelocationTests.exe gpu -o artifacts/dx11-native/repeat-status-zero.txt,txt
$env:FLORA_FEEDBACK_STATUS_SEED = '4294967295'
$env:FLORA_FEEDBACK_RELOCATION_EVIDENCE = 'artifacts/dx11-native/repeat-status-ones'
& build/vs2022/Release/FloraDxbcFeedbackRelocationTests.exe gpu -o artifacts/dx11-native/repeat-status-ones.txt,txt
python tools/validate_feedback_status_lifetime.py artifacts/dx11-native/feedback-relocation-audit artifacts/dx11-native/repeat-status-zero artifacts/dx11-native/repeat-status-ones --out artifacts/dx11-native/repeat-status-validation.json
```

Keep `FLORA_FEEDBACK_STATUS_FIRST` and `FLORA_FEEDBACK_BATCH_BIND` unset for the
seed comparison. Set only `FLORA_FEEDBACK_STATUS_FIRST=1` for the earlier-check
research control. These controls do not alter product behavior.
