#include "SrvEdits.h"
#include "core/Contexts.h"
#include <algorithm>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
uint32_t integer(const Json &v, uint32_t max = UINT32_MAX) {
    if ((!v.is_number_unsigned() && (!v.is_number_integer() || v.get<int64_t>() < 0)) ||
        v.get<uint64_t>() > max)
        throw std::runtime_error("SRV value requires an unsigned integer in range");
    return v.get<uint32_t>();
}
std::set<std::string> allowed(unsigned dimension) {
    const auto &fields = srvFields(dimension);
    std::set<std::string> out(fields.begin(), fields.end());
    out.insert("format");
    out.insert("dimension");
    return out;
}
void retain(Json &values, unsigned dimension) {
    const auto keys = allowed(dimension);
    for (auto it = values.begin(); it != values.end();) {
        if (!keys.contains(it.key()))
            it = values.erase(it);
        else
            ++it;
    }
}
} // namespace
const std::vector<std::string> &srvFields(unsigned dimension) {
    static const std::array<std::vector<std::string>, 12> fields{
        {{},
         {"first_element", "num_elements"},
         {"most_detailed_mip", "mip_levels"},
         {"most_detailed_mip", "mip_levels", "first_array_slice", "array_size"},
         {"most_detailed_mip", "mip_levels"},
         {"most_detailed_mip", "mip_levels", "first_array_slice", "array_size"},
         {},
         {"first_array_slice", "array_size"},
         {"most_detailed_mip", "mip_levels"},
         {"most_detailed_mip", "mip_levels"},
         {"most_detailed_mip", "mip_levels", "first_2d_array_face", "num_cubes"},
         {"first_element", "num_elements", "flags"}}};
    if (!dimension || dimension >= fields.size())
        throw std::runtime_error("Unsupported SRV dimension");
    return fields[dimension];
}
const char *srvDimensionName(unsigned dimension) {
    static constexpr const char *names[]{"Unknown",        "Buffer",           "Texture1D",
                                         "Texture1DArray", "Texture2D",        "Texture2DArray",
                                         "Texture2DMS",    "Texture2DMSArray", "Texture3D",
                                         "TextureCube",    "TextureCubeArray", "BufferEx"};
    srvFields(dimension);
    return names[dimension];
}
unsigned srvStage(const std::string &name) {
    static constexpr const char *names[]{"vs", "hs", "ds", "gs", "ps", "cs"};
    for (unsigned i = 0; i < 6; ++i)
        if (name == names[i])
            return i;
    throw std::runtime_error("SRV stage must be vs/hs/ds/gs/ps/cs");
}
unsigned srvSlot(const Json &value) { return integer(value, 127); }
Json normalizeSrv(const Json &values) {
    if (!values.is_object() || values.empty())
        throw std::runtime_error("Unknown or empty SRV descriptor fields");
    std::set<std::string> fields{"format", "dimension"};
    for (unsigned dim = 1; dim <= 11; ++dim) {
        const auto &names = srvFields(dim);
        fields.insert(names.begin(), names.end());
    }
    Json out = Json::object();
    for (const auto &[key, v] : values.items()) {
        if (!fields.contains(key))
            throw std::runtime_error("Unknown SRV field: " + key);
        out[key] = integer(v);
    }
    if (out.contains("dimension"))
        srvFields(out["dimension"].get<unsigned>());
    if (out.value("flags", 0u) & ~1u)
        throw std::runtime_error("SRV BufferEx flags must be 0 or RAW (1)");
    for (auto key : {"num_elements", "mip_levels", "array_size", "num_cubes"})
        if (out.contains(key) && out[key] == 0)
            throw std::runtime_error(std::string(key) + " must be positive");
    return out;
}
Json mergeSrv(Json previous, const Json &patch) {
    const auto normalized = normalizeSrv(patch);
    if (previous.is_null())
        previous = Json::object();
    if (normalized.contains("dimension"))
        retain(previous, normalized["dimension"].get<unsigned>());
    previous.update(normalized);
    return previous;
}
D3D11_SHADER_RESOURCE_VIEW_DESC nativeSrv(const Json &values) {
    const auto v = normalizeSrv(values);
    const auto dim = v.at("dimension").get<unsigned>();
    const auto fields = allowed(dim);
    if (v.size() != fields.size())
        throw std::runtime_error("SRV fields must match the selected dimension");
    for (const auto &[key, _] : v.items())
        if (!fields.contains(key))
            throw std::runtime_error("SRV fields must match the selected dimension");
    std::array<uint32_t, 6> words{v.at("format").get<uint32_t>(), dim};
    size_t index = 2;
    for (const auto &key : srvFields(dim))
        words[index++] = v.at(key).get<uint32_t>();
    D3D11_SHADER_RESOURCE_VIEW_DESC descriptor{};
    static_assert(sizeof(descriptor) == sizeof(words));
    std::memcpy(&descriptor, words.data(), sizeof descriptor);
    return descriptor;
}
Json capturedSrv(const Frame &frame, Id view) {
    if (!view)
        throw std::runtime_error("SRV descriptor editing requires a bound non-null view");
    Reader r(frame.payload(view, 5, 0x8c));
    r.skip(24);
    Json value{{"format", r.read<uint32_t>()}};
    const auto dimension = r.read<uint32_t>();
    value["dimension"] = dimension;
    const auto words = r.array<uint32_t, 4>();
    r.end();
    size_t i = 0;
    for (const auto &field : srvFields(dimension))
        value[field] = words[i++];
    return value;
}
Json effectiveSrv(const Frame &frame, Id event, unsigned stage, unsigned slot, const Json &patch,
                  const std::map<Id, SrvBinding> &setters, const State *effective) {
    if (stage >= 6 || slot >= 128)
        throw std::runtime_error("Invalid SRV experiment target");
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || !isDraw(entry.type))
        throw std::runtime_error("Select a draw or dispatch");
    const auto draw = frame.event(event);
    requireImmediateContext(frame, draw.context);
    const auto state =
        effective ? *effective : effectiveSrvBindings(frame, event, frame.state(draw.state), setters);
    const auto view = state.stages[stage].srv[slot];
    auto value = capturedSrv(frame, view);
    const auto normalized = normalizeSrv(patch);
    const auto dimension = normalized.value("dimension", value.at("dimension").get<unsigned>());
    const auto keys = allowed(dimension);
    for (const auto &[key, _] : normalized.items())
        if (!keys.contains(key))
            throw std::runtime_error("SRV edit fields do not belong to the selected dimension");
    retain(value, dimension);
    value.update(normalized);
    nativeSrv(value);
    Reader r(frame.payload(view));
    r.skip(16);
    const auto resource = frame.resource(r.read<Id>());
    if (resource.type < 0x83 || resource.type > 0x87)
        throw std::runtime_error("Unsupported SRV resource");
    return value;
}
} // namespace flora
