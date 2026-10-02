# M2: buffer creation boundaries and private-data observations

`Device.CreateBuffer` (`0x3578`) now creates native storage at the recorded event.
`Buffer.SetPrivateData` (`0x3017`) now has a checked CPU-observation path. Neither
uses the unchecked auxiliary fallback. This does not complete ordinary replay,
resource versioning, or the remaining M3–M6 work.

## The semantic gap fixed

GPA's buffer resource GenData is not always a creation-time initializer. Our
unmodified original captures distinguish these cases:

| Original application | Resource GenData | Correct replay behavior |
|---|---|---|
| CreateBuffer with initial words `0x11110000 + i` | Initial words | Initialize at CreateBuffer |
| CreateBuffer without initial data, then upload `0x22220000 + i` | Later uploaded words | Create without an initializer; execute the later upload |
| Initial words, then upload updated words | Initial words | Initialize at CreateBuffer, update at the recorded upload |

Previously the CreateBuffer call was skipped and storage was materialized on first
use from resource GenData. An inspection before creation could therefore expose
future content. The new path creates the resource explicitly, checks the saved
descriptor/device/returned identity, uses saved bytes only for an actual initial-data
call, and rejects access until creation has executed. This includes observers at
the before-command boundary, not just replay stopped at an earlier event.

No-initial-data storage remains undefined as requested by the original application;
it is not zero-filled or populated from a later snapshot. This is also the behavior
specified by Microsoft's [CreateBuffer contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createbuffer).
Preflight records `buffer_initial_contents_undefined`, and replay counts
`buffers_created_without_initial_data`. Restoring time-qualified first-use snapshots
for workloads that depend on undefined/partially written regions remains an M4
problem; this change does not certify those bytes or assume them to be zero.

S_FALSE validation-only calls and failed calls with no returned identity are
observed without allocating a replay buffer. Successful linked calls, repeated
resource identities, mismatched descriptors/devices, missing initial bytes and
unexpected returned identities are rejected explicitly. A captured pointer is
never dereferenced. Nonstandard device aliases and resource identity reuse are not
resolved by guessing. The existing RenderDoc resource identity labels are preserved
for explicitly requested independent recaptures.

## Private data boundary

The exact 48-byte SetPrivateData record contains owner, HRESULT, GUID, size and
an original process pointer. It does not contain the private-data payload. The
inspector retains those fields; replay validates them and counts
`private_data_observations` without allocating by size or following the pointer.
Successful calls with nonzero size produce the preflight information item
`private_data_payload_not_saved`. Saved resource debug names remain separate
metadata, as they were before this change.

This is a CPU metadata observation, not a reconstruction of an application's
private-data store, rename history, GetPrivateData-driven CPU control flow or
SetPrivateDataInterface behavior. Ordinary private data and debug naming are
described in the [SetPrivateData contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-setprivatedata).
Checked private-data observations may follow a terminal Present without inventing
buffer rotation or changing GPU bindings.

The layouts agree with the existing API inspector and original shim decompiler
exports in `D:/CDXrepo/FloraGPA/analysis/api_commands/queries.c`: CreateBuffer at
RVA `0xe3880`, SetPrivateData at `0x57e20`. The creation record stores optional
24-byte descriptor and 16-byte initial-data *structure*, then returned buffer ID.
The pointed-to initializer is recovered from the matching buffer's saved GenData,
not from that numeric pointer. GF2 supplies two real no-initial-data creations and
four debug private-data calls; the new producer exercises the other paths.

## Verification and reproduction

`FloraBufferCreationProbe` is a development-only, self-owned DX11 producer. It
reuses the hidden-window/request helpers from `present_capture_probe.cpp`, and is
excluded from default builds and deployment. Five modes cover initial data,
creation followed by upload, validation-only S_FALSE, failed creation, and initial
data followed by update. Each runs 12 frames both natively and under the original
shim. Buffer bytes are read back and checked against an independent CPU pattern.

For the three successful creation modes, the buffer is also consumed as vertex
input: the shader renders green only when the input values are correct, otherwise
magenta. A magenta preclear detects a missing draw. Every produced frame is read
back and checked. The two non-creation modes use a green control image. This lets
original-player image comparisons exercise the created-buffer GPU path rather than
only a constant clear. C++ tests additionally check all 64 buffer bytes at creation
and after the final update on hardware and WARP.

```powershell
cmake --build --preset release --target FloraBufferCreationProbe FloraBufferCreationTests
python tools/capture_buffers.py --producer build/vs2022/Release/FloraBufferCreationProbe.exe --out artifacts/new-buffer-originals
python tools/validate_corpus.py --manifest artifacts/new-buffer-originals/manifest.json --captures-root artifacts/new-buffer-originals --exe out/FloraGPA-buffer-final-20261003/FloraGPA.Cli.exe --out artifacts/new-buffer-validation --oracle-tools D:/CDXrepo/FloraGPA/tools --timeout 60
$env:FLORA_BUFFER_CAPTURES = "$PWD/artifacts/new-buffer-originals"
ctest --preset release -R '^buffer_creation$' --output-on-failure
```

Registered capture hashes are in [buffer-corpus.json](buffer-corpus.json), with
root `artifacts/m2-buffer-gpu-originals`. Regenerated captures use fresh identities
and must use their own manifest. No capture or original DLL is committed.

`BufferCreationTests` checks both complete layouts, all 124 strict call prefixes,
trailing bytes, invalid optional flags, descriptor/resource contradictions, missing
initial data, duplicate identities, creation-before-use ordering, validation/failure
observations, and opaque private pointers/sizes. A no-initial-data fixture deliberately
has an unusable later data reference: successful creation must not read it. No test
asserts a particular value for undefined native storage. Repeated replay creates
fresh storage each time.

Final results, binary/source identities and raw evidence links are recorded in
[buffer-creation-baseline.json](buffer-creation-baseline.json). Earlier compile/test
attempts and clear-only exploratory captures are retained separately; final GPU
producer results are not inferred from those earlier captures. The first producer
attempt hit a WRL conditional out-pointer error in its validation-only mode; it was
fixed to use an explicit raw out-pointer before any successful validation-mode
capture was accepted.

| Final check | Result |
|---|---|
| Prior 91-file corpus | 182 replays completed; 90 stable images unchanged; Helldivers retains known variability |
| Prior five Present captures | 10 repeat-stable independent replays; Present state tests retained |
| Five new buffer captures | 10 repeat-stable independent replays; 10 successful original-player runs with exact matching images |
| Native/shim producer | 120 frames checked against CPU buffer/image references |
| New draw-disabled controls | Six runs produced exact magenta images and zero Draw calls |
| Delivery tests | Eight CTest suites; 55 main UI cases and seven buffer Qt cases, no skips |
| Existing packaged golden/negative checks | Four passed with an isolated runtime environment |
| Explicit optional RDC recapture | Created buffer retains its GPA resource identity in native inventory |

The registered corpus includes the draw-disable controls, selected using native
`inventory` output without editing the captures. Earlier producer manifests retain
their original hashes. The original player's adapter selection remains unidentified;
matching images are observed results, not a claim of matched device/driver settings.
Per-buffer creation/final byte comparisons use the self-owned native producer and
C++ readback; the original player comparison exercises those buffers through the
shader-dependent frame, not an original-kernel buffer-memory dump.

Final local evidence is in `artifacts/m2-buffer-final-corpus`,
`m2-buffer-present-regression`, `m2-buffer-gpu-originals`, `m2-buffer-gpu-validation`,
`m2-buffer-negative-controls`, `m2-buffer-golden`, `m2-buffer-ui` and
`m2-buffer-rdc-inventory`. The independent package is
`out/FloraGPA-buffer-final-20261003/`. The optional RDC check is separate from the
default runtime audits; default replay does not load RenderDoc, GPA or Python.

In the historical 99-type corpus, handling is now 47 execute, 40 metadata and
12 snapshot, with no observed family left in auxiliary-unverified. This is an
implementation-location inventory, not proof that every layout/interface version
or execution path is fully supported. Snapshot/setter semantics, other interface
versions, resource validators, missing data and deferred execution still require
work. Module totals remain 72 ported, 117 partial, 15 pending; M2 remains active.
