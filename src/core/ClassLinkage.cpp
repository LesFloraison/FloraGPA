#include "ClassLinkage.h"
#include <algorithm>
namespace flora {
ClassRecord readClassRecord(const Frame &frame, Id id) {
    const auto &entry = frame.entry(id);
    if (entry.category != 5 || (entry.type != 0x97 && entry.type != 0x98))
        throw std::runtime_error("Expected a class linkage or class instance resource");
    Reader r(frame.payload(id));
    ClassRecord out;
    out.id = id;
    out.instance = entry.type == 0x98;
    r.skip(8);
    auto owner = r.read<Id>();
    if (!out.instance) {
        r.end();
        return out;
    }
    out.linkage = owner;
    out.desc = r.array<uint32_t, 8>();
    out.namesData = r.read<Id>();
    r.end();
    frame.payload(owner, 5, 0x97);
    if (out.desc[7] > 1)
        throw std::runtime_error("Invalid class instance Created flag");
    Reader names(frame.payload(out.namesData, 9, 0x89));
    auto name = [&]() -> std::string {
        auto count = names.read<uint32_t>();
        if (count > 256)
            throw std::runtime_error("Class name exceeds captured string bounds");
        auto raw = names.take(count);
        if (raw.empty())
            return {};
        if (raw.back() != 0 ||
            std::any_of(raw.begin(), raw.end() - 1, [](uint8_t ch) { return ch == 0 || ch > 127; }))
            throw std::runtime_error("Class name must be a terminated ASCII string");
        return {reinterpret_cast<const char *>(raw.data()), raw.size() - 1};
    };
    out.instanceName = name();
    out.typeName = name();
    names.end();
    if ((out.desc[7] ? out.typeName : out.instanceName).empty())
        throw std::runtime_error("Class instance has no required instance/type name");
    return out;
}
Id shaderClassLinkage(const Frame &frame, Id shader) {
    if (!shader)
        return 0;
    const auto &entry = frame.entry(shader);
    if (entry.category != 5 || entry.type < 0x90 || entry.type > 0x95)
        throw std::runtime_error("Expected a shader resource");
    Reader r(frame.payload(shader));
    r.skip(32);
    auto linkage = r.read<Id>();
    r.skip(16);
    r.end();
    if (linkage && readClassRecord(frame, linkage).instance)
        throw std::runtime_error("Shader class linkage has the wrong resource type");
    return linkage;
}
} // namespace flora
