# M2: captured RTV, DSV and texture UAV creation

The GPA 2025 R1 legacy DX11 path now executes Device5 view creation at its
recorded event. `0x357d` creates a texture UAV, `0x357e` an RTV and `0x357f` a
DSV. These records were previously rejected. The common checked creation audit
also serves the already accepted SRV and Texture1D/2D/3D paths.

## Semantics and boundaries

Each view call contains link/device, HRESULT, source resource, a checked
presence flag, an optional descriptor and the returned identity. RTV/UAV
explicit descriptors occupy 20 bytes; DSV occupies 24. Null descriptors retain
the native default behavior. The resulting native GetDesc must reproduce the
saved view before an experiment overlay is applied. Only the active descriptor
union participates in semantic comparison; DSV flags participate too.

The audit checks source/parent/device agreement, resource category, full wire
length and unique creation identity. Failed HRESULTs and validation-only
S_FALSE with no returned identity allocate no resource. Early access is rejected
at the referring event. Repeated identities still require resource versioning.
Texture source resources are accepted; buffer view creation remains explicitly
unsupported, including structured/counter UAV creation. No initial resource
contents or shader changes are invented by this work.

## Evidence

Read-only Ghidra output is retained in
`artifacts/m2-next-view-creation-wrappers.c`, with its index and log. Device5
creation function RVAs are `e6020` (UAV), `e6940` (RTV), `e7270` (DSV).
The optional descriptor sizes and returned identity positions agree with the
unmodified original captures registered in [view-creation-corpus.json](view-creation-corpus.json).

| Family | Producer modes | Checked behavior |
|---|---|---|
| RTV | 0–3 | RGBA8 array/mip, selected subresource clear and dependent shader image |
| DSV | 10–13 | D32_FLOAT depth bytes, selected subresource clear and GREATER depth test with writes disabled |
| Texture UAV | 20–23 | R32_UINT array/mip, uint clear and dependent shader image |

Each group includes an explicit descriptor, a null/default descriptor, a failed
call followed by success and a validation-only call followed by success.
Explicit views target mip 1/layer 1. Default RTV/UAV views cover mip 0 across the
array; the default DSV fixture uses one mip/layer. Initial contents are written
inside the captured frame. A missed clear produces magenta, rather than the
accepted green image, without changing the original shader or file.

The independent producer checks full packed resource bytes and images for twelve
frames both natively and under the GPA shim: 288 checks. Captures, producer
sources/executable, device IDs and oracles are hash-pinned. C++ tests verify
clear-boundary and final bytes on hardware and WARP. Synthetic tests cover all
truncated prefixes, trailing bytes, illegal flags, wrong identities/categories,
repeated creations, early access, descriptor unions, default mismatch and
experiment overlays. An overlay cannot hide a default-view mismatch.

## Metadata coverage

Eighteen view observations use strict checked decoding:

| Interface | QI | AddRef | Release | GetDevice | GetResource | GetDesc |
|---|---|---|---|---|---|---|
| RTV | `3053` | `3054` | `3055` | `3056` | `305a` | `305b` |
| DSV | `301d` | `301e` | `301f` | `3020` | `3024` | `3025` |
| UAV | `3185` | `3186` | `3187` | `3188` | `318c` | `318d` |

QI retains HRESULT/IID/returned identity; reference counts and returned device
or parent identities remain observations. GetDesc uses a checked flag and the
correct descriptor size. Replay does not issue COM lifetime calls or recover CPU
control flow from them. API inspection exposes these fields.

The twelve originals contain the six family-specific GetResource/GetDesc record
types. Although the producer also calls QI/AddRef/Release/GetDevice, the shim does
not emit the corresponding family-specific records in these files. Those twelve
additional layouts have Ghidra writer evidence and synthetic wire/inspection
checks, not real-capture coverage. This distinction is retained in the baseline.

## Acceptance and remaining scope

The verified package is `out/FloraGPA-view-creation-20261003/`. Across 160
registered files, 320 independent runs complete: 159 files repeat exactly and
Helldivers retains its diagnosed variability. All 147 previously stable image
hashes are unchanged. The 24 new original-player runs agree with native output;
24 new boundary exports match producer storage, and 24 new clear-omission
controls pass. Total corpus controls are 85 and boundary exports 88. Four
isolated golden/negative checks and all 320 ordinary runtime module audits pass.
All five packaged application binaries match Release.

Fifteen related CTest suites pass. View creation has 32 passing Qt cases,
texture creation 55, main UI 55 and texture edit replay 34, all without skips.
Before the change, all twelve new files had preflight errors; that baseline is
retained in `artifacts/m2-view-creation-before/validation.json`.

Reproduce the new corpus acceptance with the existing serial developer runner:

```powershell
python tools/validate_corpus.py --manifest docs/view-creation-corpus.json --captures-root artifacts/m2-view-creation-originals --exe out/FloraGPA-view-creation-20261003/FloraGPA.Cli.exe --out artifacts/view-validation-fresh --repeat 2 --oracle-tools D:/CDXrepo/FloraGPA/tools
```

Build `FloraViewCreationProbe` explicitly to regenerate fixtures. Its arguments
are `<new-output-dir> <mode> [<capture.gpa_frame> <shimloader64.dll>]`; capture
runs use `GPA_LOCAL_INJECT=true`. Run modes 0–3, 10–13 and 20–23 serially,
with separate native/captured output directories. Captures and generated outputs
remain outside Git. Original GPA and Python are development-only dependencies.

Final package results are recorded in [view-creation-baseline.json](view-creation-baseline.json).
Original player adapter selection remains unidentified; observed image equality
does not establish matched-device equivalence. Two repeats do not establish M5
long-duration reliability. These fixtures accept the listed texture formats and
subresource patterns, not every MSAA, volume, usage, buffer or device-interface
combination. Buffer views, identity versioning, other creation layouts and
Deferred/Command List remain separate work. M2 and M3–M6 remain incomplete.

## Subsequent buffer view acceptance

[BUFFER_VIEW_CREATION_AUDIT.md](BUFFER_VIEW_CREATION_AUDIT.md) extends creation
to the validated typed/raw/structured buffer view paths and explicit Counter/
Append initialization. Its original fixtures and new-counter provenance checks
supersede this batch's blanket buffer-source rejection. Frame-before counter
recovery and other unverified combinations remain open.
