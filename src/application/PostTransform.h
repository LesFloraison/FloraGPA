#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct PostTransformOptions {
    std::string stage = "final";
    uint32_t stream = 0;
    uint64_t maxBytes = 256ull * 1024 * 1024;
    std::optional<uint32_t> instance;
    bool hullDownstream = false;
};
struct PostTransformGeometry {
    nlohmann::json report;
    std::vector<uint8_t> bytes;
    struct Identity {
        uint32_t instance;
        int64_t vertexIndex;
        uint32_t vertexId;
    };
    std::vector<Identity> identities;
    std::vector<uint8_t> downstreamBytes;
};
// The replay must be positioned before this event, with its snapshot prepared.
PostTransformGeometry inspectPostTransform(Replay &replay, Id event,
                                           const PostTransformOptions &options = {});
// Internal diagnostics: caller owns the selected event's bound input/edit scope.
// Returns the raw final-stage capture report without export/presentation fields.
PostTransformGeometry captureBoundPostTransform(Replay &replay, const Event &event, const State &state,
                                                uint32_t stream = 0,
                                                uint64_t maxBytes = 256ull * 1024 * 1024);
nlohmann::json postTransformLayout(Bytes bytecode, uint32_t stream);
void exportPostTransform(const PostTransformGeometry &geometry, const std::filesystem::path &directory);
nlohmann::json postTransformTables(const PostTransformGeometry &geometry);
} // namespace flora
