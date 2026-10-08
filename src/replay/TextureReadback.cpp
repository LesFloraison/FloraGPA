#include "InspectionCopyBindings.h"
#include "Replay.h"
#include "Unpredicated.h"
#include <algorithm>
namespace flora {
std::vector<uint8_t> Replay::readTexture(Id id) {
    const auto resource = frame_.resource(id);
    return readTextureStorage(get<ID3D11Resource>(id), resource);
}
std::vector<uint8_t> Replay::readTextureStorage(ID3D11Resource *source, const Resource &resource) {
    requireResourceLod(resource.id);
    // D3D11 functional spec 5.8.7: resource MinLOD affects shader SRV accesses,
    // not resource copies. Copy all stored mips without resetting the clamp;
    // subsequent sampling must keep the captured value, including an empty SRV.
    const auto info = textureInfo(resource);
    if (info.samples != 1 || !info.mips || info.mips > 32 || !info.layers ||
        uint64_t(info.mips) * info.layers > 30720)
        throw std::runtime_error("Texture storage readback requires explicit non-MSAA subresources");
    auto words = resource.desc;
    words[words.size() - 4] = D3D11_USAGE_STAGING;
    words[words.size() - 3] = 0;
    words[words.size() - 2] = D3D11_CPU_ACCESS_READ;
    words[words.size() - 1] = 0;
    Com<ID3D11Resource> staging;
    if (info.dimension == 2) {
        D3D11_TEXTURE1D_DESC desc{};
        std::memcpy(&desc, words.data(), sizeof desc);
        Com<ID3D11Texture1D> texture;
        check(device_->CreateTexture1D(&desc, nullptr, &texture), "Create texture storage staging");
        staging = texture;
    } else if (info.dimension == 4) {
        D3D11_TEXTURE3D_DESC desc{};
        std::memcpy(&desc, words.data(), sizeof desc);
        Com<ID3D11Texture3D> texture;
        check(device_->CreateTexture3D(&desc, nullptr, &texture), "Create texture storage staging");
        staging = texture;
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        std::memcpy(&desc, words.data(), sizeof desc);
        Com<ID3D11Texture2D> texture;
        check(device_->CreateTexture2D(&desc, nullptr, &texture), "Create texture storage staging");
        staging = texture;
    }
    {
        Unpredicated unpredicated(context_.Get());
        InspectionCopyBindings isolation(context_.Get(), options_.warp);
        context_->CopyResource(staging.Get(), source);
    }
    std::vector<uint8_t> result;
    for (uint32_t layer = 0; layer < info.layers; ++layer)
        for (uint32_t mip = 0; mip < info.mips; ++mip) {
            auto [row, rows] =
                pitches(std::max(1u, info.width >> mip), std::max(1u, info.height >> mip), info.format);
            const auto depth = std::max(1u, info.depth >> mip);
            const uint64_t size = uint64_t(row) * rows * depth;
            if (size > result.max_size() - result.size())
                throw std::runtime_error("Texture storage readback size overflow");
            const auto offset = result.size();
            result.resize(offset + size_t(size));
            const auto sub = mip + layer * info.mips;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context_->Map(staging.Get(), sub, D3D11_MAP_READ, 0, &mapped), "Map texture storage");
            if ((rows > 1 && mapped.RowPitch < row) ||
                (depth > 1 && uint64_t(mapped.DepthPitch) < uint64_t(rows) * mapped.RowPitch)) {
                context_->Unmap(staging.Get(), sub);
                throw std::runtime_error("Mapped texture pitches are smaller than packed storage");
            }
            for (uint32_t z = 0; z < depth; ++z)
                for (uint32_t y = 0; y < rows; ++y)
                    std::memcpy(result.data() + offset + (size_t(z) * rows + y) * row,
                                static_cast<const uint8_t *>(mapped.pData) + size_t(z) * mapped.DepthPitch +
                                    size_t(y) * mapped.RowPitch,
                                row);
            context_->Unmap(staging.Get(), sub);
        }
    return result;
}
} // namespace flora
