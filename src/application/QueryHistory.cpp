#include "ApiCommands.h"
#include <QByteArray>
#include <set>

namespace flora {
using Json = nlohmann::json;
namespace {
const std::vector<std::string> names{"EVENT",
                                     "OCCLUSION",
                                     "TIMESTAMP",
                                     "TIMESTAMP_DISJOINT",
                                     "PIPELINE_STATISTICS",
                                     "OCCLUSION_PREDICATE",
                                     "SO_STATISTICS",
                                     "SO_OVERFLOW_PREDICATE",
                                     "SO_STATISTICS_STREAM0",
                                     "SO_OVERFLOW_PREDICATE_STREAM0",
                                     "SO_STATISTICS_STREAM1",
                                     "SO_OVERFLOW_PREDICATE_STREAM1",
                                     "SO_STATISTICS_STREAM2",
                                     "SO_OVERFLOW_PREDICATE_STREAM2",
                                     "SO_STATISTICS_STREAM3",
                                     "SO_OVERFLOW_PREDICATE_STREAM3"};
struct Member {
    std::string name, type;
    uint32_t offset, size;
};
std::pair<uint32_t, std::vector<Member>> layout(uint32_t kind) {
    if (kind == 0 || kind == 5 || kind == 7 || kind == 9 || kind == 11 || kind == 13 || kind == 15)
        return {4, {{kind == 0 ? "finished" : "predicate", "BOOL", 0, 4}}};
    if (kind == 1 || kind == 2)
        return {8, {{kind == 1 ? "samples" : "timestamp", "UINT64", 0, 8}}};
    if (kind == 3)
        return {16, {{"Frequency", "UINT64", 0, 8}, {"Disjoint", "BOOL", 8, 4}}};
    if (kind == 4) {
        std::vector<Member> result;
        for (const char *name :
             {"IAVertices", "IAPrimitives", "VSInvocations", "GSInvocations", "GSPrimitives", "CInvocations",
              "CPrimitives", "PSInvocations", "HSInvocations", "DSInvocations", "CSInvocations"})
            result.push_back({name, "UINT64", uint32_t(result.size() * 8), 8});
        return {88, result};
    }
    if (kind == 6 || kind == 8 || kind == 10 || kind == 12 || kind == 14)
        return {16, {{"NumPrimitivesWritten", "UINT64", 0, 8}, {"PrimitivesStorageNeeded", "UINT64", 8, 8}}};
    return {};
}
struct Query {
    Json descriptors = Json::array(), sizes = Json::array(), invalid = Json::array();
};
Json interpret(const Json &row, const Json &fields, const Query *state) {
    Json out{{"source", "captured_GetData"},
             {"object", fields.value("query", Json(nullptr))},
             {"status", "unavailable"},
             {"query_type", nullptr},
             {"query_name", nullptr},
             {"expected_bytes", nullptr},
             {"requested_bytes", fields.value("data_size", Json(nullptr))},
             {"captured_hex", ""},
             {"typed_result_complete", false},
             {"replayed_query", false},
             {"fields", Json::array()},
             {"metadata", Json::array()},
             {"issues", Json::array()}};
    auto status = [&](const char *value) {
        out["status"] = value;
        return out;
    };
    if (row["status"] != "decoded")
        return status("invalid_record");
    if (fields.value("captured_data_word_present", 0)) {
        auto word = fields.at("captured_data_word[0]").get<uint32_t>();
        out["captured_hex"] = QByteArray(reinterpret_cast<const char *>(&word), 4).toHex().toStdString();
    }
    auto hr = fields.at("hresult").get<int32_t>();
    out["hresult"] = hr;
    if (state) {
        out["metadata"] = state->descriptors;
        for (auto size : state->sizes) {
            size["source"] = "same_id_GetDataSize";
            out["metadata"].push_back(size);
        }
        out["issues"] = state->invalid;
        std::set<std::pair<uint32_t, uint32_t>> descriptors;
        std::set<uint32_t> sizes;
        for (auto &d : state->descriptors)
            descriptors.emplace(d.at("query_type"), d.at("misc_flags"));
        for (auto &s : state->sizes)
            sizes.insert(s.at("size").get<uint32_t>());
        if (descriptors.size() > 1)
            out["issues"].push_back("Conflicting descriptors for the same object ID");
        if (sizes.size() > 1)
            out["issues"].push_back("Conflicting GetDataSize results for the same object ID");
        if (descriptors.size() == 1) {
            auto [kind, flags] = *descriptors.begin();
            auto [size, members] = layout(kind);
            out.update({{"query_type", kind},
                        {"query_name", kind < names.size() ? names[kind] : "UNKNOWN"},
                        {"misc_flags", flags},
                        {"expected_bytes", size ? Json(size) : Json(nullptr)}});
            if ((flags & ~1u) || (flags && kind != 5))
                out["issues"].push_back("Unsupported query misc flags");
            if (size && !sizes.empty() && sizes != std::set<uint32_t>{size})
                out["issues"].push_back("GetDataSize disagrees with the query type");
        }
    }
    if (hr)
        return status(hr == 1 ? "not_ready" : hr < 0 ? "failed" : "unexpected_hresult");
    if (fields.at("data_size") == 0)
        return status("status_only");
    if (!state || fields.at("query") == 0)
        return status("unresolved_identity");
    if (!out["issues"].empty())
        return status("metadata_conflict");
    if (out["query_type"].is_null())
        return status("unknown_type");
    if (out["expected_bytes"].is_null())
        return status("unsupported_type");
    if (fields.at("data_size") != out["expected_bytes"])
        return status("size_mismatch");
    if (out["misc_flags"].get<uint32_t>() & 1)
        return status("predicate_hint");
    auto raw = QByteArray::fromHex(QByteArray::fromStdString(out["captured_hex"]));
    if (raw.isEmpty())
        return status("missing_bytes");
    auto [size, members] = layout(out["query_type"]);
    (void)size;
    bool complete = true;
    for (auto &m : members) {
        auto part = raw.mid(m.offset, m.size);
        Json value{{"name", m.name},
                   {"type", m.type},
                   {"offset", m.offset},
                   {"expected_bytes", m.size},
                   {"captured_bytes", part.size()},
                   {"captured_hex", part.toHex().toStdString()},
                   {"value", nullptr},
                   {"status", "missing"}};
        uint64_t number = 0;
        for (qsizetype i = 0; i < part.size(); ++i)
            number |= uint64_t(uint8_t(part[i])) << (8 * i);
        if (part.size() == m.size) {
            value.update({{"status", "complete"},
                          {"value", m.type == "BOOL" ? Json(number != 0) : Json(number)},
                          {"raw_integer", number}});
        } else {
            complete = false;
            if (!part.isEmpty())
                value.update({{"status", "partial"},
                              {"known_low_bits", part.size() * 8},
                              {"known_low_value", number}});
        }
        out["fields"].push_back(value);
    }
    out["typed_result_complete"] = complete;
    return status(complete ? "complete" : "partial");
}
} // namespace
void attachQueryHistory(const Frame &frame, Json &rows) {
    std::map<Id, Query> states;
    auto state = [&](Id id) -> Query & {
        auto [it, inserted] = states.try_emplace(id);
        auto &q = it->second;
        if (inserted) {
            auto found = frame.entries().find(id);
            if (found != frame.entries().end()) {
                if (found->second.category == 5 && found->second.type == 0x96) {
                    try {
                        Reader r(frame.payload(id));
                        r.skip(16);
                        auto kind = r.read<uint32_t>(), flags = r.read<uint32_t>();
                        r.end();
                        if ((kind != 5 && kind != 7) || (flags & ~1u) || (flags && kind != 5))
                            throw std::runtime_error("Invalid predicate");
                        q.descriptors.push_back({{"event", nullptr},
                                                 {"source", "predicate_resource"},
                                                 {"query_type", kind},
                                                 {"misc_flags", flags}});
                    } catch (const std::exception &) {
                        q.invalid.push_back("Invalid predicate resource");
                    }
                } else
                    q.invalid.push_back("Object ID resolves to a non-query resource or record");
            }
        }
        return q;
    };
    for (auto &row : rows) {
        Json fields = Json::object();
        for (auto &f : row["fields"])
            fields[f["name"].get<std::string>()] = f["value"];
        auto kind = row["type"].get<uint16_t>();
        auto owner = fields.value("object", Id(0));
        if ((kind == 0x3151 || kind == 0x3152) && owner) {
            auto &q = state(owner);
            if (row["status"] != "decoded") {
                q.invalid.push_back("Malformed query getter at " + row["id"].dump());
                continue;
            }
            if (kind == 0x3151)
                q.sizes.push_back({{"event", row["id"]}, {"size", fields["return_data_size"]}});
            else if (fields["descriptor_present"] != 0)
                q.descriptors.push_back({{"event", row["id"]},
                                         {"source", "same_id_GetDesc"},
                                         {"query_type", fields["query_type"]},
                                         {"misc_flags", fields["misc_flags"]}});
        } else if ((kind == 0x3074 || kind == 0x3235 || kind == 0x33a8 || kind == 0x3471 || kind == 0x34b2 ||
                    kind == 0x358d) &&
                   row["status"] == "decoded" && fields["hresult"] == 0 && fields["returned_query"] != 0 &&
                   fields["descriptor_present"] != 0)
            state(fields["returned_query"])
                .descriptors.push_back({{"event", row["id"]},
                                        {"source", "explicit_CreateQuery_return"},
                                        {"query_type", fields["query_type"]},
                                        {"misc_flags", fields["misc_flags"]}});
        else if (kind == 0x30b4 || kind == 0x31b4 || kind == 0x331d || kind == 0x33e3 || kind == 0x34fb)
            row["query_result"] =
                interpret(row, fields, fields.value("query", Id(0)) ? &state(fields["query"]) : nullptr);
    }
}
} // namespace flora
