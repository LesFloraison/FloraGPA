#include "BackBufferIndex.h"
#include "SubmissionIndex.h"
#include "StreamArguments.h"
#include <dxgi1_2.h>
#include <set>
#include <tuple>

namespace flora::dx12 {
namespace {
using format::Reader;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
uint64_t output(Reader &r) { if (!r.flag()) return 0; require(r.read<uint64_t>() == 1, "Invalid swapchain output array"); return r.read<uint64_t>(); }
DXGI_SWAP_CHAIN_DESC1 legacyDesc(Reader &r) {
    require(r.flag(), "Missing legacy swapchain description"); const auto mode = r.read<DXGI_MODE_DESC>();
    DXGI_SWAP_CHAIN_DESC1 out{}; out.Width = mode.Width; out.Height = mode.Height; out.Format = mode.Format;
    out.SampleDesc = r.read<DXGI_SAMPLE_DESC>(); out.BufferUsage = r.read<UINT>(); out.BufferCount = r.read<UINT>();
    r.skip(8); r.skip(4); out.SwapEffect = r.read<DXGI_SWAP_EFFECT>(); out.Flags = r.read<UINT>(); return out;
}
}
nlohmann::json BackBufferIndex::suggestFrameOutput(const StreamDirectory &directory,
                                                   const SubmissionIndex &submissions,
                                                   const CancelCheck &cancelled) const {
    using Json = nlohmann::json;
    checkCancellation(cancelled);
    Json result{{"status", "unavailable"}, {"resource", nullptr}, {"inferred", true},
        {"method", "unique_submitted_transition_to_present"},
        {"scope", "Default output resource inferred from same-queue submitted transitions before the final Present. "
                  "Does not certify displayed pixels, original fidelity or a live backbuffer index."}};
    const auto unavailable = [&](const char *reason) {
        result["status"] = "unavailable";
        result["resource"] = nullptr;
        result["reason"] = reason;
        return result;
    };
    std::map<uint64_t, uint64_t> objects;
    std::map<uint64_t, uint64_t> generations;
    for (const auto &[creation, buffer] : buffers_) {
        const auto [owner, inserted] = generations.emplace(buffer.swapchain, buffer.generation);
        if ((!inserted && owner->second != buffer.generation) || !objects.emplace(buffer.object, creation).second)
            return unavailable("Multiple or aliased swapchain buffer lifetimes require explicit resource selection");
    }
    if (objects.empty()) return unavailable("No captured swapchain buffers");
    // Only the last transition per object in each Present interval matters.
    // Iterate submitted recordings, including repeated invocations, not raw
    // recording order. Cross-queue ownership is deliberately unresolved here.
    std::map<uint64_t, Json> transitions;
    std::set<uint64_t> presentedChains;
    auto nextList = submissions.lists().begin();
    size_t scanned{};
    bool submittedSincePresent = false;
    for (uint64_t record = directory.initializationCount() + 1;
         record <= directory.packets().size(); ++record) {
        if (record % 1024 == 0) checkCancellation(cancelled);
        while (nextList != submissions.lists().end() && nextList->submission < record) ++nextList;
        while (nextList != submissions.lists().end() && nextList->submission == record) {
            const auto &instance = *nextList++;
            submittedSincePresent = true;
            if (!instance.recording)
                return unavailable("A submitted command-list recording is missing");
            const auto &recording = submissions.recordings().at(*instance.recording);
            if (!recording.hasBeginning || !recording.closed)
                return unavailable("A submitted command-list recording is incomplete");
            for (size_t ordinal = 0; ordinal < recording.records.size(); ++ordinal) {
                if (++scanned % 1024 == 0) checkCancellation(cancelled);
                const auto source = recording.records[ordinal];
                const auto &packet = directory.packets().at(size_t(source - 1));
                if (packet.type != 0x1ae3c24e5dabeULL) continue;
                const auto arguments = decodeArguments(packet.type, directory.payload(packet)).values;
                for (const auto &barrier : arguments.at("Barriers")) {
                    const auto kind = barrier.at("Type").get<UINT>();
                    if (kind == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                        for (const char *key : {"ResourceBefore", "ResourceAfter"}) {
                            const auto object = std::stoull(barrier.at(key).get<std::string>());
                            // NULL denotes potentially aliased placed/reserved
                            // resources; GetBuffer resources have no such heap mapping.
                            if (object && objects.contains(object))
                                return unavailable("Aliasing barriers may affect a swapchain buffer");
                        }
                        continue;
                    }
                    if (kind != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
                    const auto object = std::stoull(barrier.at("Resource").get<std::string>());
                    const auto found = objects.find(object);
                    if (found == objects.end()) continue;
                    const auto &buffer = buffers_.at(found->second);
                    if (instance.queue != buffer.queue || barrier.at("Flags") != 0 ||
                        (barrier.at("Subresource") != 0 && barrier.at("Subresource") != UINT32_MAX))
                        return unavailable("Cross-queue, split or partial backbuffer transitions require explicit selection");
                    transitions[object] = {
                        {"resource", std::to_string(found->second)}, {"object", std::to_string(object)},
                        {"swapchain", std::to_string(buffer.swapchain)}, {"queue", std::to_string(instance.queue)},
                        {"serialized_index", buffer.index}, {"source_record", std::to_string(source)},
                        {"execution", std::to_string(instance.firstExecution + ordinal)},
                        {"submission", std::to_string(instance.submission)},
                        {"recording_generation", std::to_string(recording.generation)},
                        {"state_before", barrier.at("StateBefore")}, {"state_after", barrier.at("StateAfter")}};
                }
            }
        }
        const auto &packet = directory.packets().at(size_t(record - 1));
        if (packet.type == 0x1d53273029c1eULL || packet.type == 0x188b77bd89599ULL ||
            packet.type == 0x1df000dce61d3ULL)
            return unavailable("Frame swapchain creation or resizing requires explicit resource selection");
        if (packet.type != 0x1d53218275f72ULL) continue;
        const auto args = decodeArguments(packet.type, directory.payload(packet)).values;
        const auto chain = std::stoull(args.at("SwapChain").get<std::string>());
        if (args.at("PresentFlags").get<UINT>() & DXGI_PRESENT_TEST) continue;
        if (args.at("HRESULT") != 0)
            return unavailable("A non-successful Present requires explicit resource selection");
        presentedChains.insert(chain);
        submittedSincePresent = false;
        if (presentedChains.size() != 1)
            return unavailable("Multiple presenting swapchains require explicit resource selection");
        Json candidates = Json::array();
        bool nonPresentState = false;
        for (auto it = transitions.begin(); it != transitions.end();) {
            const auto &transition = it->second;
            if (transition.at("swapchain") != std::to_string(chain)) { ++it; continue; }
            const auto before = transition.at("state_before").get<UINT>();
            const auto after = transition.at("state_after").get<UINT>();
            if (after == D3D12_RESOURCE_STATE_PRESENT &&
                (before == D3D12_RESOURCE_STATE_RENDER_TARGET || before == D3D12_RESOURCE_STATE_COPY_DEST))
                candidates.push_back(transition);
            else nonPresentState = true;
            it = transitions.erase(it);
        }
        result["present_record"] = std::to_string(record);
        result["swapchain"] = std::to_string(chain);
        result["candidates"] = candidates;
        if (candidates.size() == 1 && !nonPresentState) {
            result["status"] = "suggested";
            result["resource"] = candidates.front().at("resource");
            result["reason"] = "One submitted backbuffer transitions from RT/COPY_DEST to PRESENT on its owning queue";
        } else {
            result["status"] = "unavailable";
            result["resource"] = nullptr;
            result["reason"] = "No unique backbuffer transition to PRESENT in the final presentation interval";
        }
    }
    checkCancellation(cancelled);
    if (presentedChains.empty()) return unavailable("No successful non-test Present");
    if (!transitions.empty() || submittedSincePresent)
        return unavailable("Submitted work after the final Present requires explicit resource selection");
    return result;
}
BackBufferIndex::BackBufferIndex(const StreamDirectory &directory, const CancelCheck &cancelled) {
    struct Chain { uint64_t queue{}, generation{}; DXGI_SWAP_CHAIN_DESC1 desc{}; };
    std::map<uint64_t, Chain> chains;
    std::map<std::tuple<uint64_t, uint64_t, UINT>, std::pair<uint64_t, uint64_t>> identities;
    const auto readCreation = [&](const StreamPacket &p) -> std::pair<uint64_t, Chain> {
        Reader r(directory.payload(p)); r.skip(8); const auto queue = r.read<uint64_t>(); DXGI_SWAP_CHAIN_DESC1 desc{};
        if (p.type == 0x188b77bd89599ULL) {
            r.skip(8); require(r.flag(), "Missing swapchain description"); static_assert(sizeof(desc) == 48); desc = r.read<DXGI_SWAP_CHAIN_DESC1>();
            if (r.flag()) r.skip(sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC)); r.skip(8);
        } else desc = legacyDesc(r);
        const auto object = output(r); const auto hr = r.read<HRESULT>(); r.end();
        return {SUCCEEDED(hr) ? object : 0, Chain{queue, 1, desc}};
    };
    // Keyframe dependency serialization can put GetBuffer before its parent
    // swapchain creation (Talos 35118 -> 42874). Only unique initial owners can
    // be resolved this way; frame resizes retain their chronological boundary.
    for (size_t i = 0; i < directory.initializationCount(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i];
        if (p.type != 0x188b77bd89599ULL && p.type != 0x1df000dce61d3ULL) continue;
        const auto [object, chain] = readCreation(p); if (object) require(chains.emplace(object, chain).second, "Ambiguous initialization swapchain lifetime");
    }
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i]; Reader r(directory.payload(p));
        if (p.type == 0x188b77bd89599ULL || p.type == 0x1df000dce61d3ULL) {
            if (i < directory.initializationCount()) continue;
            auto [object, chain] = readCreation(p);
            if (object) { chain.generation = chains.contains(object) ? chains.at(object).generation + 1 : 1; chains[object] = chain; }
        } else if (p.type == 0x1d53273029c1eULL) {
            const auto object = r.read<uint64_t>(); const auto count = r.read<UINT>(), width = r.read<UINT>(), height = r.read<UINT>();
            const auto format = r.read<DXGI_FORMAT>(); const auto flags = r.read<UINT>(); const auto hr = r.read<HRESULT>(); r.end();
            if (FAILED(hr)) continue; require(chains.contains(object), "ResizeBuffers has no swapchain description"); auto &chain = chains.at(object);
            ++chain.generation; if (count) chain.desc.BufferCount = count; chain.desc.Width = width; chain.desc.Height = height;
            if (format != DXGI_FORMAT_UNKNOWN) chain.desc.Format = format; chain.desc.Flags = flags;
        } else if (p.type == 0x1d53215a11fecULL) {
            const auto object = r.read<uint64_t>(); const auto desc = legacyDesc(r); const auto hr = r.read<HRESULT>(); r.end();
            if (SUCCEEDED(hr) && chains.contains(object)) chains.at(object).desc = desc;
        } else if (p.type == 0x1d532a4b90915ULL) {
            const auto object = r.read<uint64_t>(); const auto index = r.read<UINT>(); const auto iid = r.read<GUID>(); const auto buffer = output(r);
            const auto hr = r.read<HRESULT>(); r.end(); if (FAILED(hr) || !buffer) continue;
            require(iid == __uuidof(ID3D12Resource) && chains.contains(object), "Unsupported or missing swapchain buffer description"); const auto &chain = chains.at(object); const auto &desc = chain.desc;
            require(desc.Width && desc.Height && desc.BufferCount && index < desc.BufferCount && desc.Format != DXGI_FORMAT_UNKNOWN && !desc.Stereo,
                "Swapchain buffer dimensions, index or stereo layout are unresolved");
            const auto key = std::tuple{object, chain.generation, index};
            if (const auto known = identities.find(key); known != identities.end()) { require(known->second.first == buffer, "Swapchain buffer identity changed without resize"); continue; }
            D3D12_RESOURCE_DESC resource{}; resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; resource.Width = desc.Width; resource.Height = desc.Height;
            resource.DepthOrArraySize = 1; resource.MipLevels = 1; resource.Format = desc.Format; resource.SampleDesc = desc.SampleDesc;
            if (desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            buffers_.emplace(i + 1, BackBufferRecipe{object, chain.generation, chain.queue, buffer, index, resource}); identities.emplace(key, std::pair{buffer, i + 1});
        }
    }
}
}
