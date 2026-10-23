# Native DXBC texture input checkpoint

This P6 checkpoint adds native texture observations to the independent execution
input model. It does not implement texture instructions, sampling, pixel quads or
production debugger cutover. The full RenderDoc removal acceptance remains open.

## Observation contract

`snapshotResourceInputs` observes actual CB/SRV/UAV bindings inside one prepared
before-work replay boundary, after experiment changes and before their restoration.
The older internal `snapshotBufferInputs` name delegates to this same implementation.
Provenance is `independent-replay-resource-inputs/3`; probe reports use schema 2.

Texture storage uses the native object's canonical base descriptor, preserving the
GPA resource identity. It retains all stored non-MSAA mips, array layers and 3D depth
slices in the project's packed subresource order. This includes native backbuffer
format normalization and the descriptor of an experimental input clone. Row and depth padding from staging
maps is removed. The resource's observed minimum LOD accompanies storage; copying
does not reset that clamp or remove mips excluded from shader access.

The actual native view descriptor supplies format, dimension, mip and array range.
Cube-array counts are converted to face counts with checked arithmetic, following
[D3D11_TEXCUBE_ARRAY_SRV](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_texcube_array_srv).
3D UAVs retain their selected depth slices. Where the extended view interface is
available, plane selection comes from `GetDesc1`, as described by
[D3D11_SHADER_RESOURCE_VIEW_DESC1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/ns-d3d11_3-d3d11_shader_resource_view_desc1).
An unavailable plane selection remains unknown. Raw base view words are retained
alongside normalized ranges; resource format and view format remain separate.

Aliased views share one private storage snapshot. The default 256 MiB budget counts
unique buffer and texture backing bytes together, before each readback. This is a
storage budget, not a process peak-memory cap: validity arrays, replay resources,
staging allocations and capture storage also consume memory. Cancellation is checked
around each mapped subresource, with every mapped resource unmapped before a
cancellation exception. A native blocking `Map` still requires the Worker process
deadline to contain a hung driver.

MSAA views keep their GPA identities and descriptor selection with an explicit
unavailable-storage reason. A resolved image or one sample's bytes cannot stand in
for full multisample storage. Null native slots remain independently represented as
verified unbound slots. Texture bytes are not entered into the buffer memory map.

Known byte flags mean these bits were observed in this independent replay. They do
not establish original-capture initialization, defined shader behavior, or original
GPU scheduling. Observing storage does not make an unsupported texture instruction
executable; the executor continues to reject that path explicitly.

The existing lazy replay rejects saved `MipLevels=0` before resource creation.
Reading a canonical native descriptor does not bypass that boundary. A preliminary
probe expecting this path to expand mips failed before reaching observation; its
failure is retained. The final test separately checks the existing rejection and
the supported native backbuffer format normalization. Captured creation descriptor
normalization and complete multisample storage remain separate work.

Identity is scoped to the replay object map. Missing or ambiguous identities fail;
this checkpoint does not add a versioned identity for simultaneously bound original
and experimental storage objects sharing one GPA resource ID.

## Validation scope

The generated fixture suite covers 1D/2D arrays, 3D, cube arrays, BC1 and typeless
storage on WARP, NVIDIA and Intel. Tests compare exact packed bytes and view identity,
exercise alias deduplication under an exact storage budget, scoped input/output
changes, rejection of an insufficient budget and restoration before retry. The
suite also preserves a fractional resource MinLOD and cancels a mapped-subresource
readback after unmapping, checking original storage and a successful retry. The
existing buffer input suite retains its texture-execution rejection assertion while
checking the newly available texture storage separately.

The first full suite passes 76 cases without failures or skips, including 72
parameterized hardware observations. The first smoke failure came from a fixture
shader that did not use its bound resources; the corrected fixture makes those
bindings live. The texture-presence assertion was retained. An intermediate implicit
mip probe failed in the existing replay metadata check, as described above.

The final serial six-suite CTest regression passes 679 cases in 173.01 seconds:
texture inputs (76), typed buffers (473), buffer inputs (42), native analysis (40),
texture initial data (14) and texture edit replay (34). Every suite has zero
failures/skips. Texture initial data was rerun with an explicit file logger because
its CTest stdout log was empty. Five selected generated texture-creation tests plus
setup/cleanup add seven passes; the original-capture portion of that suite was not
rerun here. No skip condition, golden output or existing assertion was weakened.
The rebuilt existing texture-creation tests emit Qt deprecated-assertion warnings;
the new snapshot targets compile without warnings.

Real-frame PS input observations succeed for GF2 event 113 (three textures,
17,472 bytes) and Helldivers event 200 (seven textures, 55,989,568 bytes). BF1 PS
event 876 has no live texture bindings; BF1 CS event 209 observes four textures,
32,768 bytes. All four observations have no unavailable binding entries. These
are independent-replay observations, not full shader execution or performance
benchmarks; capture load, replay and readback are included in elapsed times.

Completed run and observation identities are recorded in
`research/dx11-dxbc-texture-input-evidence.json`. No standalone-release or full
shader execution claim follows from this checkpoint.

## Reproduction

Build `FloraDxbcTextureInputTests`, `FloraDxbcInputSnapshotTests` and
`FloraDxbcInputProbe` in the configured Release build. Run the `dxbc_texture_input`
and `dxbc_input_snapshot` CTest entries serially. The probe takes frame path, GPA
event ID, native stage index, adapter vendor (zero explicitly selects WARP), and
output JSON path. It keeps the default shared 256 MiB backing-storage budget.

Run `tools/summarize_dxbc_texture_inputs.py` with chronological `--log` arguments,
`--regression` logs, `--observation` JSON reports, source/binary `--source` paths and
`--out`. It preserves failed probe runs, requires completed Qt totals, checks each
texture observation's matching PASS record, and rejects failed/skipped final runs.
Original captures, raw logs, binaries and large reports remain outside Git.
