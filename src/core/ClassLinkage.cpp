#include "ClassLinkage.h"
#include "PipelineCreation.h"
#include <algorithm>
namespace flora {
ClassIdentityAudit auditClassIdentities(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    ClassIdentityAudit out;
    auto alias = [&](Id source, Id target) {
        if (!source || !target)
            throw std::runtime_error("Missing class linkage identity");
        frame.payload(target, 5, 0x97);
        if (source != target && frame.entries().contains(source))
            throw std::runtime_error("Class linkage alias conflicts with an existing resource identity");
        auto [it, added] = out.aliases.emplace(source, target);
        if (!added && it->second != target)
            throw std::runtime_error("Conflicting class linkage identity mapping");
    };
    for (const auto &[id, e] : frame.entries()) {
        checkCancellation(cancelled);
        if (e.category == 7) {
            try {
                if (e.type == 0x3195 || e.type == 0x3196) {
                    Reader r(frame.payload(id));
                    r.skip(8);
                    auto owner = r.read<Id>();
                    auto result = r.read<int32_t>();
                    r.skip(e.type == 0x3195 ? 4 : 16);
                    auto returned = r.read<Id>();
                    r.end();
                    if (result == 0 && returned && frame.entries().contains(returned))
                        alias(owner, readClassRecord(frame, returned).linkage);
                } else if (isPipelineCreation(e.type) && pipelineCreatedType(e.type) >= 0x90 &&
                           pipelineCreatedType(e.type) <= 0x95) {
                    const auto c = readPipelineCreation(e.type, frame.payload(id));
                    if (!c.result && c.linkage && frame.entries().contains(c.resource))
                        alias(c.linkage, shaderClassLinkage(frame, c.resource));
                }
            } catch (const std::exception &error) {
                out.errors[id] = error.what();
            }
        }
    }
    return out;
}
Id canonicalClassLinkage(const ClassIdentityAudit &a, Id id) {
    if (!a.errors.empty())
        throw std::runtime_error("Class identity event " + std::to_string(a.errors.begin()->first) + ": " +
                                 a.errors.begin()->second);
    const auto it = a.aliases.find(id);
    return it == a.aliases.end() ? id : it->second;
}
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
