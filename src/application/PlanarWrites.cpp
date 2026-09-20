#include "PlanarWrites.h"
namespace flora {
nlohmann::json planarWriteReport(const Replay &replay) {
    using Json = nlohmann::json;
    auto result = Json::array();
    for (const auto &w : replay.planarWrites()) {
        Json row{{"event_id", w.event},
                 {"resource_id", w.resource},
                 {"subresource", w.subresource},
                 {"format", w.format},
                 {"operation", w.mapType ? "Map" : "UpdateSubresource"}};
        if (w.mapType) {
            row.update({{"written_planes", Json::array({"y"})},
                        {"uv_effect", w.mapType == 4 ? "discarded_unspecified" : "retained"},
                        {"map_type", w.mapType},
                        {"source_row_pitch", w.sourceRowPitch},
                        {"written_row_bytes", w.writtenRowBytes},
                        {"native_row_pitch", w.nativeRowPitch}});
        } else {
            row.update({{"written_planes", Json::array({"y", "uv"})},
                        {"uv_effect", w.explicitSource ? "explicit_source_bytes" : "captured_region_bytes"},
                        {"source_origin", w.explicitSource ? "explicit_experiment" : "capture"},
                        {"row_pitch", w.rowPitch},
                        {"slice_pitch", w.slicePitch},
                        {"box", w.box ? Json(*w.box) : Json(nullptr)}});
        }
        result.push_back(std::move(row));
    }
    return result;
}
std::string planarWriteNotice(const Replay &replay) {
    bool map = false, legacy = false, captured = false, explicitSource = false;
    for (const auto &w : replay.planarWrites()) {
        if (w.mapType) {
            map = true;
            legacy |= w.format == 104 || w.format == 105;
        } else if (w.explicitSource)
            explicitSource = true;
        else
            captured = true;
    }
    std::string result;
    if (map)
        result =
            "Captured planar Map writes Y only. WRITE/READ_WRITE retain UV; DISCARD leaves UV undefined.";
    if (legacy)
        result += " Legacy row padding is removed; 16-bit Y uses the current device pitch.";
    if (captured)
        result += " NV12 Update writes captured Y/UV regions; missing chroma is not reconstructed.";
    if (explicitSource)
        result += " Update writes complete Y/UV from experiment assets, not recovered capture data.";
    if (!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    return result;
}
} // namespace flora
