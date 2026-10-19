// A 512x4, three-layer R32_UINT texture, two mips. UAV: mip 1, layers 1..2.
// Initial value: 10000*physicalLayer + 1000*mip + 100*y + x.
#if defined(VIEW_1D)
RWTexture1DArray<uint> image : register(u0);
#define POSITION(x, y, z) uint2(x, z)
#elif defined(VIEW_3D)
RWTexture3D<uint> image : register(u0);
#define POSITION(x, y, z) uint3(x, y, z)
#else
RWTexture2DArray<uint> image : register(u0);
#define POSITION(x, y, z) uint3(x, y, z)
#endif
RWStructuredBuffer<uint> results : register(u1);
[numthreads(1, 1, 1)]
void main()
{
    results[0] = image[POSITION(200, 1, 1)];
    image[POSITION(200, 1, 1)] = 777;
    results[1] = image[POSITION(200, 1, 1)];
    uint previous;
    InterlockedAdd(image[POSITION(200, 1, 1)], 7, previous);
    results[2] = previous;
    results[3] = image[POSITION(200, 1, 1)];
    image[POSITION(256, 0, 1)] = 999; // x outside the view must not alias the next row.
    image[POSITION(-1, 1, 1)] = 999;
    results[4] = image[POSITION(0, 1, 1)];
    results[5] = image[POSITION(256, 0, 1)];
    results[6] = image[POSITION(-1, 1, 1)];
    results[7] = image[POSITION(128, 1, 1)];
}
