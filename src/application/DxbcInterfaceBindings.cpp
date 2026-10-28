#include "DxbcInterfaceBindings.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <set>
namespace flora::dxbc_exec {
namespace {
struct Metadata {
    Bytes bytes;
    void range(uint64_t offset, uint64_t count, uint64_t stride) const {
        if (offset > bytes.size() || count > (bytes.size() - offset) / stride)
            throw std::runtime_error("DXBC interface metadata bounds");
    }
    uint32_t word(size_t offset) const {
        range(offset, 1, 4);
        uint32_t result;
        std::memcpy(&result, bytes.data() + offset, 4);
        return result;
    }
    uint16_t half(size_t offset) const {
        range(offset, 1, 2);
        uint16_t result;
        std::memcpy(&result, bytes.data() + offset, 2);
        return result;
    }
    std::string name(size_t offset) const {
        range(offset, 1, 1);
        const auto limit = bytes.begin() + offset + std::min<size_t>(4097, bytes.size() - offset);
        const auto end = std::find(bytes.begin() + offset, limit, 0);
        if (end == limit || end == bytes.begin() + offset)
            throw std::runtime_error("Invalid DXBC interface metadata name");
        return {reinterpret_cast<const char *>(bytes.data() + offset),
                size_t(end - (bytes.begin() + offset))};
    }
};
} // namespace
InterfaceBindings resolveInterfaceBindings(Bytes shader, std::span<const InterfaceClassInstance> classes) {
    if (shader.size() > 64 * 1024 * 1024 || classes.size() > 253)
        throw std::runtime_error("DXBC interface input budget exceeded");
    InterfaceBindings result;
    result.identity = nlohmann::json::array();
    const auto parts = readDxbcParts(shader);
    std::optional<Metadata> metadata;
    for (const auto &[tag, bytes] : parts)
        if (tag == 0x45434649) {
            if (metadata)
                throw std::runtime_error("Duplicate DXBC interface metadata");
            metadata = Metadata{bytes};
        }
    if (!metadata) {
        if (!classes.empty())
            throw std::runtime_error("Bound shader classes have no interface metadata");
        return result;
    }
    const auto &data = *metadata;
    data.range(0, 7, 4);
    const auto instances = data.word(0), types = data.word(4), records = data.word(8), slots = data.word(12);
    const auto instanceOffset = data.word(16), typeOffset = data.word(20), slotOffset = data.word(24);
    if (slots > 253 || classes.size() != slots || records > slots || types > 65535 || instances > 65535)
        throw std::runtime_error("DXBC interface binding count mismatch");
    data.range(instanceOffset, instances, 16);
    data.range(typeOffset, types, 12);
    data.range(slotOffset, records, 16);
    std::map<std::string, uint32_t> typeNames, instanceNames;
    for (uint32_t i = 0; i < types; ++i)
        if (!typeNames.emplace(data.name(data.word(typeOffset + i * 12)), i).second)
            throw std::runtime_error("Duplicate DXBC class type name");
    for (uint32_t i = 0; i < instances; ++i)
        if (!instanceNames.emplace(data.name(data.word(instanceOffset + i * 16)), i).second)
            throw std::runtime_error("Duplicate DXBC class instance name");
    uint32_t slot = 0;
    for (uint32_t record = 0; record < records; ++record) {
        const auto at = slotOffset + record * 16;
        const auto span = data.word(at), count = data.word(at + 4), typeIds = data.word(at + 8),
                   tableIds = data.word(at + 12);
        if (!span || span > slots - slot || !count || count > types)
            throw std::runtime_error("Invalid DXBC interface slot range");
        data.range(typeIds, count, 2);
        data.range(tableIds, count, 4);
        std::map<uint32_t, uint32_t> choices;
        for (uint32_t i = 0; i < count; ++i) {
            const auto type = data.half(typeIds + i * 2);
            if (type >= types || !choices.emplace(type, data.word(tableIds + i * 4)).second)
                throw std::runtime_error("Invalid DXBC interface type choice");
        }
        for (uint32_t end = slot + span; slot < end; ++slot) {
            const auto &bound = classes[slot];
            const auto &desc = bound.descriptor;
            uint32_t type = 0;
            Value pointer;
            if (desc[7] == 1) {
                const auto found = typeNames.find(bound.typeName);
                if (found == typeNames.end())
                    throw std::runtime_error("Bound class type is absent from shader metadata");
                type = found->second;
                pointer = Value::known({desc[3], desc[4], desc[5], desc[6]});
            } else if (desc[7] == 0) {
                const auto found = instanceNames.find(bound.instanceName);
                if (found == instanceNames.end())
                    throw std::runtime_error("Bound class instance is absent from shader metadata");
                const auto instance = instanceOffset + found->second * 16;
                type = data.half(instance + 4);
                if (type >= types || desc[1] >= data.half(instance + 6))
                    throw std::runtime_error("Bound class instance array index exceeds shader metadata");
                const auto definition = typeOffset + type * 12;
                pointer.bits[0] = data.half(instance + 8);
                pointer.bits[1] = data.half(instance + 10) + desc[1] * data.half(definition + 6);
                pointer.valid = 3;
                for (uint32_t component = 2; component < 4; ++component) {
                    const auto base = data.half(instance + 8 + component * 2);
                    if (base != UINT16_MAX) {
                        pointer.bits[component] = base + desc[1] * data.half(definition + 4 + component * 2);
                        pointer.valid |= uint8_t(1u << component);
                    }
                }
            } else
                throw std::runtime_error("Invalid observed class creation flag");
            if (!choices.contains(type))
                throw std::runtime_error("Bound class type is incompatible with interface slot");
            if (pointer.bits[0] >= 14 || pointer.bits[1] >= 4096 ||
                ((pointer.valid & 4) && pointer.bits[2] >= 128) ||
                ((pointer.valid & 8) && pointer.bits[3] >= 16))
                throw std::runtime_error("Bound class resource base exceeds the shader limits");
            result.tables.emplace(slot, choices.at(type));
            result.registers.emplace(Register{29, {slot}}, pointer);
            result.identity.push_back({{"slot", slot},
                                       {"object", bound.object},
                                       {"instance_name", bound.instanceName},
                                       {"type_name", bound.typeName},
                                       {"observed_descriptor", desc},
                                       {"shader_type", type},
                                       {"function_table", choices.at(type)},
                                       {"this_bits", pointer.bits},
                                       {"this_valid", pointer.valid}});
        }
    }
    if (slot != slots)
        throw std::runtime_error("Incomplete DXBC interface slot metadata");
    return result;
}
} // namespace flora::dxbc_exec
