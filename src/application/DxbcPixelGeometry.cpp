#include "DxbcPixelGeometry.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <set>

namespace flora::dxbc_exec {
namespace {
using Json = nlohmann::json;
using Row = std::vector<uint32_t>;
using Field = PixelInputBinding;
std::string lower(std::string value) {
    for (auto &c : value)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Pixel geometry storage write bounds");
    std::memcpy(bytes.data() + offset, &value, 4);
}
std::vector<uint8_t> signature(const std::vector<Field> &fields, bool output, bool precision = false) {
    const size_t rowSize = precision ? 32 : 24;
    std::vector<uint8_t> result(8 + rowSize * fields.size());
    put(result, 0, uint32_t(fields.size()));
    put(result, 4, 8);
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto &f = fields[i];
        const auto base = 8 + i * rowSize + (precision ? 4 : 0);
        put(result, base, uint32_t(result.size()));
        put(result, base + 4, f.index);
        put(result, base + 8, f.system);
        put(result, base + 12, f.type);
        put(result, base + 16, f.reg);
        result[base + 20] = uint8_t(f.mask);
        result[base + 21] = uint8_t(output ? 15 ^ f.mask : f.mask);
        if (precision)
            put(result, base + 24, f.precision);
        result.insert(result.end(), f.semantic.begin(), f.semantic.end());
        result.push_back(0);
    }
    while (result.size() % 4)
        result.push_back(0);
    return result;
}
Row instruction(uint32_t op, Row args = {}) {
    args.insert(args.begin(), op | (uint32_t(args.size() + 1) << 24));
    return args;
}
Row dest(uint32_t kind, uint32_t reg, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), reg};
}
struct Source {
    std::string name;
    uint32_t index, system, type, offset;
    std::vector<uint32_t> components;
};
} // namespace
static PixelGeometryBridge makeBridge(const PostTransformGeometry &geometry, const PixelInputShader &pixel,
                                      uint64_t ordinal, uint64_t count, std::optional<uint32_t> primitiveId,
                                      bool range) {
    const auto &meta = geometry.report;
    const auto factor = meta.at("vertices_per_primitive").get<uint32_t>();
    const auto primitives = meta.at("primitives").get<uint64_t>();
    const auto vertices = meta.at("vertices").get<uint64_t>();
    const auto sourceStride = meta.at("stride").get<uint32_t>();
    const auto primitiveIds = validatedPixelPrimitiveIds(geometry);
    if (!primitiveIds.empty() && primitiveId)
        throw std::runtime_error("Cannot override observed pipeline primitive IDs");
    if (factor < 1 || factor > 3 || !sourceStride || primitives > UINT64_MAX / factor ||
        vertices != primitives * factor || vertices > geometry.bytes.size() / sourceStride ||
        vertices * sourceStride != geometry.bytes.size() || ordinal >= primitives || !count ||
        count > primitives - ordinal || count > UINT32_MAX / factor)
        throw std::runtime_error("Invalid or unselected expanded pixel geometry");
    if (meta.value("geometry_stage", std::string()) != "final_pre_rasterization" ||
        meta.value("statistics_only", false))
        throw std::runtime_error("Pixel geometry requires final pre-rasterization vertex bytes");
    std::vector<Source> sources;
    std::set<std::pair<std::string, uint32_t>> names;
    for (const auto &a : meta.at("attributes")) {
        Source s{a.at("semantic").get<std::string>(),  a.at("index").get<uint32_t>(),
                 a.at("system_value").get<uint32_t>(), a.at("component_type").get<uint32_t>(),
                 a.at("offset").get<uint32_t>(),       a.at("components").get<std::vector<uint32_t>>()};
        const auto componentCount = a.at("component_count").get<uint32_t>();
        std::set<uint32_t> unique(s.components.begin(), s.components.end());
        if (s.name.empty() || s.name.find('\0') != std::string::npos || s.type < 1 || s.type > 3 ||
            componentCount != s.components.size() || !componentCount || componentCount > 4 ||
            unique.size() != componentCount || *unique.rbegin() > 3 || s.offset > sourceStride ||
            componentCount > (sourceStride - s.offset) / 4 || !names.emplace(lower(s.name), s.index).second)
            throw std::runtime_error("Invalid pixel geometry source attribute");
        sources.push_back(std::move(s));
    }
    auto find = [&](const Field &f) -> const Source * {
        const Source *found = nullptr;
        for (const auto &s : sources)
            if (lower(s.name) == lower(f.semantic) && s.index == f.index) {
                if (s.system != f.system || s.type != f.type || found)
                    throw std::runtime_error("Incompatible pixel geometry producer semantic");
                found = &s;
            }
        return found;
    };
    std::vector<Field> outputs;
    std::set<uint32_t> occupied;
    std::set<std::pair<std::string, uint32_t>> emitted;
    bool position = false;
    for (auto f : pixel.inputSignature) {
        if (f.reg == UINT32_MAX)
            continue;
        if (f.reg >= 32 || !f.mask || f.mask > 15 || (f.used & ~f.mask))
            throw std::runtime_error("Invalid compact pixel geometry signature");
        occupied.insert(f.reg);
        if (f.system == 9 || f.system == 10)
            continue; // Front face and sample index are rasterizer inputs.
        if (f.system > 7)
            throw std::runtime_error("Unsupported pixel geometry system value");
        if (!emitted.emplace(lower(f.semantic), f.index).second)
            throw std::runtime_error("Duplicate compact pixel geometry semantic");
        if (f.system == 1) {
            if (f.mask != 15 || f.type != 3)
                throw std::runtime_error("Pixel geometry requires complete float4 position");
            position = true;
        }
        outputs.push_back(std::move(f));
    }
    // Preserve all rasterization-affecting outputs even when the PS does not
    // read them. Extra registers cannot collide with rasterizer-generated PS inputs.
    for (const auto &s : sources) {
        if (s.system < 1 || s.system > 5 || emitted.contains({lower(s.name), s.index}))
            continue;
        uint32_t reg = 0;
        while (occupied.contains(reg))
            ++reg;
        if (reg >= 32)
            throw std::runtime_error("Pixel geometry output registers exhausted");
        occupied.insert(reg);
        uint32_t mask = 0;
        for (auto c : s.components)
            mask |= 1u << c;
        outputs.push_back({s.name, s.index, s.system, s.type, reg, mask, mask, 0});
        emitted.emplace(lower(s.name), s.index);
        if (s.system == 1)
            position = true;
    }
    if (!position)
        throw std::runtime_error("Pixel geometry position is missing");
    std::map<uint32_t, uint32_t> masks, payloadSlots;
    for (const auto &f : outputs) {
        if (masks[f.reg] & f.mask)
            throw std::runtime_error("Overlapping pixel geometry output masks");
        masks[f.reg] |= f.mask;
    }
    for (const auto &[reg, mask] : masks)
        payloadSlots.emplace(reg, uint32_t(payloadSlots.size()));
    PixelGeometryBridge result;
    result.stride = uint32_t(payloadSlots.size()) * 16;
    result.verticesPerPrimitive = factor;
    if (count > SIZE_MAX / factor / result.stride)
        throw std::runtime_error("Pixel geometry range allocation overflow");
    result.vertices.resize(size_t(count) * factor * result.stride);
    Json mapping = Json::array();
    for (const auto &f : outputs) {
        const auto *s = find(f);
        const bool observed = !s && f.system == 7 && !primitiveIds.empty();
        const bool supplied = !s && f.system == 7 && primitiveId.has_value();
        if (!s && !supplied && !observed)
            throw std::runtime_error("Pixel geometry producer value is missing: " + f.semantic +
                                     std::to_string(f.index));
        if (s && f.system == 7 && primitiveId)
            throw std::runtime_error("Cannot override a captured primitive identifier");
        if (f.system == 7 && (f.type != 1 || !std::has_single_bit(f.mask)))
            throw std::runtime_error("Invalid pixel geometry primitive identifier");
        const auto first = std::countr_zero(f.mask);
        Json copied = Json::array();
        for (uint32_t c = 0; c < 4; ++c) {
            if (!(f.mask & (1u << c)))
                continue;
            const auto component = c - first;
            size_t sourceIndex = 0;
            if (s) {
                const auto it = std::find(s->components.begin(), s->components.end(), component);
                if (it == s->components.end())
                    throw std::runtime_error("Pixel geometry producer component is missing");
                sourceIndex = size_t(it - s->components.begin());
            }
            for (uint64_t vertex = 0; vertex < count * factor; ++vertex) {
                uint32_t value = primitiveId.value_or(0);
                if (observed)
                    value = primitiveIds[size_t(ordinal + vertex / factor)];
                if (s) {
                    const auto offset =
                        (ordinal * factor + vertex) * sourceStride + s->offset + sourceIndex * 4;
                    value = Reader(Bytes(geometry.bytes).subspan(size_t(offset), 4)).read<uint32_t>();
                }
                put(result.vertices, size_t(vertex) * result.stride + payloadSlots.at(f.reg) * 16 + c * 4,
                    value);
            }
            copied.push_back({{"output_component", c},
                              {"semantic_component", component},
                              {"source_byte_offset", s ? Json(s->offset + sourceIndex * 4) : Json(nullptr)}});
        }
        mapping.push_back({{"semantic", f.semantic},
                           {"index", f.index},
                           {"system_value", f.system},
                           {"component_type", f.type},
                           {"register", f.reg},
                           {"mask", f.mask},
                           {"slot", payloadSlots.at(f.reg)},
                           {"source", observed   ? "observed_pipeline_primitive_id"
                                      : supplied ? "explicit_original_primitive_id"
                                                 : "captured_final_geometry"},
                           {"components", copied}});
    }
    std::vector<Field> bridgeFields;
    const bool minimumPrecision =
        std::any_of(outputs.begin(), outputs.end(), [](const auto &f) { return f.precision != 0; });
    DxbcProgram vs{{0x10040, 0}, {}}, gs{{minimumPrecision ? 0x20050u : 0x20040u, 0}, {}};
    if (minimumPrecision)
        gs.instructions.push_back(instruction(106 | (1u << 16)));
    gs.instructions.push_back(instruction(93 | (factor << 11)));
    gs.instructions.push_back(instruction(92 | ((factor == 1 ? 1u : factor == 2 ? 3u : 5u) << 11)));
    gs.instructions.push_back(instruction(94, {factor}));
    for (uint32_t slot = 0; slot < payloadSlots.size(); ++slot) {
        bridgeFields.push_back({"FLORA_GEOMETRY", slot, 0, 1, slot, 15, 15, 0});
        vs.instructions.push_back(instruction(95, dest(1, slot)));
        vs.instructions.push_back(instruction(101, dest(2, slot)));
        gs.instructions.push_back(instruction(95, {0x2010f2, factor, slot}));
    }
    for (const auto &f : outputs) {
        auto args = dest(2, f.reg, f.mask);
        if (f.precision) {
            args[0] |= 0x80000000u;
            args.insert(args.begin() + 1, 1u | (f.precision << 14));
        }
        if (f.system)
            args.push_back(f.system);
        gs.instructions.push_back(instruction(f.system ? 103u : 101u, std::move(args)));
    }
    for (uint32_t slot = 0; slot < payloadSlots.size(); ++slot)
        vs.instructions.push_back(instruction(54, {0x1020f2, slot, 0x101e46, slot}));
    vs.instructions.push_back(instruction(62));
    for (uint32_t vertex = 0; vertex < factor; ++vertex) {
        for (const auto &f : outputs) {
            auto args = dest(2, f.reg, f.mask);
            if (f.precision) {
                args[0] |= 0x80000000u;
                args.insert(args.begin() + 1, 1u | (f.precision << 14));
            }
            args.insert(args.end(), {0x201e46, vertex, payloadSlots.at(f.reg)});
            gs.instructions.push_back(instruction(54, std::move(args)));
        }
        gs.instructions.push_back(instruction(19));
    }
    gs.instructions.push_back(instruction(62));
    const auto input = signature(bridgeFields, false), bridgeOutput = signature(bridgeFields, true),
               output = signature(outputs, true, true), vsCode = writeDxbcProgram(vs),
               gsCode = writeDxbcProgram(gs);
    result.vertexShader = makeDxbc({{0x4e475349, input}, {0x4e47534f, bridgeOutput}, {0x52444853, vsCode}});
    result.geometryShader = makeDxbc({{0x4e475349, input}, {0x3147534f, output}, {0x58454853, gsCode}});
    result.identity = {{"format", "flora.pixel-geometry-bridge"},
                       {"version", 1},
                       {"primitive_ordinal", ordinal},
                       {"original_primitive_id", primitiveId ? Json(*primitiveId) : Json(nullptr)},
                       {"vertices_per_primitive", factor},
                       {"source_stride", sourceStride},
                       {"stride", result.stride},
                       {"input_register", pixel.reg},
                       {"output_mapping", mapping},
                       {"scope", "isolated_final_primitive_input_observation"},
                       {"original_fragment_eligibility", false}};
    if (range) {
        result.identity["version"] = 2;
        result.identity["primitive_count"] = count;
        result.identity["scope"] = "isolated_final_geometry_range_input_observation";
    }
    if (!primitiveIds.empty()) {
        result.identity["version"] = 3;
        result.identity["primitive_identity"] = meta.at("primitive_identity");
        result.identity["original_primitive_id"] =
            range ? Json(nullptr) : Json(primitiveIds[size_t(ordinal)]);
    }
    return result;
}
PixelGeometryBridge pixelGeometryBridge(const PostTransformGeometry &geometry, const PixelInputShader &pixel,
                                        uint64_t ordinal, std::optional<uint32_t> primitiveId) {
    return makeBridge(geometry, pixel, ordinal, 1, primitiveId, false);
}
PixelGeometryBridge pixelGeometryRangeBridge(const PostTransformGeometry &geometry,
                                             const PixelInputShader &pixel, uint64_t first, uint64_t count) {
    return makeBridge(geometry, pixel, first, count, {}, true);
}
} // namespace flora::dxbc_exec
