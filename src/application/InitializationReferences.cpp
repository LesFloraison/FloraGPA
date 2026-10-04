#include "InitializationReferences.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace flora {
using Json = nlohmann::json;
namespace {
struct Rule {
    const char *collector;
    std::vector<std::string> fields;
};
const std::map<uint16_t, Rule> rules{
    {0x31, {"0x23da0", {"object", "view"}}},
    {0x32, {"0x23da0", {"object", "view"}}},
    {0x33, {"0x23da0", {"object", "view"}}},
    {0x34, {"0x23da0", {"object", "view"}}},
    {0x35, {"0x241e0", {"context"}}},
    {0x36, {"0x241e0", {"context"}}},
    {0x37, {"0x241e0", {"context"}}},
    {0x39, {"0x241e0", {"context"}}},
    {0x3a, {"0x241e0", {"context"}}},
    {0x3c, {"0x241e0", {"context"}}},
    {0x3e, {"0x23e20", {"object", "destination", "source"}}},
    {0x3f, {"0x23ec0", {"object", "destination", "source_uav"}}},
    {0x40, {"0x23f60", {"object", "destination", "source"}}},
    {0x242, {"0x23be0", {"object"}}},
    {0x245, {"0x23da0", {"object", "view"}}},
    {0x246, {"0x24160", {"object", "data"}}},
    {0x247, {"0x24400", {"object", "destination", "data"}}},
    {0x249, {"0x23be0", {"object"}}},
    {0x24a, {"0x23be0", {"object"}}},
    {0x24b, {"0x23be0", {"object"}}},
    {0x24d, {"0x23be0", {"object"}}},
    {0x24e, {"0x23be0", {"object"}}},
    {0x24f, {"0x23be0", {"object"}}},
    {0x253, {"0x23be0", {"object"}}},
    {0x254, {"0x23be0", {"object"}}},
};
} // namespace

Json inspectInitializationReferences(const Json &command) {
    Json out{{"scope", "original_erg_initialization"},
             {"profile", "GPA_2025_R1_dx11_player_39061ff3"},
             {"execution_dependencies_complete", false},
             {"registration_sequence_available", false}};
    auto found = rules.find(command.at("type").get<uint16_t>());
    if (found == rules.end()) {
        out["status"] = "unrecovered";
        out["reason"] = command.at("type") == 0x25e
                            ? "Collector requires the original initialization cache membership"
                            : "No verified collector rule for this wire type";
        return out;
    }
    if (command.value("status", "") != "decoded") {
        out["status"] = "invalid_record";
        return out;
    }
    try {
        std::map<std::string, uint64_t> fields;
        for (const auto &field : command.at("fields"))
            if (field.at("encoding") == "Q")
                fields.emplace(field.at("name").get<std::string>(), field.at("value").get<uint64_t>());
        Json references = Json::array(), truncated = Json::array(), sequence = Json::array();
        std::set<uint32_t> dependencies;
        for (const auto &name : found->second.fields) {
            const auto raw = fields.at(name);
            // Map's collector tests the full saved uint64 before appending its
            // low word. All other fixed fields are appended even when zero.
            if (command.at("type") == 0x246 && name == "data" && !raw)
                continue;
            const auto original = uint32_t(raw);
            references.push_back({{"field", name}, {"captured_id", raw}, {"original_id", original}});
            sequence.push_back(original);
            dependencies.insert(original);
            if (raw != original)
                truncated.push_back(name);
        }
        out.update({{"status", "recovered"},
                    {"collector_rva", found->second.collector},
                    {"references", references},
                    {"collector_sequence", sequence},
                    {"dependency_set", dependencies},
                    {"truncated_fields", truncated}});
    } catch (const std::exception &error) {
        out.update({{"status", "invalid_record"}, {"reason", error.what()}});
    }
    return out;
}
} // namespace flora
