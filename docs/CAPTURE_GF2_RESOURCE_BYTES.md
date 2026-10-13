# Selected GF2 resource-byte comparison

The first three Intel capture candidates now pass **33 bound-buffer comparisons
(24 unique buffers) and three BC1 texture comparisons**, all with exact byte
equality between actual original-GPA execution resources and Flora event-input
exports. The six original executions used for these snapshots still reproduce
the captured final RGBA exactly. Formal six-frame acceptance remains pending.

The comparison targets the selected DrawIndexed events already recorded in
`capture-gf2-intel-repeat-baseline.json`: Flora events 2288, 5034 and 5040. The
original process observes the corresponding 73rd/80th/80th DrawIndexed, checks
its arguments (61155/118158/118158 indices, zero offsets), and reads its real
context bindings before forwarding the draw. All audited contexts identify Intel.

Public D3D11 getters provide VS/PS constant buffers, the first three vertex
buffers and the index buffer. Staging CopyResource/Map reads whole byte ranges;
COM references are released afterward. The experiment requires absent predication
and does not rebind application resources. Flora exports the resource identified
by its native pipeline getter at the same event's input boundary. Duplicate
VS/PS bindings are compared separately but exported once per unique buffer.

For texture inputs, the scoped experiment selects single-sample BC1-5 Texture2D
resources bound as pixel SRVs. These three draws each bind one matching BC1 sRGB
texture: 2048x2048, one layer and twelve mips. The readback copies each mapped
block row without pitch padding, in layer/mip order. Each complete 2,796,216-byte
chain matches Flora's native `texture-storage` export exactly. The captured view
record supplies the underlying texture ID for the comparison.

These are selected input-resource checks, not a proof of all intermediate GPU
resources or hidden state. Both players consume captured records; their agreement
does not independently prove every original application resource was captured
correctly. No additional MSAA format or restoration support is claimed. Existing
deterministic application-oracle tests retain their separate scope.

All GPU execution ran serially with GF2 and the original GUI closed. The original
adapter remains a hash-guarded development process using the pinned GPA build;
production capture/replay/format code and dependencies did not change. No new
CTest or golden run is claimed for this evidence-only stage.

The [baseline](capture-gf2-resource-bytes-baseline.json) pins exact sizes/hashes,
bindings and the 171-file evidence manifest under the ignored directory
`artifacts/capture-gf2-resource-bytes-20261014`. The original workspace and
previous captures are unchanged.
