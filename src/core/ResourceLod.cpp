#include "ResourceLod.h"
#include "Contexts.h"
#include "CopyCommands.h"
#include "ReplayCapabilities.h"
#include "TextureCreation.h"
#include <cmath>
namespace flora {
bool isResourceLodRecord(uint16_t type) { return type == 0x3515 || type == 0x3516; }
bool hasResourceLodClamp(const Resource &r) {
    return r.type >= 0x84 && r.type <= 0x86 && !r.desc.empty() && (r.desc.back() & 0x80);
}
ResourceLodRecord readResourceLod(uint16_t type, Bytes payload) {
    if (!isResourceLodRecord(type))
        throw std::runtime_error("Not a resource LOD record");
    Reader r(payload);
    ResourceLodRecord out;
    out.setter = type == 0x3515;
    out.link = r.read<Id>();
    out.context = r.read<Id>();
    if (out.setter) {
        out.resource = r.read<Id>();
        out.value = r.read<float>();
    } else {
        out.value = r.read<float>();
        out.resource = r.read<Id>();
    }
    r.end();
    if (out.link)
        throw std::runtime_error("Linked resource LOD execution is unresolved");
    if (!std::isfinite(out.value) || out.value < 0)
        throw std::runtime_error("Resource minimum LOD is not a finite nonnegative value");
    return out;
}
void validateResourceLod(const Frame &frame, const ResourceLodRecord &record) {
    requireImmediateContext(frame, record.context);
    const auto r = frame.resource(record.resource);
    if (r.type < 0x84 || r.type > 0x86)
        throw std::runtime_error("Resource LOD operand is not a supported texture");
    if (record.setter && !hasResourceLodClamp(r))
        throw std::runtime_error("Resource LOD setter requires RESOURCE_CLAMP");
    if (record.value > textureInfo(r).mips || (!hasResourceLodClamp(r) && record.value != 0))
        throw std::runtime_error("Resource LOD value exceeds the saved texture range");
}
std::set<Id> resourceLodAccesses(const Frame &frame, const Entry &entry, Bytes payload) {
    if (payload.empty())
        payload = frame.payload(entry.id);
    std::set<Id> result;
    auto add = [&](Id id) {
        if (!id)
            return;
        const auto &e = frame.entry(id);
        if (e.category == 5 && e.type >= 0x8c && e.type <= 0x8f) {
            Reader v(frame.payload(id));
            v.skip(16);
            id = v.read<Id>();
        }
        if (id && hasResourceLodClamp(frame.resource(id)))
            result.insert(id);
    };
    const auto t = entry.type;
    if (isDraw(t)) {
        const auto s = frame.state(frame.event(entry.id).state);
        const bool compute = t == 0x35 || t == 0x36;
        for (unsigned stage = compute ? 5 : 0; stage < (compute ? 6u : 5u); ++stage)
            if (s.stages[stage].shader)
                for (auto view : s.stages[stage].srv)
                    add(view);
        for (auto view : compute ? s.csUav : s.rtv)
            add(view);
        if (!compute) {
            add(s.dsv);
            for (auto view : s.omExtended)
                add(view);
        } else
            for (auto view : s.csExtended)
                add(view);
    } else if (isCopyCommand(t)) {
        const auto c = readCopyCommand(t, payload);
        if (c.box && ((*c.box)[0] >= (*c.box)[3] || (*c.box)[1] >= (*c.box)[4] || (*c.box)[2] >= (*c.box)[5]))
            return result;
        add(c.source);
        add(c.destination);
    } else if ((t >= 0x31 && t <= 0x34) || t == 0x245 || t == 0x247 || t == 0x255 || t == 0x257 ||
               t == 0x246) {
        Reader r(payload);
        r.skip(t == 0x246 ? 20 : 16);
        add(r.read<Id>());
    }
    return result;
}
ResourceLodAudit auditResourceLod(const Frame &frame) {
    ResourceLodAudit out;
    bool anyClamp = false, anyCreation = false;
    for (const auto &[id, e] : frame.entries()) {
        if (e.category == 5 && e.type >= 0x84 && e.type <= 0x86) {
            try {
                if (hasResourceLodClamp(frame.resource(id))) {
                    anyClamp = true;
                    out.clamped.insert(id);
                }
            } catch (const std::exception &) {
                // The ordinary per-resource validator reports malformed storage.
                // Do not abort this audit or suppress diagnostics for other entries.
            }
        }
        if (e.category == 7 && isTextureCreation(e.type))
            anyCreation = true;
        if (e.category == 7 && isResourceLodRecord(e.type)) {
            try {
                const auto record = readResourceLod(e.type, frame.payload(id));
                validateResourceLod(frame, record);
                out.records.emplace(id, record);
            } catch (const std::exception &error) {
                Id resource = 0;
                const auto raw = frame.payload(id);
                const size_t offset = e.type == 0x3515 ? 16 : 20;
                if (raw.size() >= offset + sizeof(Id)) {
                    Reader operand(raw);
                    operand.skip(offset);
                    resource = operand.read<Id>();
                }
                out.issues.push_back({id, resource, error.what()});
            }
        }
    }
    if (!anyClamp)
        return out;
    if (anyCreation) {
        for (const auto &[event, record] : auditTextureCreations(frame).records)
            if (record.error.empty() && !record.result && record.resource && !isViewCreation(record.type) &&
                hasResourceLodClamp(frame.resource(record.resource)))
                out.creations[record.resource] = event;
    }
    std::set<Id> seen;
    bool opaquePrefix = false;
    for (const auto &[event, e] : frame.entries()) {
        if (e.category != 7)
            continue;
        if (std::string(replayCapability(e.type).handling) == "unsupported")
            opaquePrefix = true;
        if (!isResourceLodRecord(e.type))
            continue;
        const auto it = out.records.find(event);
        if (it == out.records.end()) {
            opaquePrefix = true;
            continue;
        }
        const auto &r = it->second;
        if (seen.insert(r.resource).second && !r.setter && !out.creations.contains(r.resource) &&
            !opaquePrefix)
            out.initial[r.resource] = {r.value, event};
    }
    std::map<Id, float> known;
    for (const auto &[resource, initial] : out.initial)
        known[resource] = initial.value;
    for (const auto &[id, e] : frame.entries()) {
        if (e.category != 7)
            continue;
        for (const auto &[resource, creation] : out.creations)
            if (creation == id)
                known[resource] = 0;
        if (auto it = out.records.find(id); it != out.records.end()) {
            const auto &r = it->second;
            if (r.setter)
                known[r.resource] = r.value;
            else if (known.contains(r.resource) && known.at(r.resource) != r.value)
                out.issues.push_back(
                    {id, r.resource, "Captured resource LOD observation disagrees with prior state"});
        }
        try {
            for (auto resource : resourceLodAccesses(frame, e))
                if (!known.contains(resource))
                    out.issues.push_back(
                        {id, resource, "Initial resource minimum LOD is not saved before GPU access"});
        } catch (const std::exception &error) {
            out.issues.push_back({id, 0, error.what()});
        }
    }
    return out;
}
} // namespace flora
