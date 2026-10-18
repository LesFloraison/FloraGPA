#include "BackBufferIndex.h"
#include <dxgi1_2.h>
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
