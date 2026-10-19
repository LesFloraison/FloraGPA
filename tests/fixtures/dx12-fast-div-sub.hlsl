// Hellblade2 execution 36222 PS depth-motion operands, supplied as root constants:
// 0x3f7fffff, 0x43e48853, 0x3f7fffff, 0x43e33824.
// Compare compiler-permitted fast division/subtraction with precise separation.
// Supply the same four words twice. Separate loads keep the precise controls
// from propagating no-contraction requirements into the fast expression.
cbuffer Inputs : register(b0) { float4 values; float4 preciseValues; };
RWStructuredBuffer<float4> output : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    output[0] = values;
    float fastDifference = values.x / values.y - values.z / values.w;
    precise float left = preciseValues.x / preciseValues.y;
    precise float right = preciseValues.z / preciseValues.w;
    precise float separated = left - right;
    float explicitFused = mad(values.x, rcp(values.y), -right);
    output[1] = float4(fastDifference, separated, explicitFused, 0);
    output[2] = float4(left, right, rcp(values.y), rcp(values.w));
}
