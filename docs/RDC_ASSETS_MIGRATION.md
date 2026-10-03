# Native replay inventory and raw texture access

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

`FloraGPA.Rdc.exe` now implements the original `rdc_worker.py` **inventory** and
**texture** actions through the RenderDoc 1.45 native replay controller. These
are the existing analysis interfaces; the ordinary Qt resource/texture views
continue using their independent native implementations.

## Preserved behavior

Inventory returns the selected event, original GPA event/command maps, executed
draw/dispatch actions, complete resource descriptions, texture descriptions and
buffer descriptions. Resource names, IDs, parent/derived relationships,
initialization chunk indices, annotations, sizes, usage flags, sample counts,
mips, array sizes and public format properties are preserved. Resource IDs stay
strings and 64-bit sizes/addresses stay exact integers.

As in Python, the resource list is obtained before setting the selected event;
texture/buffer descriptions are queried afterward. If a capture has no executed
draw/dispatch or mapped GPA command, inventory selects the last structured API
event, or event zero if none exists. It does not invent a draw to make the
capture inspectable.

Texture access selects the explicit GPA resource ID, or color target zero at
the chosen event. It writes the unmodified `GetTextureData` result to
`texture.bin`, with exact byte length, resource identity/name and subresource
metadata in `result.json`. This is raw storage, without display conversion.
For 3D textures the API returns the **whole selected mip volume**: `layer` is
retained in the request metadata but does not extract a W slice. The original
Python adapter calls the same API with the same semantics. Mip/layer/sample
indices use checked uint32 input; an unknown resource or unavailable color
target fails explicitly.

The original `plain()` helper exposes only four `ResourceFormat` properties;
private BGRA/YUV flags are not added to this compatibility result. Similarly,
structured annotation children are exposed through callable API methods and
are not included by `plain()`. The annotation's public name/type/data and every
view of its value union are preserved, including nonfinite floating values.
Its one-byte character view uses Python's surrogateescape convention for bytes
0x80–0xff. `rdcInventoryText()` preserves those exact `\udcXX` wire escapes,
instead of silently substituting Latin-1 or a replacement character. Strict
Unicode JSON readers may reject such unpaired escapes, just as they can reject
the original Python output. Ordinary resource strings retain their UTF-8 text.

## Native job interface

Write a UTF-8 job file; the output directory must be new or empty:

```json
{
  "action": "inventory",
  "capture": "D:/captures/independent_capture.rdc",
  "renderdoc": "C:/Program Files/RenderDoc/renderdoc.dll",
  "out": "D:/results/inventory"
}
```

```powershell
# Run from the repository root; set external paths for your environment.
$JobsRoot = Join-Path (Get-Location) 'artifacts/jobs'
.\FloraGPA.Rdc.exe --job "$JobsRoot/inventory.json"
```

For texture access use `"action": "texture"` and optionally provide
`gpa_event`, `resource`, `mip`, `layer` and `sample`; choose `eid` instead of
`gpa_event` for an explicit native replay event. Omitting both event selectors
uses the same default as the original worker. No Python/qrenderdoc executable
is required by the product. RenderDoc is an explicitly selected optional native
dependency; ordinary standalone frame replay does not load it.

## Verification

`tools/validate_rdc_assets.py` compares the complete native results against the
original Python worker, with exact metadata and byte-for-byte texture checks.
It exercises Hardware/WARP, Unicode paths, disabled and before-event captures,
GF2/BF1, empty action lists, 1D/2D/3D, arrays/mips, signed/unsigned integer
textures, all four samples of a 4x MSAA target, UAV clears and CPU writes.
The Python worker and qrenderdoc are development oracles only.

The native serializer tests cover wide sizes/addresses, resource relationships,
format fields and the annotation union. The optional `--serializer` probe also
compares eight constructed annotations with the original Python `plain()`:
zero, ordinary/extended single-byte characters, infinities, NaN, negative zero
and the maximum uint64 bit pattern. Worker tests cover conflicting event
selectors and negative/overflowing texture indices before loading the backend.

```powershell
# Run from the repository root; set external paths for your environment.
$ReferenceRoot = 'C:/reference/FloraGPA'
$FixtureRoot = 'C:/fixtures'
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
python tools/validate_rdc_assets.py `
  --reference "$ReferenceRoot" `
  --exe out/FloraGPA-replay-assets/FloraGPA.Rdc.exe `
  --qt-bin out/FloraGPA-replay-assets `
  --captures "$FixtureRoot/rdc-capture-package" `
  --history "$FixtureRoot/rdc-history-package-final" `
  --out "$ResultsRoot/rdc-assets"
ctest --preset release -R '^(rdc_assets|rdc_worker)$' --parallel 1
```

The optional `--empty-capture` adds an action-free capture; `--cases` selects
named corpus comparisons. The external fixtures are development artifacts,
not runtime dependencies. GPU jobs must run serially.

The delivered worker passed **34 complete comparisons**: **2,982 resource
records, 703 texture descriptions, 646 buffer descriptions**, and
**22,593,876 exact raw texture bytes**. This includes the action-free capture
and eight additional annotation serializer comparisons. The serializer unit
suite also passed from the delivered directory with a Windows-only PATH:
**5 passed, 0 failed, 0 skipped**.

Core evidence: `artifacts/rdc-assets-package/validation.json`,
`artifacts/rdc-assets-package/annotations-{native,oracle}.json`,
`artifacts/rdc-assets-shipping.txt` and `artifacts/build-rdc-assets-release.log`.

All **5 relevant CTest suites** passed: asset serialization, worker input
validation, Pixel History UI, Replay Metrics UI and Shader Debug UI. The latter
three include real Hardware/WARP jobs with their configured external fixtures.
The delivered CLI passed **4 GF2/BF1 golden-frame and suppressed-draw controls**.
Auditing **38 runtime reports** found no Python/Tk/GPA modules; Qt loaded from
the package, and the selected RenderDoc DLL appeared only in analysis jobs.
All four product executable hashes match Release. Temporary serializer tests
and the Qt Test DLL were removed from the package afterward. These are
same-host deployment checks, not validation on a separate clean machine.

Package: `out/FloraGPA-replay-assets`. Additional evidence:
`artifacts/ctest-rdc-assets.log`, `artifacts/rdc-assets-golden/validation.json`
and `artifacts/rdc-assets-runtime-audit.json`. This batch changes no Qt layout;
the existing GPA-style workspace and independent resource views are retained.

## Remaining migration

The RenderDoc `postmesh` action and its Qt consumer were subsequently connected
in [REPLAY_MESH_MIGRATION.md](REPLAY_MESH_MIGRATION.md). The existing independent
Geometry modes remain separately available. Vendor-specific
metric scheduling, GTPin and other pending modules also remain outside this
batch. Shared modules retain `partial` status; this does not complete the full
Python-to-C++ migration.
