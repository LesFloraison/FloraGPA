Texture2D<uint4> plane : register(t0);
uint4 main(float4 position : SV_Position) : SV_Target {
    return plane.Load(int3(uint2(position.xy), 0));
}
