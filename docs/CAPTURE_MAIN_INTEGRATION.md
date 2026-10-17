# Capture integration acceptance — 2026-10-18

This record covers integration of the DX11 capture branch and creation of the
next DX12 replay development branch. It does not establish complete capture or
replay compatibility, and adds no DX12 implementation or public API design.

## Source and acceptance gate

- Main before integration: `3186c82dcf7535e269f85056cd246101f0aa2baa`.
- Capture candidate: `210a93f55772940b313752cce21474708fea9d2f`
  (`feature/dx11-capture`, 170 commits ahead of main).
- Validated code: `9e5f4d5c90d07ac4a76584436fd24a92c16cfc7b`.
- Local integration branch: `codex/capture-main-integration`.
- Local backup: `backup/main-before-capture-20261018`, at the original main.

The user explicitly revised the merge gate after both the original main and
candidate reproduced the BF1 historical golden mismatch and NVIDIA dynamic
linkage crash: require no new regression against main on this machine, preserve
the known failures, and pass the remaining required checks. Historical golden
hashes and failure assertions remain unchanged. This is a scoped integration
decision, not a claim that all historical tests pass.

Two test-only corrections follow the capture candidate:

- `725d37e`: the six MSAA Query guard variants now accept supported, completed
  in-frame EVENT queries, verify encoding, and still reject pending queries and
  missing End provenance. The old negative expectation predated ordinary Query
  completion support.
- `9e5f4d5`: missing optional original shader-project and API-search fixtures are
  explicitly skipped. A partially present shader project still fails; explicitly
  configured API-search corpus entries retain strict loading and hash checks.

There are no integration changes to production code or `docs/migration.json`.

## Environment and evidence

Fresh Release builds use C++20, Visual Studio 2022 x64, MSVC 14.44.35207 and
Qt 6.11.2 (`D:/Qt/6.11.2/msvc2022_64`). GPU checks run serially. Hardware is an
NVIDIA GeForce RTX 5090 Laptop GPU (10de:2c58, driver 32.0.16.1742), with Intel
Graphics (8086:7d67, driver 32.0.101.8826) also present; WARP checks are separate.

Local evidence, excluded from Git:

- `artifacts/merge-capture-20261018/`: inputs, capture hashes, configure/build
  logs, CTest XML/logs, same-machine comparisons and runtime reports.
- `build/merge-capture-20261018/{full,replay,capture}`: new profile builds.
- `build/merge-capture-20261018/main-baseline`: fresh original-main comparison
  binaries, built from a Git archive of the exact original SHA.
- `out/merge-capture-20261018/{replay,capture}`: new standalone packages.

The three historical inputs are GF2, BF1 and Helldivers files directly under
`D:/CDXrepo`; their full names and SHA-256 values are in `capture-hashes.json`.
Existing output directories, captures and previous evidence were preserved.

The installed official RenderDoc 1.45 files under `C:/Program Files/RenderDoc`
match `third_party/renderdoc/runtime-manifest.json`; no substitute version was
used. The replay package includes this pinned runtime, but ordinary replay
module checks prohibit loading RenderDoc, GPA, Python or Tk.

## Validation

All three profile builds succeeded. Final CTest results are recorded below;
passing suites can contain optional skipped subcases.

| Profile | Result |
|---|---|
| capture-full | 313 passed / 316 unique tests; three known failures below |
| capture-only | 166 passed / 166 tests |
| replay-only | 150 passed / 153 tests; the same three known failures |

Full-profile results combine `ctest-full-smoke`, `ctest-full-capture`,
`ctest-full-replay`, `ctest-query-fix` and `ctest-full-final`, with the latest
result for each test. `full-resolved-tests.json` preserves that mapping. Two
structure tests selected before their binaries finished building were rerun
successfully. An initial external-shader UI timeout under build load passed
unchanged on rerun; both original outputs and corrected runs are retained.

The checks cover the common format reader/container, Query completion, READ
Map, adapter selection, capture UI, replay UI and Worker behavior. Additional
generated GPU checks pass 14 Query/timing/mip cases and 16 MSAA cases. Independent
RGB10A2 conversion and clear-suppression checks pass. The first local RGB10A2
oracle incorrectly assumed floating-point clear quantization; its failed result
is retained, and the corrected check derives conversion from actual 10-bit
storage rather than requiring the wrong green channel.

The replay package's five application binaries match the replay-only build.
Shipping replay targets do not reference capture or writer targets (the common
format writer is still built for its own tests). Capture-only has no Qt/replay
application targets. Static import checks pass for all eight shipping binaries.

The existing golden helper passes GF2 and its draw-suppression control on both
main and the package, then fails BF1's historical hash on both. Separate
same-machine checks pass two repeated BF1 positives and its draw-suppression
control: both versions execute 1,202 draws / 73 dispatches / 4,151 Maps for the
positive case and zero draws / 73 dispatches / 4,151 Maps for the negative case.
Hashes and complete execution-count reports match between main and candidate.
The negative hash differs from the repeated positive hash.

Six packaged Helldivers checks pass: full replay, no draws, before event 18401,
and three runs with dispatch 18401 disabled. Full replay executes 607 draws /
130 dispatches / 2,241 Maps; draw suppression changes the image; disabling the
dispatch produces three identical outputs with 129 dispatches. The pre-sharpen
boundary and disabled-sharpen output also match original main on this machine.
Packaged and supplemental same-machine replay reports use actual GPU output,
prohibit GPA/Python/Tk/capture/RenderDoc modules, and resolve Qt inside the package.

The capture package's protocol-4 manifest matches all nine listed files.
With only Windows system paths in PATH, its bootstrap control passes and its
controller completes 24 sessions / 120 frames plus two MSAA sessions / ten
frames. Cancellation, failure recovery, disconnect and target lifetime checks
pass. All 26 runtime inventories load the packaged capture DLL and contain no
Qt, replay, GPA, Python, Tk or RenderDoc modules. Original historical capture
hashes remain unchanged after verification. `acceptance-summary.json` records
the final revised-gate result and links the detailed control data.

## Known failures and unverified boundaries

- `class_linkage::emptyFunctionTable` crashes in NVIDIA
  `nvGetCompilerInterface` on both original main and candidate. The existing
  device-specific guard does not cover this GPU. No production workaround or
  test quarantine was added during integration.
- BF1's historical RGBA hash remains
  `1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6`.
  Original main and the candidate package both instead produce
  `3d431509ec8258047bc7381367a2861e36ba26abad658bc642524692c07426cc`.
  This also fails `hlsl_recovery::simultaneousBf1Replacements` and
  `recovery_ui::originalCaptureRecovery`. The original-main HLSL test reproduces
  the same actual/expected mismatch. Historical golden runs remain failures.
- GF2 retains its historical exact golden and passing draw-suppression control;
  these do not close the independent capture-side GF2 consistency differences.
- Helldivers retains its existing completion/boundary/control criteria. Its
  full-frame hash is not made a deterministic acceptance requirement.
- Original-workspace specialist corpora, native/Python comparison reports,
  optional external tools, Intel-specific metrics and other environment-gated
  subcases were not all available. The final full and replay Qt logs each
  contain 962 skipped subcases; `full-qt-skips.json`, `replay-qt-skips.json` and
  raw test logs retain their reasons. These skips are not successes; no full
  corpus, clean-host or original-GUI parity is claimed.
- Capture remains **partial**. Endfield capture publication/user acceptance is
  not complete automated replay-fidelity acceptance. Existing unsupported wire
  layouts and pending features retain their explicit boundaries.

The merge preserves the capture history, original capture branch and main
backup. No release is published. The intended next development branch is
`feature/dx12-replay`, starting from the merged main.
