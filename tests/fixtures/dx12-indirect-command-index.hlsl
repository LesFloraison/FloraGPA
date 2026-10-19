RWStructuredBuffer<uint> Values : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // Two indirect dispatches have different group counts. Atomics keep their
    // overlapping word deterministic without inserting a barrier inside EI.
    InterlockedAdd(Values[tid.x], 7);
}
