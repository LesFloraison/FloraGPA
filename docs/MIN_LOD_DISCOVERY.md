# M2: resource minimum LOD discovery

> Historical discovery at `6c8478f`. The six files below remain unchanged.
> Subsequent [resource LOD implementation](RESOURCE_LOD_AUDIT.md) covers them
> and adds creation/order controls plus an explicit missing-state rejection.
> The original failures and mismatched original-player images remain evidence;
> they are not rewritten as historical passes.

Six unmodified GPA 2025 R1 captures now demonstrate an ordinary immediate-context
compatibility gap. This is discovery evidence, not acceptance of new replay code.
The producer is `tools/native/min_lod_probe.cpp`; the capture registry is
[min-lod-discovery-corpus.json](min-lod-discovery-corpus.json) and immutable result
hashes are in [min-lod-discovery-baseline.json](min-lod-discovery-baseline.json).

The producer fills four mip levels with red, green, blue and white, creates the
texture with `D3D11_RESOURCE_MISC_RESOURCE_CLAMP`, and samples it with a point
sampler at constant coordinates. Expected pixels are generated on the CPU from
the intended mip color. It verifies the getter result and every output byte on
each of twelve frames, both without and with GPA injection. Captures are emitted
through the original `CaptureNextFrame` export; captured bytes are not edited.

| Mode | Application operation | Native and injected result | Original-player result |
|---|---|---|---|
| 0 | Set resource LOD to 0 | Red | Red |
| 1 | Set resource LOD to 1 | Green | Red |
| 2 | Set resource LOD to 2 | Blue | Red |
| 3 | Set to 2, then reset to 0 | Red | Red |
| 4 | Set to 1, then ClearState and rebind | Green | Red |
| 5 | Set to 1 before capture; getter inside frame | Green | Red |

The 144 producer frame checks pass. FloraGPA at `5da80a6` rejects all six files
explicitly at unsupported records, in twelve attempted native replays. Original
playback completes twice per file and produces stable red pixels in every case:
two files match their producer oracle and four differ. An original-player exit
code of zero is therefore insufficient acceptance evidence for this family.
The original adapter/configuration remains unidentified. These observations do
not establish why the original result differs or a general claim about all GPA
frontends, settings or devices. No shader replacement or tolerance is involved.

## Confirmed wire observations

Read-only Ghidra analysis of the installed `shimd3d64.dll` finds Context4 setter
wrapper RVA `0x11e960` from string `0x4bb640` and getter wrapper `0x11f0a0` from
string `0x4bb6d0`. Evidence is `artifacts/m2-minlod-wrappers.c` and its headless log.
The wrappers invoke native vtable offsets `0x1b8` and `0x1c0` and serialize record
types `0x3515` and `0x3516`. Actual capture payloads agree:

| Record | Payload after the capture directory header | Size |
|---|---|---|
| `0x3515`, SetResourceMinLOD | link ID, context ID, resource ID, float LOD | 28 bytes |
| `0x3516`, GetResourceMinLOD | link ID, context ID, float result, resource ID | 28 bytes |

All six files have zero links and a saved immediate context. The getter is an
observation, not a resource write. It must not automatically undo an intentionally
disabled setter during replay experiments. The mode-5 file has no in-frame setter
but does save a nonzero getter result before sampling. That is available evidence
at the getter event, not proof that all pre-frame LOD information is absent and
not permission to infer state before arbitrary earlier resource accesses.

The [D3D11 SetResourceMinLOD contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setresourceminlod)
requires the resource-clamp creation flag and describes resource-wide minimum
LOD. It is distinct from sampler MinLOD. Mode 4 directly establishes that the
producer's resource value survives ClearState; this behavior must not be replaced
with a blanket reset of resource state on every context reset.

## Next implementation gate

1. Decode and execute the saved setter with checked length, references, context,
   resource descriptor and numeric values. Keep unresolved linked/interface
   variants explicit. Verify actual mip-dependent pixels on hardware and WARP.
2. Model the getter as recorded evidence. Establish when an initial observation
   can safely restore a saved snapshot, and distinguish it from later observations
   after an explicit setter. Add a real pre-frame capture with no getter, plus
   before/after resource use and creation-time controls, before defining recovery.
3. Check repeated replay, setter disabling, ClearState, frame-time creation,
   malformed records and affected resource inspection. Missing initial state
   must produce a located diagnosis rather than an unproven zero value.
4. Preserve the original mismatch as a diagnostic comparison. The producer's
   byte oracle remains necessary; do not make red the golden result merely to
   match the current original adapter output.

The combined inventory is now 316 files: 310 in the previously accepted matrix
and six newly blocked discovery files. The prior 310-file regression was not
rerun for this development-only producer/documentation change; production replay
sources are unchanged. Captures, raw images and logs remain local under
`artifacts/m2-minlod-discovery/` and `artifacts/m2-minlod-before/`, outside Git.
The comparison runner's failure exit is expected evidence of these six unresolved
files, not a successful acceptance run. M2 and M3–M6 remain incomplete.
