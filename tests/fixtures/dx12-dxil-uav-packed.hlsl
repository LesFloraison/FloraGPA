// Uses the UAV-view fixture harness with optional format r11g11b10.
// First store uses the actual Hellblade2 selected CS operands. The second sits
// just below representable values, exposing an intermediate float16 rounding.
globallycoherent RWTexture2DArray<float4> image : register(u0);
RWStructuredBuffer<uint> results : register(u1);
#define STORE_READ(I, R, G, B) \
    image[uint3(200 + I, 1, 1)] = asfloat(uint4(R, G, B, 0)); \
    DeviceMemoryBarrier(); \
    { float4 v = image[uint3(200 + I, 1, 1)]; \
      results[I * 4] = asuint(v.x); results[I * 4 + 1] = asuint(v.y); \
      results[I * 4 + 2] = asuint(v.z); results[I * 4 + 3] = asuint(v.w); }
[numthreads(1, 1, 1)]
void main()
{
    image[uint3(200, 1, 1)] = float4(0.043701171875, 0.0369873046875, 0.0390625, 0);
    DeviceMemoryBarrier();
    float4 first = image[uint3(200, 1, 1)];
    results[0] = asuint(first.x);
    results[1] = asuint(first.y);
    results[2] = asuint(first.z);
    results[3] = asuint(first.w);
    image[uint3(201, 1, 1)] = asfloat(uint4(0x3f81ffff, 0x3fffffff, 0x3f83ffff, 0));
    DeviceMemoryBarrier();
    float4 second = image[uint3(201, 1, 1)];
    results[4] = asuint(second.x);
    results[5] = asuint(second.y);
    results[6] = asuint(second.z);
    results[7] = asuint(second.w);
    STORE_READ(2, 0x47800000, 0x7f7fffff, 0x477fffff); // Finite overflow clamps to max finite.
    STORE_READ(3, 0xbf800000, 0xff800000, 0x7f800000); // Negatives clamp, +INF survives.
    STORE_READ(4, 0x35800000, 0x35ffffff, 0x36000000); // Packed subnormal boundaries.
    STORE_READ(5, 0x38800000, 0x387fffff, 0x35ffffff); // Smallest normal and just below.
    STORE_READ(6, 0x7fc01234, 0xffc01234, 0x7f800001); // NaN classification, not payload.
    STORE_READ(7, 0x00000000, 0x80000000, 0x007fffff); // Signed zero and tiny input.
}
