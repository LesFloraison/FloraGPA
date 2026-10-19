// Hellblade2 MainPS arithmetic reproduction, SM6.6. Supply identical captured
// input bits (1049650790,1052812248) in both pairs. Separate loads prevent DXC
// common-subexpression elimination from propagating 'precise' to both paths.
// Output rows: fast angle/sine/product/hash; precise equivalents; two dithers/x/y.
cbuffer Inputs : register(b0) { float x; float y; float splitX; float splitY; };
RWStructuredBuffer<float4> output : register(u0);
float4 hashStages(float angle)
{
    float s = sin(angle);
    float product = s * 493013.0f;
    return float4(angle, s, product, frac(product));
}
float dither(float hash)
{
    float v = hash * 2.0f - 1.0f;
    float signValue = clamp(v * asfloat(0x7f800000), -1.0f, 1.0f);
    return (signValue - sqrt(saturate(1.0f - abs(v))) * signValue) * (1.0f / 1023.0f);
}
[numthreads(1, 1, 1)]
void main()
{
    float angle = y * 543.31f + x;
    precise float product = splitY * 543.31f;
    precise float separated = product + splitX;
    float4 fused = hashStages(angle);
    float4 split = hashStages(separated);
    output[0] = fused;
    output[1] = split;
    output[2] = float4(dither(fused.w), dither(split.w), x, y);
}
