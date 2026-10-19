RWStructuredBuffer<uint> Values : register(u0);
StructuredBuffer<uint> Input : register(t0);
[numthreads(1, 1, 1)]
void main() {
    Values[0] = Values[0] + 7;
    Values[1] = Input[0];
}
