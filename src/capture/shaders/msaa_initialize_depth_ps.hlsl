Texture2D<uint2> plane : register(t0);
float main(float4 position : SV_Position) : SV_Depth {
    return asfloat(plane.Load(int3(uint2(position.xy), 0)).x);
}
