# Replay saved occluded Present TEST calls

Reviewed 2026-10-06. Runtime implementation: `f46197f`; development producer:
`64029c7`. This is an M4 compatibility correction for original GPA 2025 R1
legacy DX11 captures. It does not complete general Present or M4/M5 support.

## Reproduced failure

Two unmodified original captures, using discard and sequential blt-model swap
chains, save `Present(0, DXGI_PRESENT_TEST)` with `DXGI_STATUS_OCCLUDED`
(`0x087a0001`) at event 27. The same capture then performs further copies and a
green clear before a successful ordinary Present. The preceding FloraGPA package
rejects both exact registered files at event 27 with `present_unsupported`.

This status is not a device failure. Microsoft's [DXGI status documentation](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-status)
describes an invisible window and excludes flip-model occlusion status.
[Present TEST](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present)
tests the chain without submitting the frame. The actual unchanged binding and
resource bytes are independently measured, rather than inferred from the final
green image.

## Accepted behavior

The existing checked Present decoder additionally accepts the saved occlusion
status when flags are exactly TEST and the swap effect is discard or sequential.
It preserves native bindings and texture contents and permits the following
commands to execute. It does not create a real window or issue native Present.
Replay retains `present_tests` and additionally reports `present_occluded_tests`.
Preflight emits a located informational `present_test_occluded` finding carrying
the captured swap-chain identity, without claiming GPU success.

The exact envelope, chain identity, descriptor, interval, flags and link checks
remain enforced. An occluded ordinary Present, flip-model occlusion, other
statuses and unsupported flag combinations still reject. No missing resource
content is synthesized and no shader is changed.

## Evidence chain

`capture_occluded_present.py` runs the self-owned producer on hardware, WARP and
hardware with the pinned original shim. The two accepted modes each run 12 frames
per backend: **72 producer frames**, with 144 complete before/after red resource
images and independent green final-image oracles. Returned status, actual RTV
binding, minimized state and adapter identities are recorded for every frame.
The tool rejects a generated file unless the requested occluded TEST and later
green clear are present in its original record stream. Capture requests and
successful producer execution alone do not qualify a fixture.

Before and after the TEST, the 8x8 RGBA storage is exactly `(255,0,0,255)` at every
pixel. After the later clear it is exactly `(0,255,0,255)`. Production replay is
checked at both sides of event 27 on hardware and WARP, twice each, including
binding retention. Disabling only the later clear is a separate diagnostic
control that leaves the expected red final image; it is never applied by default.

The serial original-player comparison completes twice for each file. Its exported
images are stable and observed equal to FloraGPA and the independent green image.
The original player's selected adapter remains unidentified, so this is not a
matched-device equivalence certification. Six registered texture boundaries,
each exported twice, independently check red before/after TEST and green after
the clear. Final-image equality alone is not the resource oracle.

The native Present suite passes **27 Qt rows without skips**. New negative
controls cover ordinary/unsupported flags, flip occlusion, all 28 strict prefixes
of each original TEST record and an unexpected returned status. Existing cases
retain missing/wrong references, illegal descriptors, trailing bytes, post-Present
execution and selective unbinding/counter checks. Very short records fail in the
earlier envelope decoder, so their located finding is `record_rejected`; the
initial overly specific test expecting `present_unsupported` is retained in its
failure log. The corrected assertion requires an error at the actual event.

## Release acceptance

The continuous serial gate passes **31 suites, 494 registrations / 484 unique
files**, with 468 native completions (458 unique) and 26 located rejections.
It records 988 ordinary attempts, 684 controls and 728 resource exports.
All previous 492 cases retain exactly equal complete preflight reports,
execution counts and deterministic image hashes. Helldivers remains the
documented variable-image exception. The 33 known capture limitations remain
separate from native completion; the new cases add two scoped producer-fidelity
passes, not universal application equivalence.

Three relevant CTest suites pass across two invocations: Present records,
offline validation and main UI. The full UI suite passes **64 rows without
skips**. A relocated package with system-only PATH additionally passes the
two-capture navigation workflow: Final green, After Event 27 red, back to Final
green. The first UI attempt searched the default GPU-command filter; switching
the test through the existing All API calls selector exposes Present as intended.
That initial test failure is retained. No production filter or UI layout changed.

Four packaged GF2/BF1 golden and disabled-Draw checks pass. Package:
`out/FloraGPA-occluded-present-20261006/` (44 files). The portable UI evidence
uses the native FloraUi test harness, packaged Worker and Qt offscreen plugin;
it is not an independent clean-machine deployment test. Screenshots and the
offscreen font warning are preserved. The current committed capture tool and
producer also regenerate both fixtures successfully into a separate directory;
those fresh pointer identities do not replace the registered originals.

The [acceptance baseline](occluded-present-baseline.json) pins the package,
matrix, independent comparison, original files' manifests, producer controls,
UI results and the initial failing checks. Captures and generated artifacts stay
outside Git. UI test implementation: `14e25ab`.

## Discovery boundaries

Preliminary modes also explore ordinary occluded Present and DO_NOT_SEQUENCE.
These are **not additional accepted capabilities**. In an early dual-chain run,
requesting capture did not produce a file until the already recovered, hash-pinned
original primary selector ran. The resulting files still omitted the target
non-primary operation. Moving the workload to the primary chain produces the
two actual saved TEST statuses. Other discovery files contain only a terminal
ordinary S_OK Present; they do not establish the requested flag/status semantics.
They remain research evidence outside the compatibility registry.

DO_NOT_SEQUENCE also behaves differently before and after establishing a current
buffer in the local native probes. No general transition is inferred from one
setup, and production continues to reject this flag. Original generated captures
are never patched into acceptance fixtures. Synthetic mutations exist only as
explicit negative controls.

## Reproduction and remaining work

```powershell
cmake --build --preset release --parallel 4 --target FloraPresentCaptureProbe FloraPresentRecordTests
python tools/capture_occluded_present.py --producer build/vs2022/Release/FloraPresentCaptureProbe.exe --out artifacts/new-occluded-originals
$env:FLORA_PRESENT_OCCLUDED_CAPTURES="$PWD/artifacts/new-occluded-originals"
ctest --preset release -R '^present_records$' --output-on-failure
```

The five earlier Present originals additionally use `FLORA_PRESENT_CAPTURES`.
Run all GPU work serially. The checked-in [corpus](occluded-present-corpus.json)
uses the specific original files under `artifacts/m4-present-occluded-originals-final/`;
regenerated pointer identities/hashes belong to a new manifest. Producer source,
executable, shim hashes, images and logs are frozen with each capture run.

The new producer source keeps its standalone entry point out of older probes
that include its shared helpers; those probes need no new BCrypt dependency.
The Present, buffer-creation, buffer-copy and texture-copy developer targets all
build successfully. Neither the producer, GPA DLLs nor Python enters the runtime
package.

Ordinary non-S_OK Present, other flags, resize, post-submission rotation, absent
frame-before data, command lists and independent clean-machine deployment remain
open. Module migration counts are unchanged at 72/117/15. Final matrix, package
and UI evidence are pinned in the accompanying acceptance baseline.
