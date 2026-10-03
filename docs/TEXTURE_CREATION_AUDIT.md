# M2: creation-time Texture2D and SRV replay

This change addresses the original late-texture capture discovered during the
strict-dispatch audit. That file previously stopped at event 4 with nine missing
record types. It is now covered by explicit creation and observation decoders.
The accepted profile remains GPA 2025 R1 legacy DX11. M2 is still in progress;
this is not a declaration of complete device-interface or resource coverage.

## Recovered execution

- `0x357a` creates Texture2D storage at the recorded event. Device, returned
  identity, descriptor, initial-data observations and saved storage are checked.
  A resource cannot be materialized from its resource table before creation.
- When the initial-data flag is present, the saved resource blob supplies all
  array/mip bytes. Captured process pointers are never dereferenced. The shim
  serializes only `MipLevels` pointer/pitch observations, although the resource
  blob covers `MipLevels * ArraySize` subresources. Tight replay pitches are
  computed from saved storage; caller padding is not copied into texture pixels.
- Without initial data, creation does not consume the resource's later first-use
  blob. Subsequent captured uploads establish contents. Unspecified contents are
  not synthesized or declared deterministic.
- Failed creation and validation-only `S_FALSE` calls with no returned identity
  remain observations. They do not allocate replay resources.
- `0x357c` creates an SRV of a Texture2D on the recorded device. Explicit and
  default descriptors are supported; native `GetDesc` must reproduce the saved
  active descriptor fields. Existing experimental view overlays are applied
  separately after checking the captured descriptor.
- Texture2D QueryInterface/AddRef/Release/GetDevice/GetDesc and Device5
  CheckFormatSupport/CheckMultisampleQualityLevels1 are strictly sized read-only
  observations. Captured COM counts and CPU query results do not mutate replay
  storage or recreate application CPU control flow.

All new readers reject truncated payloads, invalid flags and trailing bytes.
Creation audits reject malformed references, contradictory descriptors, missing
initial storage, invalid lengths/pitches and reused creation identities. Errors
retain event/resource context in preflight and runtime reports.

## Original evidence and controls

The seven unmodified captures in [texture-creation-corpus.json](texture-creation-corpus.json)
come from `tools/native/texture_creation_probe.cpp`. The producer uses a 16x16
RGBA8 Texture2D array with two layers and two mips, distinct subresource contents
and padded input rows. Its native and GPA-injected runs independently check all
final texture bytes and a texture-dependent 8x8 image on each of twelve frames.

| Mode | Verified distinction |
|---|---|
| 0 | No initializer; all subresources supplied by later uploads |
| 1 | Creation-time data only |
| 2 | Initial data read back before a later overwrite |
| 3 | Null/default SRV descriptor |
| 4 | Failed creation returns no resource; later valid creation succeeds |
| 5 | Validation-only creation returns `S_FALSE`; later valid creation succeeds |
| 6 | Initial data overwritten immediately, with no intervening resource read |

Mode 6 is essential: it verifies that the blob used for initialization still
contains the initial bytes even when the application's first resource operation
overwrites them. The C++ tests check creation boundaries and final bytes on both
hardware and WARP. Synthetic tests cover pre-creation access, metadata bounds,
reference/length corruption, failed calls, duplicate identities and SRV overlays.
Modes 2 and 6 also retain diagnostic-only omission of the first upload: the
initial pixels then produce a strict magenta image instead of the green baseline.
Original captures and shaders remain unchanged.

Ghidra evidence is retained outside Git in `artifacts/m2-creation-wrappers.c`
and `artifacts/m2-texture-refcount-wrappers.c`, with read-only headless logs.
Function RVAs include CreateTexture2D `e4850`, CreateSRV `e56f0`, format support
`f1b50`, MSAA quality `fbc90`, Texture2D QI `2a7770`, AddRef `2a7e70`, Release
`2a83d0`, GetDevice `2a8930` and GetDesc `2ab560`.

Pinned acceptance outputs are recorded in
[texture-creation-baseline.json](texture-creation-baseline.json). Original-player
images are compared exactly, but its adapter selection is unidentified; this
does not establish matched-device cross-implementation equivalence.

## Package acceptance

`out/FloraGPA-texture-creation-20261003/` contains the verified package. Its five
application binaries match the tested Release outputs. Across 131 registered
files, all 262 independent runs complete: 130 files repeat exactly and Helldivers
retains its diagnosed variability. All 122 previously stable images are unchanged.
The 53 corpus controls, four isolated golden/negative checks, nine new storage
boundaries (18 repeat exports), and 16 original-player runs pass. Each new storage
export also matches its independent producer byte oracle, beyond repeat stability.

Fourteen related CTest suites pass; the main UI has 55 passing Qt cases, texture
edit replay has 34, and texture creation has seven, with no skipped cases in those
three suites. All 262 ordinary-run module audits pass. This two-repeat batch is
regression evidence, not an extended-duration M5 reliability certification.

## Remaining boundaries

- Texture1D/Texture3D creation records are decoded, but successful execution is
  explicitly rejected until original creation captures pass their own acceptance.
- Other device/interface creation layouts, RTV/DSV/UAV creation, resource identity
  reuse, linked creation, and deferred/list execution are not added here.
- Implicit mip/descriptor normalization is rejected rather than inferred.
  SRV descriptor sentinel normalization not represented consistently in the
  saved descriptor remains a diagnosable limitation.
- Multisampled initial data, missing saved bytes and unresolved P010/P016 creation
  encodings are rejected. These RGBA8 fixtures do not certify every special
  format, usage, cube layout, sample count or cross-device combination.
- Missing contents and original workload races remain separate from implementation
  gaps. The Helldivers nondeterminism policy and shaders are unchanged.

Next, extend the real creation corpus to 1D/3D and remaining view kinds, then
audit special creation formats and normalization. The ordinary-path gate remains
ahead of M3 command-list work and M6 analyzer expansion.

## Subsequent dimensional acceptance

The Texture1D/Texture3D limitation above describes this historical batch.
[TEXTURE_DIMENSIONS_AUDIT.md](TEXTURE_DIMENSIONS_AUDIT.md) records the subsequent
original-capture acceptance, related metadata and correction of the old minimum
row-pitch assumption. Other creation and deferred boundaries remain open.
