// BF1 VS instruction 55: dp4 r2.w, r1.xyzw, cb2[17].
// Inputs must be supplied as exact captured float32 bits at runtime.
// Row 0: dot, separate sequential sum, separate reordered sum, mad chain.
// FXC may lower the source-level mad chain to precise separate operations;
// inspect the compiled instructions before interpreting the fourth component.
// Row 1: individually rounded products. Row 2: original input a.
cbuffer Inputs : register(b0) { float4 a; float4 b; };
RWStructuredBuffer<float4> output : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    float nativeDot = dot(a, b);
    precise float4 products = a * b;
    precise float sequential = ((products.x + products.y) + products.z) + products.w;
    precise float reordered = ((products.x + products.z) + products.y) + products.w;
    float chained = mad(a.w, b.w, mad(a.z, b.z, mad(a.y, b.y, a.x * b.x)));
    output[0] = float4(nativeDot, sequential, reordered, chained);
    output[1] = products;
    output[2] = a;
}
