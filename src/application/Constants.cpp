#include "Constants.h"
#include "ShaderInspector.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace flora {
using Json = nlohmann::json;
namespace {
struct Scalar {
    const char *name;
    unsigned size;
};
Scalar scalar(unsigned base) {
    switch (base) {
    case 1:
        return {"bool", 4};
    case 2:
        return {"int", 4};
    case 3:
        return {"float", 4};
    case 19:
        return {"uint", 4};
    case 39:
        return {"double", 8};
    default:
        throw std::runtime_error("Unsupported constant scalar/type class");
    }
}
uint64_t align16(uint64_t size) { return (size + 15) / 16 * 16; }
uint64_t layout(const Json &type, const std::string &path, uint64_t offset, Json &leaves,
                unsigned depth = 0) {
    if (depth > 64)
        throw std::runtime_error("Constant type nesting exceeds 64");
    auto elements = type.at("elements").get<uint32_t>();
    if (elements) {
        auto single = type;
        single["elements"] = 0;
        Json unused = Json::array();
        auto span = layout(single, path, 0, unused, depth + 1), stride = align16(span);
        if (!span || stride * elements > 65536)
            throw std::runtime_error("Constant array exceeds the DXBC buffer limit");
        for (uint32_t i = 0; i < elements; ++i)
            layout(single, path + "[" + std::to_string(i) + "]", offset + i * stride, leaves, depth + 1);
        return (elements - 1) * stride + span;
    }
    auto cls = type.at("class_id").get<unsigned>();
    if (cls == 5) {
        auto members = type.value("member_types", Json::array());
        if (members.empty() || members.size() != type.at("members").get<size_t>())
            throw std::runtime_error("Missing reflected structure members");
        uint64_t end = 0;
        for (auto &member : members) {
            auto start = member.at("offset").get<uint32_t>();
            if (start < end)
                throw std::runtime_error("Overlapping reflected structure members");
            end = start + layout(member.at("type"), path + "." + member.at("name").get<std::string>(),
                                 offset + start, leaves, depth + 1);
            if (end > 65536)
                throw std::runtime_error("Constant structure exceeds the DXBC buffer limit");
        }
        return end;
    }
    if (cls > 3)
        throw std::runtime_error("Unsupported constant scalar/type class");
    auto base = type.at("base_type").get<unsigned>();
    auto s = scalar(base);
    auto rows = type.at("rows").get<unsigned>(), cols = type.at("columns").get<unsigned>();
    if (!rows || rows > 4 || !cols || cols > 4 || (cls < 2 && rows != 1) || (cls == 0 && cols != 1))
        throw std::runtime_error("Invalid reflected constant shape");
    Json offsets = Json::array();
    std::string label = s.name;
    if (cls == 2 || cls == 3) {
        auto stride = align16((cls == 2 ? cols : rows) * s.size);
        for (unsigned r = 0; r < rows; ++r)
            for (unsigned c = 0; c < cols; ++c)
                offsets.push_back(offset + (cls == 2 ? r * stride + c * s.size : c * stride + r * s.size));
        label = std::string(cls == 2 ? "row_major " : "column_major ") + s.name + std::to_string(rows) + "x" +
                std::to_string(cols);
    } else {
        for (unsigned c = 0; c < cols; ++c)
            offsets.push_back(offset + c * s.size);
        if (cls == 1)
            label += std::to_string(cols);
    }
    auto last = std::max_element(offsets.begin(), offsets.end())->get<uint64_t>();
    leaves.push_back({{"path", path},
                      {"type_label", label},
                      {"base_type", base},
                      {"class_id", cls},
                      {"rows", rows},
                      {"columns", cols},
                      {"scalar_bytes", s.size},
                      {"component_offsets", offsets}});
    return last + s.size - offset;
}
template <class T> T read(Bytes data) {
    T value;
    std::memcpy(&value, data.data(), sizeof(T));
    return value;
}
Json valueOf(Bytes bytes, unsigned base) {
    if (base == 1)
        return read<uint32_t>(bytes) != 0;
    if (base == 2)
        return read<int32_t>(bytes);
    if (base == 19)
        return read<uint32_t>(bytes);
    double v = base == 3 ? double(read<float>(bytes)) : read<double>(bytes);
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    return v;
}
std::string hex(Bytes bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}
unsigned digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    throw std::runtime_error("Invalid constant hexadecimal bits");
}
std::vector<uint8_t> unhex(const std::string &s, unsigned size) {
    if (s.size() != size * 2)
        throw std::runtime_error("Constant bit width mismatch");
    std::vector<uint8_t> result(size);
    for (unsigned i = 0; i < size; ++i)
        result[i] = uint8_t(digit(s[i * 2]) * 16 + digit(s[i * 2 + 1]));
    return result;
}
template <class T> std::vector<uint8_t> packed(T v) {
    std::vector<uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));
    return out;
}
std::vector<uint8_t> encode(const Json &value, unsigned base, Bytes original) {
    auto s = scalar(base);
    if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text.starts_with("bits:")) {
            text.erase(0, 5);
            if (text.starts_with("0x"))
                text.erase(0, 2);
            auto bytes = unhex(text, s.size);
            std::reverse(bytes.begin(), bytes.end());
            return bytes;
        }
    }
    if (base == 1) {
        if (!value.is_boolean())
            throw std::runtime_error("bool requires true or false");
        if (value == valueOf(original, base))
            return {original.begin(), original.end()};
        return packed(uint32_t(value.get<bool>()));
    }
    if (base == 2 || base == 19) {
        if (!value.is_number_integer())
            throw std::runtime_error(std::string(s.name) + " requires an integer");
        if (base == 19) {
            if ((!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
                value.get<uint64_t>() > UINT32_MAX)
                throw std::runtime_error("Constant value is outside uint range");
            return packed(value.get<uint32_t>());
        }
        if ((value.is_number_unsigned() && value.get<uint64_t>() > INT32_MAX) ||
            (!value.is_number_unsigned() &&
             (value.get<int64_t>() < INT32_MIN || value.get<int64_t>() > INT32_MAX)))
            throw std::runtime_error("Constant value is outside int range");
        return packed(value.get<int32_t>());
    }
    double v;
    if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text != "nan" && text != "inf" && text != "-inf")
            throw std::runtime_error("Floating point strings must be nan, inf, -inf or bits:HEX");
        if (value == valueOf(original, base))
            return {original.begin(), original.end()};
        v = text == "nan" ? std::numeric_limits<double>::quiet_NaN()
                          : std::numeric_limits<double>::infinity() * (text == "-inf" ? -1 : 1);
    } else {
        if (!value.is_number())
            throw std::runtime_error(std::string(s.name) + " requires a number");
        v = value.get<double>();
    }
    if (base == 39)
        return packed(v);
    auto f = static_cast<float>(v);
    if (std::isfinite(v) && !std::isfinite(f))
        throw std::runtime_error("Constant value is outside float range");
    return packed(f);
}
} // namespace
Json constantFields(const Json &variable, Bytes data, Replay::ConstantRange range) {
    Json leaves = Json::array();
    const auto path = variable.at("name").get<std::string>();
    const uint64_t first = uint64_t(range.first) * 16;
    try {
        auto span = layout(variable.at("type"), path, first + variable.at("offset").get<uint32_t>(), leaves);
        if (span > variable.at("size").get<uint32_t>())
            throw std::runtime_error("Reflected constant layout exceeds variable size");
    } catch (const std::exception &e) {
        return Json::array({{{"path", path}, {"status", "unsupported_layout"}, {"reason", e.what()}}});
    }
    const uint64_t limit =
        range.count ? std::min<uint64_t>(data.size(), first + uint64_t(*range.count) * 16) : data.size();
    for (auto &leaf : leaves) {
        auto size = leaf["scalar_bytes"].get<unsigned>();
        auto offsets = leaf["component_offsets"].get<std::vector<uint64_t>>();
        leaf["buffer_offset"] = *std::min_element(offsets.begin(), offsets.end());
        if (std::any_of(offsets.begin(), offsets.end(),
                        [&](auto pos) { return pos < first || pos > limit || size > limit - pos; })) {
            leaf["status"] = "outside_bound_range";
            continue;
        }
        Json values = Json::array(), bits = Json::array();
        for (auto pos : offsets) {
            auto raw = data.subspan(size_t(pos), size);
            values.push_back(valueOf(raw, leaf["base_type"].get<unsigned>()));
            bits.push_back(hex(raw));
        }
        leaf["component_hex"] = bits;
        leaf["status"] = "ready";
        auto cls = leaf["class_id"].get<unsigned>(), cols = leaf["columns"].get<unsigned>();
        if (cls == 2 || cls == 3) {
            leaf["value"] = Json::array();
            for (unsigned r = 0; r < leaf["rows"].get<unsigned>(); ++r)
                leaf["value"].push_back(Json(values.begin() + r * cols, values.begin() + (r + 1) * cols));
        } else
            leaf["value"] = cls == 1 ? values : values[0];
    }
    return leaves;
}
Json constantValue(const Json &field) {
    if (field.value("status", "") != "ready")
        throw std::runtime_error("Constant is unavailable or has an unsupported layout");
    unsigned cls = field.at("class_id"), rows = field.at("rows"), cols = field.at("columns"),
             base = field.at("base_type");
    auto size = scalar(base).size;
    const auto &bits = field.at("component_hex");
    if (!rows || rows > 4 || !cols || cols > 4 || bits.size() != rows * cols || cls > 3 ||
        (cls < 2 && rows != 1) || (cls == 0 && cols != 1))
        throw std::runtime_error("Constant component count mismatch");
    Json values = Json::array();
    for (const auto &component : bits)
        values.push_back(valueOf(unhex(component.get<std::string>(), size), base));
    if (cls < 2)
        return cls == 0 ? values[0] : values;
    Json matrix = Json::array();
    for (unsigned r = 0; r < rows; ++r)
        matrix.push_back(Json(values.begin() + r * cols, values.begin() + (r + 1) * cols));
    return matrix;
}
std::vector<BufferPatch> constantPatches(const Json &field, const Json &value) {
    if (field.value("status", "") != "ready")
        throw std::runtime_error("Constant is unavailable or has an unsupported layout");
    unsigned cls = field.at("class_id"), rows = field.at("rows"), cols = field.at("columns"),
             base = field.at("base_type");
    Json values = Json::array();
    if (cls == 2 || cls == 3) {
        if (!value.is_array() || value.size() != rows)
            throw std::runtime_error("Matrix row count mismatch");
        for (const auto &row : value) {
            if (!row.is_array() || row.size() != cols)
                throw std::runtime_error("Matrix column count mismatch");
            for (const auto &v : row)
                values.push_back(v);
        }
    } else if (cls == 1) {
        if (!value.is_array() || value.size() != cols)
            throw std::runtime_error("Vector component count mismatch");
        values = value;
    } else if (cls == 0)
        values.push_back(value);
    else
        throw std::runtime_error("Unsupported constant type class");
    const auto &offsets = field.at("component_offsets"), &bits = field.at("component_hex");
    if (offsets.size() != values.size() || bits.size() != values.size())
        throw std::runtime_error("Constant component count mismatch");
    std::vector<BufferPatch> changed, result;
    for (size_t i = 0; i < values.size(); ++i) {
        auto original = unhex(bits[i].get<std::string>(), scalar(base).size);
        auto bytes = encode(values[i], base, original);
        if (bytes != original)
            changed.push_back({offsets[i].get<uint64_t>(), std::move(bytes)});
    }
    std::sort(changed.begin(), changed.end(),
              [](const auto &a, const auto &b) { return a.offset < b.offset; });
    for (auto &patch : changed) {
        if (!result.empty() && result.back().offset + result.back().bytes.size() == patch.offset)
            result.back().bytes.insert(result.back().bytes.end(), patch.bytes.begin(), patch.bytes.end());
        else
            result.push_back(std::move(patch));
    }
    return result;
}
Json inspectConstants(const Frame &frame, const Replay &replay, const ReplayOptions &options, Id eventId,
                      Id resource, Bytes data) {
    Json result = Json::array();
    auto event = frame.event(eventId);
    auto state = frame.state(event.state);
    bool compute = event.type == 0x35 || event.type == 0x36;
    const char *names[] = {"vs", "hs", "ds", "gs", "ps", "cs"};
    for (unsigned stage = compute ? 5 : 0; stage < (compute ? 6u : 5u); ++stage) {
        auto &s = state.stages[stage];
        if (std::find(s.cb.begin(), s.cb.end(), resource) == s.cb.end())
            continue;
        Json metadata = Json::object();
        if (s.shader) {
            auto it = options.shaders.find(s.shader);
            metadata = inspectShader(it == options.shaders.end() ? frame.shader(frame.resource(s.shader).data)
                                                                 : Bytes(it->second));
        }
        for (unsigned slot = 0; slot < s.cb.size(); ++slot) {
            if (s.cb[slot] != resource)
                continue;
            auto range = replay.constantRange(stage, slot, resource);
            Json cb;
            for (const auto &binding : metadata.value("bindings", Json::array())) {
                uint64_t start = binding.at("slot"), count = binding.at("count");
                if (binding.at("type") == 0 && start <= slot && slot < start + count)
                    for (const auto &candidate : metadata.value("constant_buffers", Json::array()))
                        if (candidate.at("name") == binding.at("name"))
                            cb = candidate;
            }
            Json item{{"stage", names[stage]},
                      {"slot", slot},
                      {"shader", s.shader},
                      {"first_constant", range.first},
                      {"constant_count", range.count ? Json(*range.count) : Json(nullptr)},
                      {"name", cb.is_null() ? Json(nullptr) : cb.at("name")},
                      {"variables", Json::array()}};
            if (!cb.is_null())
                for (const auto &v : cb.at("variables")) {
                    Json variable{{"name", v.at("name")},
                                  {"offset", v.at("offset")},
                                  {"size", v.at("size")},
                                  {"flags", v.at("flags")},
                                  {"type", v.at("type_layout")}};
                    uint64_t start = uint64_t(range.first) * 16 + v.at("offset").get<uint32_t>(),
                             size = v.at("size");
                    variable["buffer_offset"] = start;
                    variable["fields"] = constantFields(variable, data, range);
                    if (start > data.size() || size > data.size() - start ||
                        (range.count && v.at("offset").get<uint64_t>() + size > uint64_t(*range.count) * 16))
                        variable["status"] = "outside_bound_range";
                    else {
                        variable["bytes_hex"] = hex(data.subspan(size_t(start), size_t(size)));
                        const auto &t = variable.at("type");
                        auto base = t.at("base_type").get<unsigned>();
                        if ((base == 1 || base == 2 || base == 3 || base == 19) &&
                            t.at("class_id").get<unsigned>() < 2 && t.at("elements") == 0 &&
                            t.at("members") == 0 &&
                            size == 4 * t.at("rows").get<unsigned>() * t.at("columns").get<unsigned>()) {
                            variable["values"] = Json::array();
                            for (uint64_t p = start; p < start + size; p += 4)
                                variable["values"].push_back(
                                    valueOf(data.subspan(size_t(p), 4), base == 1 ? 19 : base));
                        }
                    }
                    item["variables"].push_back(std::move(variable));
                }
            result.push_back(std::move(item));
        }
    }
    return result;
}
} // namespace flora
