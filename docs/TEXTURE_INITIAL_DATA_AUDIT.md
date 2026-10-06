# Saved texture initial-data validation

Reviewed 2026-10-06. Implementation: `33183fb`. The user authorized prioritizing
M4–M5 while retaining the unresolved M3 acceptance gate. This is the first M4
resource-boundary correction, not completion of either stage. See the
[machine-readable baseline](texture-initial-data-baseline.json).

## Defect and correction

A texture with a nonzero saved data identity could resolve to an empty GenData
payload. Replay previously skipped initializer construction whenever that payload
was empty, then created undefined native storage. Offline validation checked the
GenData envelope but did not verify total texture storage length. A self-owned
fixture reproduced both failures before the fix; the initial test revision had
nine failures, including successful native creation of an invalid empty payload.

`textureInitialSubresources` now shares the checked subresource layout between
preflight and replay. It requires the exact complete byte length, including all
mips, layers and volume slices, and enforces the native slice-pitch limit.
Malformed or unresolved saved data produces `texture_initial_data_invalid`
with both `resource_id` and `data_id`. Replay rejects before native creation.
An explicitly empty texture replacement also rejects.

This covers saved standard single-sample Texture1D/2D/3D storage. A zero data
identity still means no captured initializer; undefined bytes are not invented.
MSAA GenData is not a per-sample initializer and retains its existing warning.
Legacy P010/P016 GenData retains its separate storage limitation and runtime
rejection; verified standard replacements remain a distinct path. Capture-end
reference pixels are not replay resources. No shader or valid-data execution
semantics are intentionally changed.

## Evidence

| Check | Result and scope |
|---|---|
| Trigger | Controlled malformed fixture; not an enrolled original capture |
| Corpus survey | 2,850 initial single-sample texture data references across 460 unique registered captures; none empty |
| Focused suite | 14 passes, no failures/skips, including initialization/cleanup |
| Resource matrix | 1D arrays, 2D arrays, 3D mip chains and BC1 on hardware and WARP; valid bytes read back exactly |
| Negative cases | Empty, short and extra storage; truncated data headers, illegal declared length/type, missing reference and empty replacement |
| Related CTest | 9/9 serial suites pass |
| Before/after corpus preflight | 920 CPU invocations on 460 unique captures; exit code, status, error count and full findings unchanged |
| GPU goldens | GF2 and BF1, each ordinary replay plus disabled-Draw negative control: 4/4 pass |

The nine suites are `texture_initial_data`, `texture_inspector`, `texture_edits`,
`texture_edit_replay`, `texture_creation`, `texture_copies`, `planar_writes`,
`frame_validation` and `core`. Final images and execution counts are retained in
the golden report. The focused test asserts only storage size for absent initial
data, never values for undefined contents.

No new original-player run, full-corpus GPU regression, GUI validation or release
package was produced in this batch. The original-player evidence behind existing
goldens remains historical evidence, not a newly established device-equivalence
claim. Inventory stays 470 registrations / 460 unique hashes; the migration
ledger stays 72 ported / 117 partial / 15 pending. M3, missing capture contents,
M4's remaining boundary audits and M5 stability/deployment gates remain open.

## Reproduction

Build the release CLI and `FloraTextureInitialDataTests` using the VS2022 preset.
Run GPU checks serially with Qt on PATH:

```powershell
ctest --test-dir build/vs2022 -C Release -R '^texture_initial_data$' --output-on-failure -j 1
python tools/validate_native.py --exe build/vs2022/Release/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --out artifacts/texture-initial-goldens-recheck --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --isolated-env
```

The development preflight comparison script and per-file old/new reports are
local artifacts pinned by the baseline. Captures and generated evidence remain
outside Git. The runtime remains C++ and requires neither Python nor GPA.
