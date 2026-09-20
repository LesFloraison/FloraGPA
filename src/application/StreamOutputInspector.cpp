#include "StreamOutputInspector.h"
namespace flora {
using Json = nlohmann::json;
Json streamOutputJson(const StreamOutputDeclaration &decl) {
    Json entries = Json::array();
    for (const auto &e : decl.entries)
        entries.push_back({{"stream", e.stream},
                           {"semantic", e.semantic ? Json(*e.semantic) : Json(nullptr)},
                           {"semantic_id", e.semanticId},
                           {"index", e.index},
                           {"start_component", e.start},
                           {"component_count", e.count},
                           {"output_slot", e.slot}});
    return {{"id", decl.id},
            {"entries", entries},
            {"strides", decl.strides},
            {"rasterized_stream", decl.rasterizedStream}};
}
Json drawAutoJson(const DrawAutoParameters &p) {
    Json out{{"source",
              p.verified ? "recomputed_stream_output_vertex_count" : "captured_stream_output_vertex_count"},
             {"stream", p.stream},
             {"geometry_shader", p.shader},
             {"stream_output_declaration", p.declaration},
             {"history_verified", p.verified}};
    if (p.verified) {
        out["resource"] = p.resource;
        out["captured_vertex_count"] = p.capturedCount;
        if (p.filledBytes) {
            out["filled_bytes"] = *p.filledBytes;
            out["ia_offset"] = p.iaOffset;
            out["ia_stride"] = p.iaStride;
        }
    } else {
        out["history_note"] =
            "No known in-frame SO cursor; using captured count without native verification.";
        if (!p.shader || p.noRasterization)
            out["original_player_vertex_count"] = 0;
        if (p.noRasterization)
            out["rasterized_stream"] = UINT32_MAX;
    }
    return out;
}
} // namespace flora
