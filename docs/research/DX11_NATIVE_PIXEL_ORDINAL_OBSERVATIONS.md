# Private stencil-reference observations from GPA draws

This P5 increment adds an internal native replay operation and a real-capture
research probe. It preserves original shader execution on private writable
resources while replacing depth/stencil state with an initially zero stencil
selector. It does not establish original fragment identity or complete Pixel
History. Strict Intel selection failures remain visible; public history and
debugger consumers do not use this operation. RenderDoc removal and standalone
release acceptance remain open.

Implementation commit: `f28228a18292b37f07fc694ca9e36d173fe8d43c`.
The baseline main commit remains
`3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28`.

## Relationship to the original GPA research

The preceding [counter-state study](DX11_PIXEL_ORDINAL_RESEARCH.md) pins the
original `dx11_player.dll` identity, Ghidra addresses, blend constants, SDK slots
and independent original-player observations. In particular, the state bound at
RVA `0x48851` implements additive diagnostic counting; it is not proof of a
fragment ordinal. This increment reuses those read-only exports in its verifier.
It does not claim that the owned stencil algorithm is GPA's exact implementation.
No new original GPA engine or private debugger object was recovered here.

The debugger request at RVA `0x902b0` and host `+0x3c0` still lack a verified
non-null implementation object, constructor, vtable and module. Neither this
operation nor the earlier Shader Profiler findings close that research gate.

## Operation and provenance

`replayNativePixelOrdinal` uses the existing private-output replay and restoration
boundary. It accepts a bound 2D RTV slot, a stencil reference in `0..254`, and an
optional returning RGB marker. It creates a D24S8 resource matching the selected
view's logical mip dimensions, array length and sample description. The original
depth view and resources remain available for restoration. Depth testing is
disabled; both stencil faces compare `EQUAL` and increment with saturation on
all outcomes. Reference 255 is rejected because it aliases subsequent overlaps.
This bounded research operation is not a proposed 255-fragment product limit.

The original PS body, coverage output, predication, class instances and application
UAV bindings remain in the replay. No extra application UAV slot is used. The
optional marker uses the existing completion transform, preserves alpha, clears
its selected private target, and overrides that target's blending. Without the
marker, original blending remains active. All observations therefore retain
their diagnostic scope; output bytes are not automatically raw shader outputs.

Reports use `native-pixel-ordinal/1` and
`original_ps_body_with_counterfactual_stencil_reference`. They record requested
reference, target layout, original and replacement depth/stencil descriptors,
force-early flags, adapter identity and logical payload budget. The field
`verified_fragment_ordinal` is always null. Original fragment eligibility,
original depth/stencil eligibility and original execution scheduling are not
inferred. Early-test updates, late survivor updates and no-PS updates have distinct
population labels. Population labels alone do not establish ordering correctness.

The operation rejects incompatible forced sampling, invalid view selections,
unsupported D24S8 sample quality, exhausted budgets and expired/cancelled work.
The API supports 2D mip/array view construction, but the new extraction matrix
does not qualify all array, alias, forced-sampling or viewport combinations.
Existing shared replay paths support additional draw forms; that inheritance is
not a substitute for dedicated ordinal coverage of every form.

## Synthetic extraction and restoration

The extraction fixture embeds owned shaders in GPA frames and covers 144 groups:
three adapters, direct/instanced/GS output layouts, one/four samples, plain/late
discard/early discard/coverage-output modes, and one/eight MRTs. The GS and
instanced cases deliberately repeat primitive IDs. Each group observes references
0, 1, 2, 127, 253 and 254 with and without the marker: 1,728 diagnostic draws and
4,320 sample vectors. Raw bits are checked against an independently implemented
closed-form population model.

Eighteen additional groups exercise append/counter UAVs, class instances,
passing/skipped predication and original depth storage across WARP, NVIDIA and
Intel. They inject observer failures before and after drawing, invalid target
and reference requests, a one-byte budget and cancellation after setup, then
retry successfully. Tests compare original color/depth/buffer storage, hidden
counters, RTV/DSV/depth-state bindings, OM UAVs, predicate bindings and PS/class
instances. The independent ordinary replay matches the private result in these
controlled cases. This is not yet full SO cursor or arbitrary alias coverage.

The final first-run suite results are:

| Suite | Passed | Failed | Skipped | Duration |
| --- | ---: | ---: | ---: | ---: |
| Native ordinal replay | 161 | 3 | 0 | 36.033 s |
| Existing geometry replay | 257 | 0 | 0 | 89.134 s |
| Existing primitive presence | 53 | 0 | 0 | 18.374 s |
| Independent default ordinal probe | 140 | 6 | 0 | 32.341 s |

The serial runner reproduction records the same pass/fail/skip counts in all four
suites, the same three native failing groups and eighteen failing rows, the same
independent configuration outcomes, and nine successful real-capture controls.
Its native, geometry, presence and default-probe durations are respectively
34.693, 90.220, 16.857 and 33.231 seconds. Its aggregate exit is 1.

The initial native run, before final provenance fields and three depth cases,
records 158 passes and the same three failing extraction groups. It remains in
the evidence directory. No assertion, golden or adapter skip was relaxed.

## Retained Intel deviations

The new extraction failures are Intel 32902, single-sample 1x1 targets, coverage
output and eight MRTs, for all three layouts. Eighteen observed rows fail the
model. For example, reference 2 can expose tag 7 where tag 5 is expected, and
other references leave initial storage unchanged. A matching query count does
not establish a matching selected fragment. These are new research failures,
not established P0 exceptions.

The independent public-D3D11 probe reproduces the deviations without GPA replay
or the new API. The controlled variants are:

| Variant | Groups | Groups with extraction failures |
| --- | ---: | ---: |
| 1x1 float, raster multisampling disabled, all adapters | 9 | 3 Intel |
| 1x1 float, raster multisampling enabled, Intel | 3 | 3 |
| 4x4 float, raster multisampling enabled, Intel | 3 | 0 |
| 1x1 integer, raster multisampling enabled, Intel | 3 | 3 |
| 1x1 float, physical 4x4 stencil resource at mip 2, Intel | 3 | 3 |

The last variant retains matching logical 1x1 RTV/DSV dimensions. It does not bind
mismatched output dimensions; Microsoft's [output-merger requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage)
describe those constraints. Neither output type, raster multisample state nor
the tested physical stencil mip arrangement eliminates the small-target issue.
The evidence identifies a configuration dependency, not a general root cause.

The default 4x4 integer probe still has the previous six Intel eight-MRT plain
shader occlusion failures and passes all output-vector checks. Its 102 nonzero
query observations remain eight times the independent model. No division or
normalization is applied. The installed Intel and NVIDIA drivers are respectively
`32.0.101.8826` and `32.0.16.1742`.

## Real-capture controls

Nine explicit-adapter probes select GF2 event 113 at (799,480), BF1 event 2010 at
(751,433), and Helldivers event 200 at (168,183), each on RTV 0 and sample 0 with
reference 0 and the marker. All restore original storage, and their separate
unmodified private draw matches ordinary replay bytes and same-adapter queries.
The reports retain exact original/marked shader bytecode and before, cleared,
marked, restored, original-draw and independent reference storage.

The first-run probe intervals are 171..1601 ms. These smoke measurements include
replay and control work and are not a P8 performance comparison or peak-memory
measurement. Diagnostic query counts can differ from the unmodified draw. Intel
Helldivers reports 19,548 diagnostic and 24,960 original samples, while WARP and
NVIDIA report 4,887 and 6,240. Raw counts remain unchanged. Passing recovery and
same-adapter controls does not qualify cross-adapter counts or fragment identity.

`validate_pixel_ordinal_replay.py` checks all synthetic and real evidence, retained
unknown fields, actual adapter, source identities, storage and control queries.
It rejects 900 synthetic corruptions: 864 native report/model mutations and 36
real report/storage/shader mutations. Its in-memory canonical model only tests
the verifier and never overwrites observed files. Both strict verifiers return
exit 1 and `strict_contract_passed: false` on this machine.

## Reproduction and evidence

Build the five probe/test targets plus the applications before running GPU work:

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraNativePixelOrdinalReplayTests FloraNativePixelGeometryReplayTests FloraNativePixelPresenceTests FloraNativePixelOrdinalTests FloraNativePixelCompletionProbe FloraGPA FloraGPA.Cli FloraGPA.Worker
python tools/run_pixel_ordinal_replay.py --binaries build/vs2022/Release --qt-bin <Qt-runtime-directory> --captures <capture-directory> --ghidra <previous-counter-state-export-directory> --sdk-header <Windows-SDK/um/d3d11.h> --out <new-evidence-directory>
```

The runner requires Python 3.11 or newer, records input SHA-256 identities,
preserves executable snapshots, clears inherited ordinal-probe configuration,
and runs GPU processes serially. Qt processes have 600-second outer deadlines;
real probes have 180-second deadlines. Failures are retained and the aggregate
exit remains nonzero. Do not run another GPU suite concurrently or modify compiled
sources during execution. The Ghidra export can be regenerated using the script
in the preceding counter-state study; original installation files remain read-only.

The first evidence root is `artifacts/dx11-native/pixel-ordinal-replay/`.
The runner reproduction uses `artifacts/dx11-native/pixel-ordinal-replay-repeat/`.
The compact index is `dx11-native-pixel-ordinal-observations-evidence.json`.
Raw captures, original DLLs, logs, shader binaries and executables remain outside
Git. GUI, CLI and Worker Release builds pass. No new frontend default or portable
package is delivered by this checkpoint.

P5 still needs verified original fragment qualification, ordering beyond stencil
saturation, complete failure attribution and reconstruction across the full
capability matrix. P6 original-pixel debugging and full execution parity, P7
dependency removal and P8 clean-environment acceptance remain open. These failures
must not be hidden by promoting diagnostic observations to original fragments.
