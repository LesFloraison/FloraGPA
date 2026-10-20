// Controlled replacement for Hellblade2 execution 10267's HWRasterize MS.
// Keep the captured PS signature. Only group zero emits geometry; its one
// full-viewport triangle makes the captured PS write (135, asuint(0.5)).
// This is an experiment oracle, not reconstructed game geometry.
struct Vertex {
    float4 projected : TEXCOORD0;
    nointerpolation uint3 unused : TEXCOORD1;
    nointerpolation int4 rectangle : TEXCOORD2;
    float4 position : SV_Position;
};
struct Primitive {
    nointerpolation uint4 identity : TEXCOORD7;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(uint3 group : SV_GroupID,
          out vertices Vertex vertices[3],
          out indices uint3 triangles[1],
          out primitives Primitive primitives[1]) {
    bool emit = all(group == 0);
    SetMeshOutputCounts(emit ? 3 : 0, emit ? 1 : 0);
    if (emit) {
        float2 clip[3] = {float2(-1, 1), float2(3, 1), float2(-1, -3)};
        float2 pixels[3] = {float2(0, 0), float2(512, 0), float2(0, 512)};
        for (uint i = 0; i < 3; ++i) {
            vertices[i].position = float4(clip[i], 0.5, 1);
            vertices[i].projected = float4(pixels[i], 0.5, 1);
            vertices[i].unused = 0;
            vertices[i].rectangle = int4(0, 0, 256, 256);
        }
        // The captured rasterizer uses counterclockwise front faces.
        triangles[0] = uint3(0, 2, 1);
        primitives[0].identity = uint4(0, 7, 0, 0);
    }
}
