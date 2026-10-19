// BF1 VS instructions 267/269 use MAD with these inputs supplied at runtime.
// No precise dependency: retain a DXBC mad and inspect the compiled assembly.
// Compare its raw output with separate CPU multiply/add and captured VS output.
// a bits: [3180323104,1009877347,3187663360,1015251207]
// b bits: [1082133053,1082133053,1082133053,1082133053]
// c bits: [1056964608,1056964608,1056964608,1056964608]
// Observed GPU bits: [1046488764,1057691897,949519973,1058042225].
cbuffer Inputs : register(b0) { float4 a; float4 b; float4 c; };
RWStructuredBuffer<float4> output : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    output[0] = mad(a, b, c);
    output[1] = a;
    output[2] = b;
}
