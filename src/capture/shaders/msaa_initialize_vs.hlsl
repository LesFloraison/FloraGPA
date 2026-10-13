float4 main(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1, 0, 1);
}
