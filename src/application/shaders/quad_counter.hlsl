RWTexture2D<uint> lock_uav : register(u0);
RWTexture2D<uint> overdraw_uav : register(u1);
RWTexture2D<uint> live_count_uav : register(u2);
RWTexture1D<uint> live_stats_uav : register(u3);
[earlydepthstencil]
void quadOverdrawCounterPS (float4 vpos : SV_Position, uint id : SV_PrimitiveID)
{
    bool processed = false;
    int lock_count = 0;
    int pixel_count = 0;
    uint unlocked_id = 0xffffffff;
    uint prev_id = 0;
    uint2 quad = vpos.xy*0.5;
    for (int i = 0; i < 64; i++)
    {
        if (!processed)
            InterlockedCompareExchange (lock_uav [quad], unlocked_id, id, prev_id);
        [branch]
        if (prev_id == unlocked_id)
        {
            if (++lock_count == 4)
            {
                InterlockedAnd (live_count_uav[quad], 0, pixel_count);
                InterlockedExchange (lock_uav[quad], unlocked_id, prev_id);
            }
            processed = true;
        }
        if (prev_id == id && !processed)
        {
            InterlockedAdd (live_count_uav[quad], 1);
            processed = true;
        }
    }
    if (lock_count)
    {
        InterlockedAdd (overdraw_uav[quad], 1);
        InterlockedAdd (live_stats_uav[pixel_count], 1);
    }
}
float4 mainVS (float4 pos : POSITION) : SV_Position
{
    return pos;
}
Texture2D<uint> overdraw_srv : register(t0);
float4 mainPS(float4 vpos : SV_POSITION) : SV_Target
{
    const uint MAX_COLOR_SIZE = 10;
    uint2 quad = vpos.xy*0.5;
    uint overdraw_count = overdraw_srv [quad];
    float converted_value = (float)overdraw_count / 255.0;
    float4 color = float4 (0, 0, 0, converted_value);
    return color;
}
