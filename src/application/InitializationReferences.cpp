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
    {0x25e, {"0x23c30", {"object"}}},
};
} // namespace

bool originalErgType(uint16_t type) {
    return (type >= 1 && type <= 0xfe) || (type >= 0x201 && type <= 0x2fe);
}

InitializationCache initialFileCache(const Frame &frame) {
    InitializationCache out;
    auto reject = [&](const std::string &reason) {
        out.complete = false;
        if (out.reason.empty())
            out.reason = reason;
    };
    if (frame.hasEditedViews())
        reject("Edited version cache transitions are not recovered");
    for (const auto &[id, entry] : frame.entries()) {
        if (id > UINT32_MAX || entry.flags ||
            (entry.category != 3 && entry.category != 5 && entry.category != 7 && entry.category != 9)) {
            reject("Entry " + std::to_string(id) + " is outside the verified initial cache index profile");
            continue;
        }
        const uint32_t kind = entry.category / 2;
        out.categories.emplace(uint32_t(id), kind);
        if (kind != 3 || originalErgType(entry.type))
            out.descriptors[kind].insert(uint32_t(id));
    }
    return out;
}

bool InitializationCache::contains(uint32_t id) const {
    const auto category = categories.find(id);
    if (!complete || category == categories.end() || category->second < 1 || category->second > 4)
        return false;
    const auto descriptor = descriptors.find(category->second);
    return descriptor != descriptors.end() && descriptor->second.contains(id);
}

Json InitializationCache::report() const {
    Json out{{"scope", "unmodified_initial_file_registration"},
             {"version", 0},
             {"resource_objects_validated", false},
             {"status", complete ? "recovered" : "unrecovered"}};
    if (!complete) {
        out["reason"] = reason;
        return out;
    }
    out["categories"] = Json::array();
    for (const auto &[id, kind] : categories)
        out["categories"].push_back({{"id", id}, {"kind", kind}});
    for (const auto &[kind, name] :
         std::map<uint32_t, const char *>{{1, "state"}, {2, "resource"}, {3, "erg"}, {4, "data"}}) {
        const auto found = descriptors.find(kind);
        out["descriptors"][name] = found == descriptors.end() ? Json::array() : Json(found->second);
    }
    return out;
}

Json inspectInitializationReferences(const Json &command, const InitializationCache *cache) {
    Json out{{"scope", "original_erg_initialization"},
             {"profile", "GPA_2025_R1_dx11_player_39061ff3"},
             {"execution_dependencies_complete", false},
             {"registration_sequence_available", false}};
    auto found = rules.find(command.at("type").get<uint16_t>());
    if (found == rules.end()) {
        out["status"] = "unrecovered";
        out["reason"] = "No verified collector rule for this wire type";
        return out;
    }
    if (command.value("status", "") != "decoded") {
        out["status"] = "invalid_record";
        return out;
    }
    const bool conditional = command.at("type") == 0x25e;
    if (conditional && (!cache || !cache->complete)) {
        out.update({{"status", "unrecovered"},
                    {"reason", cache ? cache->reason : "Initial cache membership is unavailable"}});
        return out;
    }
    try {
        std::map<std::string, uint64_t> fields;
        for (const auto &field : command.at("fields"))
            if (field.at("encoding") == "Q" || field.at("encoding") == "I" || field.at("encoding") == "B")
                fields.emplace(field.at("name").get<std::string>(), field.at("value").get<uint64_t>());
        Json references = Json::array(), truncated = Json::array(), sequence = Json::array();
        std::set<uint32_t> dependencies;
        auto names = found->second.fields;
        Json membership = Json::array();
        if (conditional && fields.at("uavs_present")) {
            const auto count = fields.at("count");
            if (count > 65536)
                throw std::runtime_error("CSUAV reference count is too large");
            for (uint64_t i = 0; i < count; ++i) {
                const auto name = "uavs[" + std::to_string(i) + "]";
                const auto raw = fields.at(name);
                const auto original = uint32_t(raw);
                const bool included = original && cache->contains(original);
                membership.push_back({{"field", name},
                                      {"captured_id", raw},
                                      {"original_id", original},
                                      {"included", included}});
                if (included)
                    names.push_back(name);
            }
        }
        for (const auto &name : names) {
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
        if (conditional) {
            out["cache_scope"] = "unmodified_initial_file_registration";
            out["membership_checks"] = membership;
        }
    } catch (const std::exception &error) {
        out.update({{"status", "invalid_record"}, {"reason", error.what()}});
    }
    return out;
}
} // namespace flora
