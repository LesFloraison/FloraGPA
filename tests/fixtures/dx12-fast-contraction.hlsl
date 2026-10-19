// Captured Hellblade2 inputs: (1049650790,1052812248) repeated in zw.
// Compare single-use fast, precise, and shared-product chains with GPU stores.
cbuffer Inputs : register(b0) { float4 values; };
RWStructuredBuffer<float4> output : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    float x = values.x, y = values.y, z = values.z, w = values.w;
    output[0] = float4(x, y, 0, 0);
    float angle = y * 543.31f + x;
    output[0].z = angle;
    precise float product = w * 543.31f;
    precise float separated = product + z;
    output[1] = float4(z, w, separated, product);
    float reusedProduct = y * 321.51f;
    output[2] = float4(reusedProduct, reusedProduct + x, 0, 0);
}
