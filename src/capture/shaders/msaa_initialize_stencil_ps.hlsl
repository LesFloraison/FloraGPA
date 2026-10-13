Texture2D<uint2> plane : register(t0);
cbuffer Values : register(b0) { uint bit; };
void main(float4 position : SV_Position) {
    if ((plane.Load(int3(uint2(position.xy), 0)).y & bit) == 0)
        discard;
}
