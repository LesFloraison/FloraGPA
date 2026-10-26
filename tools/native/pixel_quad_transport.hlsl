// Independent raw-bit transport experiment. No original application shader runs.
#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 1
#endif
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 0
#endif
cbuffer Inputs : register(b0) {
    uint4 inputBits[256];
    float4 vertices[3];
};
float4 VSMain(uint id : SV_VertexID) : SV_Position { return vertices[id]; }
struct PixelInput {
    float4 position : SV_Position;
    uint coverage : SV_Coverage;
#if SAMPLE_RATE
    uint sample : SV_SampleIndex;
#endif
};
uint sampleIndex(PixelInput input) {
#if SAMPLE_RATE
    return input.sample;
#else
    return 0;
#endif
}
uint4 sourceBits(PixelInput input) {
    uint2 p = uint2(input.position.xy);
    uint sample = sampleIndex(input);
    return inputBits[p.y * 16 + p.x] ^
        (sample * uint4(0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu));
}
// Every byte and every derivative/intermediate is an exactly represented small
// integer. Never subtract raw floating-point representations of arbitrary bits.
uint4 transport(uint4 value, float2 role, uint target) {
    float2 delta = float2(target & 1u, target >> 1u) - role;
    uint4 result = 0;
    [unroll] for (uint shift = 0; shift < 32; shift += 8) {
        float4 b = float4((value >> shift) & 255u);
        float4 dx = ddx_fine(b);
        float4 dy = ddy_fine(b);
        float4 dxy = ddx_fine(dy);
        float4 neighbor = b + delta.x * dx + delta.y * dy + (delta.x * delta.y) * dxy;
        result |= uint4(neighbor) << shift;
    }
    return result;
}
struct Capture {
    uint4 lane0 : SV_Target0;
    uint4 lane1 : SV_Target1;
    uint4 lane2 : SV_Target2;
    uint4 lane3 : SV_Target3;
    uint4 location : SV_Target4;
    uint4 samples : SV_Target5;
    uint4 coverage : SV_Target6;
    uint4 orientation : SV_Target7;
};
Capture PSMain(PixelInput input) {
    float2 parity = float2(uint2(input.position.xy) & 1u);
    float2 direction = float2(ddx_fine(parity.x), ddy_fine(parity.y)) * (1 - 2 * parity);
    float2 role = float2(direction.x < 0, direction.y < 0);
    uint4 value = sourceBits(input);
    uint4 samples = sampleIndex(input).xxxx;
    uint4 coverage = input.coverage.xxxx;
    Capture output;
    output.lane0 = transport(value, role, 0);
    output.lane1 = transport(value, role, 1);
    output.lane2 = transport(value, role, 2);
    output.lane3 = transport(value, role, 3);
    output.location = uint4(uint2(input.position.xy), uint2(role));
    output.samples = uint4(transport(samples, role, 0).x, transport(samples, role, 1).x,
                           transport(samples, role, 2).x, transport(samples, role, 3).x);
    output.coverage = uint4(transport(coverage, role, 0).x, transport(coverage, role, 1).x,
                            transport(coverage, role, 2).x, transport(coverage, role, 3).x);
    output.orientation = asuint(float4(direction, ddx_fine(parity.y), ddy_fine(parity.x)));
    return output;
}
uint4 ReferenceMain(PixelInput input) : SV_Target { return sourceBits(input); }
#if SAMPLE_COUNT > 1
Texture2DMS<uint4, SAMPLE_COUNT> readbackSource : register(t0);
#else
Texture2D<uint4> readbackSource : register(t0);
#endif
RWStructuredBuffer<uint4> readbackOutput : register(u0);
[numthreads(8, 8, 1)]
void ReadbackMain(uint3 id : SV_DispatchThreadID) {
    [unroll] for (uint sample = 0; sample < SAMPLE_COUNT; ++sample) {
#if SAMPLE_COUNT > 1
        uint4 value = readbackSource.Load(id.xy, sample);
#else
        uint4 value = readbackSource.Load(int3(id.xy, 0));
#endif
        readbackOutput[(id.y * 16 + id.x) * SAMPLE_COUNT + sample] = value;
    }
}
