# M2: class linkage and instance creation

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

This increment executes Device5 CreateClassLinkage (`3588`),
ClassLinkage.GetClassInstance (`3195`) and ClassLinkage.CreateClassInstance
(`3196`) at their recorded events. Linkage uses the captured device; instance
creation uses the saved instance/type name and captured index or four offsets.
A resource cannot be loaded from a future snapshot before its creation event.
Nested creation and repeated resource identities without lifetime evidence are
rejected rather than silently assigned a new object.

## Two linkage identity domains

The original shim can give a linkage one ID in API records and another ID in
shader/instance resource snapshots. For example, mode 0 records API linkage 9,
while the saved shader and instance both refer to linkage resource 45. Creating
separate native linkages for those IDs would split the dynamic class namespace.

The identity audit pairs a successful shader/instance creation's returned
resource with its saved snapshot and relates the two linkage IDs. It verifies
that the target is a saved linkage, rejects conflicts and collisions with
existing distinct resource IDs, and retains both IDs for one native COM object.
The same checked mapping is used by shader creation, class creation and object
lookup. Preflight exposes `class_linkage_aliases`; conflicts name the event.
Unused linkage creations still execute even without a resource snapshot.
This is a narrowly evidenced linkage mapping, not general COM identity or
resource-version reconstruction.

## Creation order and captured information

Instances may be acquired before their shader is created; the implementation
preserves that order and does not preload future shaders to populate a type
namespace. This is independently verified in modes 3 and 4 and agrees with the
[CreateClassInstance contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11classlinkage-createclassinstance).
Its constant-vector offset is a vector index, not a byte count. The producer
uses constant buffer slot 2 and vector offset 3, with shader-dependent output.

Instance call records omit the name. A saved type `5/98` instance and its `9/89`
name data provide that immutable name. An unused instance can lack this
snapshot; modes 11/12/21/22 explicitly report unmaterialized metadata and a
per-event handling override. Any saved state requiring that identity, or any
runtime use, is rejected. No name is inferred from a neighboring instance.

Native validation checks the acquired instance's name, creation method,
meaningful index/offsets and owning linkage. InstanceID and TypeID are not used
as portable identifiers. Named instances use InstanceIndex; explicitly created
instances use the four supplied offsets, as distinguished by the
[class instance descriptor contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_class_instance_desc).
GetInstanceName is only authoritative for instances acquired through Get;
created instances use GetTypeName. See the
[GetInstanceName contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11classinstance-getinstancename).

## Original fixtures

[class-creation-corpus.json](class-creation-corpus.json) pins thirteen unmodified
original CaptureNextFrame files from `tools/native/class_creation_probe.cpp`.

| Modes | Captured behavior |
|---|---|
| 0, 1 | New linkage, linked CS, named or explicitly created instance |
| 2 | Two new linkages, first unused, second used for CS and instance |
| 3, 4 | New linkage and named/created instance before CS creation |
| 10, 20 | Existing linkage/CS, instance acquired inside the frame |
| 11, 21 | Unregistered-name instance left unused, then a used instance |
| 12, 22 | Two identical acquisition calls, first instance unused |
| 13 | Two named array indices, separately bound and dispatched |
| 23 | Two explicit types/constant offsets, separately bound and dispatched |

Each probe run verifies twelve frames natively and under the original shim:
312 frame checks. A dynamic CS produces four uint values from a chosen class;
a separate static PS verifies all four values before producing green 8x8 RGBA.
Named and explicit class cases produce different values; the two-dispatch
cases preserve both intermediate and final buffer bytes. Hardware/WARP tests
check those bytes immediately after Dispatch, query the native bound instance
and its owning linkage, and verify creation-time object identities.

Exploratory evidence is retained separately. A null CreateClassLinkage output
pointer caused a native access violation, so it is not used as a validation-only
capture. An unregistered name returned a real instance rather than a failure;
the final modes 11/21 exercise that observed behavior without binding it. The
producer makes no claim that an arbitrary name must fail at acquisition time.
Negative HRESULT/S_FALSE noncreating observations are tested synthetically,
not reported as original-file coverage.

## Read-only observations and rejection tests

Nine additional families have strict wire decoding and original-file coverage:
ClassInstance QI/AddRef/Release/GetDesc/GetInstanceName/GetTypeName
(`3112`, `3113`, `3114`, `311a`, `311b`, `311c`) and ClassLinkage
QI/AddRef/Release (`318e`, `318f`, `3190`). COM counts are observations; they do
not release replay storage. Name queries use checked optional lengths and
buffer flags. The original writers are retained in
`artifacts/m2-class-creation-wrappers.c` and
`artifacts/m2-class-observation-wrappers.c`, with RVA lists and Ghidra logs.

Tests reject every truncated prefix and trailing bytes, invalid presence flags,
contradictory result/identity combinations, bad owner/index/offsets, malformed
names, wrong linkage device, alias conflicts, required use of a missing name,
and use before creation. Modified research copies are negative controls;
acceptance uses the unmodified captures.

## Acceptance and reproduction

Results and source/artifact hashes are pinned in
[class-creation-baseline.json](class-creation-baseline.json).
The release package is `out/FloraGPA-class-creation-final-20261003/`. GPA and Python
remain development-only dependencies; application replay stays C++/Qt.

The 244-file matrix completes 488 independent replays: 243 files repeat exactly,
with only the already documented Helldivers variability. All 230 previously
stable image hashes remain unchanged. The batch retains 253 controls, 88 existing
texture boundary exports, four golden checks and 29 passing CTest suites.
Class creation has 33 passing Qt cases without skips, including 26 hardware/WARP
original-file cases and 30 exact Dispatch buffer dumps. The producer independently
verifies 312 frames. The five packaged binaries match Release; all 488 ordinary
replays pass the runtime dependency audit. These counts measure this matrix,
not universal DX11 support or a whole-product completion percentage.

### Original-player disagreement is retained

The 26 uninstrumented original-kernel runs finish successfully, but **none of
the thirteen new files matches its expected image in that player path**.
Every original export is magenta, whereas the captured resource, native and
shim-injected producers, and FloraGPA hardware/WARP produce green. This is not
reported as successful image parity, and no tolerance or golden hash is changed.

Thirteen additional process-local observation runs use
`tools/probe_original_classes.py`. The pinned private-ABI player calls
`CSSetShader` with a non-null shader and **zero class instances**. At all fifteen
Dispatch boundaries, `CSGetShader` reports no shader/classes; UAV bytes remain
four zero uints before and after execution. The final Draw sees those same zero
bytes in PS SRV0. Constant-buffer contents are present. The observed device is
the NVIDIA GeForce RTX 3070 Laptop GPU (vendor 4318, device 9373), matching the
producer device IDs. Hooked and unhooked exports agree; hooks are restored and
the installed player and captures remain unchanged.

This behavior is consistent with the
[CSSetShader contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-cssetshader):
a shader using an interface requires its corresponding instance. The evidence
localizes the failure to the development original-player binding path, not TGA
decoding or a FloraGPA buffer mismatch. It does **not** certify all original GUI
execution/configuration paths. The general corpus's original adapter remains
unidentified; device information here applies to these diagnostic runs only.

The baseline separates completed original runs, differing files and diagnostic
runs. Correctness of the newly implemented path rests on the independently
checked producer output, unmodified captures, hardware/WARP native binding checks
and exact resource bytes. Original-player image parity remains unavailable for
these files. Raw observations, images, buffers and restoration checks are kept
in `artifacts/m2-class-creation-original-audit-final/` and hashed by the baseline.

Build `FloraClassCreationProbe` explicitly. Arguments are
`<new-output-dir> <mode> [<capture.gpa_frame> <shimloader64.dll>]`, with
`GPA_LOCAL_INJECT=true` for capture. Set `FLORA_CLASS_CREATION_CAPTURES` to the
numbered capture directory and `FLORA_CLASS_CREATION_ARTIFACT_DIR` to retain
hardware/WARP Dispatch buffers. Execute all GPU tests serially. Captures,
original binaries, raw outputs and generated logs remain outside Git.

The accepted identity relation requires matching returned-resource evidence.
Other linkage layouts, general identity/version reuse, broader dynamic-linkage
combinations, deferred execution and frame-before resource/counter recovery
remain open. M2 and M3–M6 are incomplete; module migration counts are unchanged.
