#include "ViewEdits.h"
#include "SrvEdits.h"
#include <algorithm>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
using Fields = std::map<unsigned, std::vector<std::string>>;
const Fields &fields(const std::string &kind) {
    static const Fields rtv{{1, {"first_element", "num_elements"}},
                            {2, {"mip_slice"}},
                            {3, {"mip_slice", "first_array_slice", "array_size"}},
                            {4, {"mip_slice"}},
                            {5, {"mip_slice", "first_array_slice", "array_size"}},
                            {6, {}},
                            {7, {"first_array_slice", "array_size"}},
                            {8, {"mip_slice", "first_w_slice", "w_size"}}};
    static const Fields dsv{{1, {"mip_slice"}},
                            {2, {"mip_slice", "first_array_slice", "array_size"}},
                            {3, {"mip_slice"}},
                            {4, {"mip_slice", "first_array_slice", "array_size"}},
                            {5, {}},
                            {6, {"first_array_slice", "array_size"}}};
    static const Fields uav{
        {1, {"first_element", "num_elements", "flags"}},       {2, {"mip_slice"}},
        {3, {"mip_slice", "first_array_slice", "array_size"}}, {4, {"mip_slice"}},
        {5, {"mip_slice", "first_array_slice", "array_size"}}, {8, {"mip_slice", "first_w_slice", "w_size"}}};
    static const Fields srv = [] {
        Fields result;
        for (unsigned i = 1; i <= 11; ++i)
            result[i] = srvFields(i);
        return result;
    }();
    if (kind == "srv")
        return srv;
    if (kind == "rtv")
        return rtv;
    if (kind == "dsv")
        return dsv;
    if (kind == "uav")
        return uav;
    throw std::runtime_error("Unsupported view kind");
}
size_t descriptorSize(const std::string &kind) {
    fields(kind);
    return kind == "srv" || kind == "dsv" ? 24 : 20;
}
uint32_t integer(const Json &value) {
    if ((!value.is_number_unsigned() && (!value.is_number_integer() || value.get<int64_t>() < 0)) ||
        value.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("View value requires a uint32 integer");
    return value.get<uint32_t>();
}
} // namespace
std::string viewKind(uint16_t type) {
    switch (type) {
    case 0x8c:
        return "srv";
    case 0x8d:
        return "rtv";
    case 0x8e:
        return "dsv";
    case 0x8f:
        return "uav";
    default:
        throw std::runtime_error("View experiment requires an SRV/RTV/DSV/UAV resource");
    }
}
std::vector<std::string> viewKeys(const std::string &kind, unsigned dimension) {
    const auto &table = fields(kind);
    const auto it = table.find(dimension);
    if (it == table.end())
        throw std::runtime_error("Unsupported view dimension");
    std::vector<std::string> keys{"format", "dimension"};
    if (kind == "dsv")
        keys.push_back("flags");
    keys.insert(keys.end(), it->second.begin(), it->second.end());
    return keys;
}
Json normalizeView(const std::string &kind, const Json &patch) {
    std::set<std::string> allowed{"format", "dimension", "flags"};
    for (const auto &[dimension, names] : fields(kind))
        allowed.insert(names.begin(), names.end());
    if (!patch.is_object() || patch.empty())
        throw std::runtime_error("Unknown or empty view descriptor fields");
    Json result = Json::object();
    for (const auto &[key, value] : patch.items()) {
        if (!allowed.contains(key))
            throw std::runtime_error("Unknown view field: " + key);
        result[key] = integer(value);
    }
    if (result.contains("dimension"))
        viewKeys(kind, result.at("dimension").get<unsigned>());
    if (result.contains("flags")) {
        const auto flags = result.at("flags").get<unsigned>();
        const bool valid = kind == "dsv"   ? flags <= 3
                           : kind == "uav" ? (flags <= 2 || flags == 4)
                           : kind == "srv" ? flags <= 1
                                           : flags == 0;
        if (!valid)
            throw std::runtime_error("Invalid view flags");
    }
    for (auto key : {"num_elements", "mip_levels", "array_size", "num_cubes", "w_size"})
        if (result.contains(key) && result.at(key) == 0)
            throw std::runtime_error(std::string(key) + " must be positive");
    return result;
}
Json unpackView(const std::string &kind, Bytes bytes) {
    if (bytes.size() != descriptorSize(kind))
        throw std::runtime_error("Unexpected view descriptor size");
    Reader r(bytes);
    r.skip(4);
    auto keys = viewKeys(kind, r.read<uint32_t>());
    Reader words(bytes);
    Json result = Json::object();
    for (const auto &key : keys)
        result[key] = words.read<uint32_t>();
    return result;
}
std::vector<uint8_t> packView(const std::string &kind, const Json &value) {
    auto normalized = normalizeView(kind, value);
    const auto keys = viewKeys(kind, normalized.at("dimension").get<unsigned>());
    if (normalized.size() != keys.size())
        throw std::runtime_error("Descriptor fields do not match the view dimension");
    std::vector<uint8_t> bytes(descriptorSize(kind), 0);
    size_t offset = 0;
    for (const auto &key : keys) {
        const auto word = normalized.at(key).get<uint32_t>();
        std::memcpy(bytes.data() + offset, &word, sizeof word);
        offset += sizeof word;
    }
    return bytes;
}
Json describeView(const Frame &frame, Id view) {
    const auto &entry = frame.entry(view);
    if (entry.category != 5)
        throw std::runtime_error("View experiment requires a view resource");
    auto kind = viewKind(entry.type);
    auto raw = frame.payload(view);
    if (raw.size() != 24 + descriptorSize(kind))
        throw std::runtime_error("Unexpected view resource size");
    Reader r(raw);
    r.skip(16);
    return {{"view", view},
            {"kind", kind},
            {"resource", r.read<Id>()},
            {"descriptor", unpackView(kind, raw.subspan(24))}};
}
Json mergeView(const std::string &kind, Json previous, const Json &patch) {
    auto normalized = normalizeView(kind, patch);
    auto keys = viewKeys(kind, normalized.value("dimension", previous.at("dimension").get<unsigned>()));
    auto allowed = [&](const std::string &key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    for (const auto &[key, _] : normalized.items())
        if (!allowed(key))
            throw std::runtime_error("Edit fields do not belong to the selected view dimension");
    for (auto it = previous.begin(); it != previous.end();)
        if (!allowed(it.key()))
            it = previous.erase(it);
        else
            ++it;
    previous.update(normalized);
    packView(kind, previous);
    return previous;
}
std::vector<uint8_t> editedViewPayload(const Frame &frame, Id view, const Json &descriptor) {
    const auto info = describeView(frame, view);
    auto prefix = frame.payload(view).first(24);
    std::vector<uint8_t> bytes(prefix.begin(), prefix.end());
    auto packed = packView(info.at("kind").get<std::string>(), descriptor);
    bytes.insert(bytes.end(), packed.begin(), packed.end());
    return bytes;
}
} // namespace flora
