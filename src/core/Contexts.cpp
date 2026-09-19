#include "Contexts.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace flora {
std::optional<unsigned> contextVersion(uint16_t type) {
    switch (type) {
    case 0x99:
        return 0;
    case 0x10b:
        return 1;
    case 0x121:
        return 2;
    case 0x123:
        return 3;
    case 0x127:
        return 4;
    default:
        return {};
    }
}
namespace {
ContextRecovery recover(const Frame &frame) {
    ContextRecovery out;
    using Key = std::tuple<Id, Id, uint32_t>;
    std::map<Key, ContextEvidence> pending;
    std::map<Id, std::vector<ContextEvidence>> candidates;
    std::vector<Id> candidateOrder;
    std::set<Id> blocked;
    auto issue = [&](Id context, std::optional<Id> event, const std::string &reason) {
        blocked.insert(context);
        out.issues.push_back({context, event, reason});
    };
    for (const auto &[id, e] : frame.entries()) {
        if (e.category != 7 || (e.type != 0x34ec && e.type != 0x246 && e.type != 0x34ed))
            continue;
        auto raw = frame.payload(id);
        if (raw.size() < 16)
            continue;
        Reader r(raw);
        auto link = r.read<Id>(), owner = r.read<Id>();
        if (!owner || frame.entries().contains(owner))
            continue;
        if (raw.size() != (e.type == 0x34ed ? 28u : 48u)) {
            issue(owner, id, "Malformed Map/Unmap evidence");
            continue;
        }
        if (e.type == 0x34ed) {
            auto resource = r.read<Id>();
            auto sub = r.read<uint32_t>();
            auto found = pending.find({owner, resource, sub});
            if (found != pending.end()) {
                auto proof = found->second;
                pending.erase(found);
                if (!link) {
                    proof.unmap = id;
                    if (!candidates.contains(owner))
                        candidateOrder.push_back(owner);
                    candidates[owner].push_back(proof);
                }
            }
            continue;
        }
        auto hr = r.read<int32_t>();
        auto resource = r.read<Id>();
        auto sub = r.read<uint32_t>(), kind = r.read<uint32_t>(), flags = r.read<uint32_t>();
        auto data = r.read<Id>();
        Key key{owner, resource, sub};
        if (pending.erase(key))
            issue(owner, id, "Overlapping Map evidence");
        if (e.type != 0x34ec || hr != 0 || kind != 1)
            continue;
        try {
            auto desc = frame.resource(resource);
            const auto &d = desc.desc;
            if (desc.type != 0x85 || d.size() != 11)
                throw std::runtime_error("Map READ resource is not a verified Texture2D");
            auto device = frame.entries().find(desc.device);
            if (link || !data || (flags != 0 && flags != 0x100000) || !d[0] || !d[1] || !d[2] || !d[3] ||
                sub >= uint64_t(d[2]) * d[3] || d[5] != 1 || d[6] || d[7] != 3 || d[8] || !(d[9] & 0x20000) ||
                device == frame.entries().end() || device->second.category != 5 ||
                device->second.type != 0x81 || device->second.size != 28)
                throw std::runtime_error("Map READ descriptor, identity or flags are incompatible");
            pending[key] = {id, resource, desc.device, data, 0, e.type, sub, kind, hr};
        } catch (const std::exception &error) {
            issue(owner, id, error.what());
        }
    }
    // Python's observed pending order follows insertion/event order, not the resource ID.
    std::vector<std::pair<Id, ContextEvidence>> unmatched;
    for (auto &[key, proof] : pending)
        unmatched.emplace_back(std::get<0>(key), proof);
    std::sort(unmatched.begin(), unmatched.end(),
              [](const auto &a, const auto &b) { return a.second.event < b.second.event; });
    for (auto &[owner, proof] : unmatched)
        issue(owner, proof.event, "Map READ has no matching Unmap");
    for (auto id : frame.entryOrder()) {
        const auto &e = frame.entry(id);
        if (e.category == 5 && e.type == 0x9a && e.size == 16) {
            Reader r(frame.payload(id));
            r.skip(8);
            Id parent = uint32_t(r.read<Id>());
            if (candidates.contains(parent))
                issue(parent, id, "Command-list parent contradicts immediate context inference");
        }
    }
    for (auto owner : candidateOrder) {
        const auto &proofs = candidates.at(owner);
        std::set<Id> devices;
        for (const auto &p : proofs)
            devices.insert(p.device);
        if (devices.size() != 1)
            issue(owner, {}, "Map READ evidence refers to different devices");
        if (!blocked.contains(owner))
            out.contexts.push_back({owner, proofs.front().device, false, {}, {}, {}, proofs});
    }
    return out;
}
} // namespace
const ContextRecovery &Frame::contextRecovery() const {
    std::call_once(contextOnce_,
                   [&] { contextRecovery_ = std::make_shared<const ContextRecovery>(recover(*this)); });
    return *contextRecovery_;
}
ContextDescription describeContext(const Frame &frame, Id id, bool allowRecovery) {
    auto found = frame.entries().find(id);
    if (found == frame.entries().end()) {
        if (allowRecovery)
            for (const auto &context : frame.contextRecovery().contexts)
                if (context.id == id)
                    return context;
        throw std::runtime_error("Captured context resource " + std::to_string(id) +
                                 " is missing; immediate/deferred kind cannot be determined");
    }
    auto version = contextVersion(found->second.type);
    if (found->second.category != 5 || !version)
        throw std::runtime_error("Expected a captured device context");
    auto raw = frame.payload(id);
    if (raw.size() != 24)
        throw std::runtime_error("Device context record must contain 24 bytes");
    Reader r(raw);
    auto pointer = r.read<Id>(), device = r.read<Id>();
    auto kind = r.read<uint32_t>(), flags = r.read<uint32_t>();
    if (kind > 1)
        throw std::runtime_error("Invalid device context kind");
    return {id, device, kind != 0, version, flags, pointer, {}};
}
void requireImmediateContext(const Frame &frame, Id id) {
    if (describeContext(frame, id).deferred)
        throw std::runtime_error(
            "Replay requires an immediate context; deferred context execution is not implemented");
}
std::optional<unsigned> finishCommandListVersion(uint16_t type) {
    switch (type) {
    case 0x3109:
        return 0;
    case 0x3209:
        return 1;
    case 0x3372:
        return 2;
    case 0x3438:
        return 3;
    case 0x3550:
        return 4;
    default:
        return {};
    }
}
FinishCommandList readFinishCommandList(uint16_t type, Bytes bytes) {
    auto version = finishCommandListVersion(type);
    if (!version)
        throw std::runtime_error("Unknown FinishCommandList wire type");
    if (bytes.size() != 32)
        throw std::runtime_error("FinishCommandList requires a 32-byte payload");
    Reader r(bytes);
    FinishCommandList out;
    out.link = r.read<Id>();
    out.context = r.read<Id>();
    out.hresult = r.read<int32_t>();
    out.restore = r.read<uint32_t>();
    out.reference = r.read<Id>();
    out.version = *version;
    return out;
}
void acceptFinishCommandList(const Frame &frame, const FinishCommandList &command) {
    if (describeContext(frame, command.context).deferred || command.reference || command.hresult > 0)
        throw std::runtime_error(
            "FinishCommandList execution is not restored: only captured immediate-context no-op results with "
            "zero returned reference are supported");
}
} // namespace flora
