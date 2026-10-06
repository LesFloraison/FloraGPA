#include "StreamOutput.h"
#include "Contexts.h"
#include "InspectionRecords.h"
#include "PipelineGetters.h"
#include <algorithm>
namespace flora {
Id shaderStreamOutput(const Frame &frame, Id shader) {
    if (!shader)
        return 0;
    const auto &entry = frame.entry(shader);
    if (entry.category != 5 || entry.type != 0x91)
        throw std::runtime_error("Stream-output shader must be a geometry shader resource");
    Reader r(frame.payload(shader));
    r.skip(40);
    auto id = r.read<Id>();
    r.skip(8);
    r.end();
    return id;
}
StreamOutputDeclaration readStreamOutputDeclaration(const Frame &frame, Id id) {
    Reader r(frame.payload(id, 9, 0x85));
    auto count = r.read<uint32_t>();
    if (count > 512)
        throw std::runtime_error("Stream-output declaration exceeds 512 elements");
    StreamOutputDeclaration out;
    out.id = id;
    for (uint32_t i = 0; i < count; ++i) {
        StreamOutputElement e;
        e.stream = r.read<uint32_t>();
        r.skip(4);
        e.semanticId = r.read<Id>();
        e.index = r.read<uint32_t>();
        e.start = r.read<uint8_t>();
        e.count = r.read<uint8_t>();
        e.slot = r.read<uint8_t>();
        r.skip(1);
        if (e.stream > 3 || e.slot > 3 || !e.count || e.start > 3 || (e.semanticId && e.start + e.count > 4))
            throw std::runtime_error("Stream-output declaration component or slot range");
        if (e.semanticId) {
            auto name = frame.data(e.semanticId);
            if (name.empty() || name.size() > 256 || name.back() ||
                std::any_of(name.begin(), name.end() - 1, [](uint8_t ch) { return !ch || ch > 127; }))
                throw std::runtime_error("Stream-output semantic must be a bounded ASCII C string");
            e.semantic = std::string(reinterpret_cast<const char *>(name.data()), name.size() - 1);
        }
        out.entries.push_back(std::move(e));
    }
    auto strides = r.read<uint32_t>();
    if (strides > 4)
        throw std::runtime_error("Stream-output stride count exceeds four");
    for (uint32_t i = 0; i < strides; ++i) {
        auto value = r.read<uint32_t>();
        if (value > 2048 || value % 4)
            throw std::runtime_error("Stream-output stride is unaligned or exceeds 2048");
        out.strides.push_back(value);
    }
    out.rasterizedStream = r.read<uint32_t>();
    if (out.rasterizedStream > 3 && out.rasterizedStream != UINT32_MAX)
        throw std::runtime_error("Invalid rasterized stream");
    r.end();
    return out;
}
bool isStreamOutputTargets(uint16_t type) {
    return type == 0x30bc || type == 0x31bc || type == 0x3325 || type == 0x33eb || type == 0x3503;
}
StreamOutputTargets readStreamOutputTargets(Bytes payload) {
    Reader r(payload);
    if (r.read<Id>())
        throw std::runtime_error("Linked SOSetTargets records are not supported");
    StreamOutputTargets out;
    out.context = r.read<Id>();
    out.count = r.read<uint32_t>();
    if (out.count > 4)
        throw std::runtime_error("Stream-output target count exceeds four");
    if (r.flag()) {
        out.buffers.emplace();
        for (uint32_t i = 0; i < out.count; ++i)
            out.buffers->push_back(r.read<Id>());
    }
    if (r.flag()) {
        out.offsets.emplace();
        for (uint32_t i = 0; i < out.count; ++i)
            out.offsets->push_back(r.read<uint32_t>());
    }
    r.end();
    return out;
}
UnusedStreamOutputLifetime proveUnusedStreamOutputLifetime(const Frame &frame, Id event,
                                                           const std::set<Id> &disabled,
                                                           const std::map<Id, std::vector<uint8_t>> &payloads,
                                                           Id until, bool before,
                                                           const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (cancelled)
        frame.contextRecovery(cancelled);
    auto payload = [&](Id id) -> Bytes {
        auto it = payloads.find(id);
        return it == payloads.end() ? frame.payload(id) : Bytes(it->second);
    };
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || !isStreamOutputTargets(entry.type) || disabled.contains(event))
        throw std::runtime_error("Unused SO lifetime requires an enabled SO setter");
    const auto initial = readStreamOutputTargets(payload(event));
    requireImmediateContext(frame, initial.context);
    UnusedStreamOutputLifetime proof{event};
    auto absentOnly = [&](const StreamOutputTargets &targets) {
        if (targets.context != initial.context)
            throw std::runtime_error("Unused SO lifetime crosses different contexts");
        if (targets.count && !targets.buffers)
            throw std::runtime_error("SOSetTargets requires a buffer array for nonzero count");
        bool absent = false;
        if (targets.buffers)
            for (size_t i = 0; i < targets.buffers->size(); ++i) {
                auto id = targets.buffers->at(i);
                if (!id)
                    continue;
                if (frame.entries().contains(id))
                    throw std::runtime_error(
                        "Unused SO lifetime cannot discard a saved target or its cursor");
                if (!targets.offsets)
                    throw std::runtime_error("Unused SO lifetime requires explicit offsets");
                auto offset = targets.offsets->at(i);
                if (offset != UINT32_MAX && offset % 4)
                    throw std::runtime_error("Unused SO lifetime has an unaligned byte offset");
                absent = true;
                if (std::find(proof.resources.begin(), proof.resources.end(), id) == proof.resources.end())
                    proof.resources.push_back(id);
            }
        return absent;
    };
    if (!absentOnly(initial))
        throw std::runtime_error("Unused SO lifetime has no absent target");
    for (auto it = frame.entries().upper_bound(event); it != frame.entries().end(); ++it) {
        checkCancellation(cancelled);
        const auto &[id, next] = *it;
        if (next.category != 7)
            continue;
        if (until && (id > until || (before && id == until)))
            break;
        if (disabled.contains(id))
            continue;
        auto raw = payload(id);
        try {
            if (isStreamOutputTargets(next.type)) {
                if (absentOnly(readStreamOutputTargets(raw)))
                    continue;
                proof.closingEvent = id;
                return proof;
            }
            if (next.type == 0x242) {
                Reader reader(raw);
                if (reader.read<Id>() || reader.read<Id>() != initial.context)
                    throw std::runtime_error("Closing ClearState has a link or different context");
                reader.end();
                proof.closingEvent = id;
                return proof;
            }
            if (isPipelineGetter(next.type)) {
                // Runtime does not accept getter payload edits, even if structurally valid.
                if (!std::ranges::equal(raw, frame.payload(id)))
                    throw std::runtime_error("Pipeline getter payload experiments are not supported");
                const auto getter = readPipelineGetter(next.type, raw);
                validatePipelineGetter(frame, getter);
                if (getter.context != initial.context)
                    throw std::runtime_error("Getter has a different context");
                continue;
            }
            if (next.type == 0x3013 || next.type == 0x3014) {
                acceptPassiveObjectRecord(next.type, raw);
                Reader reader(raw);
                const auto link = reader.read<Id>(), resource = reader.read<Id>();
                const auto references = reader.read<uint32_t>();
                reader.end();
                if (link || !references ||
                    std::find(proof.resources.begin(), proof.resources.end(), resource) ==
                        proof.resources.end())
                    throw std::runtime_error(
                        "Buffer reference observation does not preserve an absent target's lifetime");
                continue;
            }
            throw std::runtime_error("Command may consume or change unresolved SO state");
        } catch (const std::exception &error) {
            throw std::runtime_error("Unused SO lifetime blocked at event " + std::to_string(id) + ": " +
                                     error.what());
        }
    }
    throw std::runtime_error("Unused SO lifetime has no explicit close before the replay boundary");
}
void validateStreamOutputBindings(const Frame &frame, std::span<const Id> ids,
                                  std::span<const uint32_t> offsets) {
    if (ids.size() > 4 || ids.size() != offsets.size())
        throw std::runtime_error("Stream-output binding array size");
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i]) {
            const auto &entry = frame.entry(ids[i]);
            if (entry.category != 5 || entry.type != 0x83)
                throw std::runtime_error("Stream-output target must be a buffer");
            auto resource = frame.resource(ids[i]);
            if (!(resource.desc.at(2) & 16))
                throw std::runtime_error("Stream-output target lacks STREAM_OUTPUT binding");
            if (offsets[i] != UINT32_MAX && (offsets[i] % 4 || offsets[i] > resource.desc.at(0)))
                throw std::runtime_error("Stream-output byte offset is unaligned or outside buffer");
        }
}
} // namespace flora
