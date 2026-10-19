// Target: cs_6_6, entry: main. Hellblade2 execution 36779 replacement fixture.
// Keep the captured TileListsBuffer addressing and the 16x16 Morton lane layout.
// Only u0 changes; the original root signature and captured inputs remain in use.
StructuredBuffer<uint> TileListsBuffer : register(t0);
RWTexture2D<float4> SceneColorOutputMip0 : register(u0);
cbuffer Parameters : register(b0) {
    int2 ViewportMin : packoffset(c2.x);
    int2 ViewportMax : packoffset(c2.z);
    uint TileListOffset : packoffset(c30.z);
};

[numthreads(16, 16, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint tile = TileListsBuffer[TileListOffset + group.x];
    const uint x = (lane & 1) | ((lane >> 1) & 2) | ((lane >> 2) & 4) | ((lane >> 3) & 8);
    const uint y = ((lane >> 1) & 1) | ((lane >> 2) & 2) | ((lane >> 3) & 4) | ((lane >> 4) & 8);
    const int2 pixel = ViewportMin + int2(((tile & 0xffff) << 4) | x, (tile >> 12 & 0xffff0) | y);
    if (all(pixel >= ViewportMin) && all(pixel < ViewportMax))
        SceneColorOutputMip0[pixel] = float4(1, 0, 1, 0);
}
