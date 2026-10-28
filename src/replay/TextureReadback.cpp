#include "InspectionCopyBindings.h"
#include "Replay.h"
#include "Unpredicated.h"
#include <algorithm>
namespace flora {
std::optional<std::vector<uint8_t>> Replay::readTexturePoint(Id id, uint32_t x, uint32_t y, uint32_t slice,
                                                             uint32_t mip, uint32_t layer) {
    const auto resource = frame_.resource(id);
    const auto info = textureInfo(resource);
    if (!info.mips || info.mips > 32 || mip >= info.mips || layer >= info.layers ||
        x >= std::max(1u, info.width >> mip) || y >= std::max(1u, info.height >> mip) ||
        slice >= std::max(1u, info.depth >> mip))
        throw std::invalid_argument("Texture point selection exceeds subresource bounds");
    const auto format = info.format;
    const bool ordinary = (format >= 1 && format <= 18) || (format >= 23 && format <= 39) ||
                          (format >= 41 && format <= 43) || (format >= 48 && format <= 54) ||
                          (format >= 56 && format <= 65) || format == 67 || (format >= 85 && format <= 93) ||
                          format == 115;
    // Depth and multisampled resources require whole-subresource copies. Packed,
    // compressed and planar layouts retain their established storage readers.
    if (!ordinary || info.samples != 1 || (info.dimension == 4 && featureLevel() < D3D_FEATURE_LEVEL_10_0) ||
        (resource.desc.at(resource.desc.size() - 3) & D3D11_BIND_DEPTH_STENCIL))
        return std::nullopt;
    requireResourceLod(id);
    auto *source = get<ID3D11Resource>(id);
    if (!pointReadbackStaging_ || pointReadbackFormat_ != format ||
        pointReadbackDimension_ != info.dimension) {
        Com<ID3D11Resource> staging;
        if (info.dimension == 2) {
            D3D11_TEXTURE1D_DESC d{
                1, 1, 1, DXGI_FORMAT(format), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            Com<ID3D11Texture1D> texture;
            check(device_->CreateTexture1D(&d, nullptr, &texture), "Create point readback staging");
            staging = texture;
        } else if (info.dimension == 4) {
            D3D11_TEXTURE3D_DESC d{
                1, 1, 1, 1, DXGI_FORMAT(format), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            Com<ID3D11Texture3D> texture;
            check(device_->CreateTexture3D(&d, nullptr, &texture), "Create point readback staging");
            staging = texture;
        } else {
            D3D11_TEXTURE2D_DESC d{
                1, 1, 1, 1, DXGI_FORMAT(format), {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            Com<ID3D11Texture2D> texture;
            check(device_->CreateTexture2D(&d, nullptr, &texture), "Create point readback staging");
            staging = texture;
        }
        pointReadbackStaging_ = std::move(staging);
        pointReadbackFormat_ = format;
        pointReadbackDimension_ = info.dimension;
    }
    const D3D11_BOX box{x, y, slice, x + 1, y + 1, slice + 1};
    {
        Unpredicated unpredicated(context_.Get());
        InspectionCopyBindings isolation(context_.Get(), options_.warp);
        context_->CopySubresourceRegion(pointReadbackStaging_.Get(), 0, 0, 0, 0, source,
                                        mip + layer * info.mips, &box);
    }
    std::vector<uint8_t> bytes(pitches(1, 1, format).first);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(pointReadbackStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map point readback");
    std::memcpy(bytes.data(), mapped.pData, bytes.size());
    context_->Unmap(pointReadbackStaging_.Get(), 0);
    return bytes;
}
std::vector<uint8_t> Replay::readTexture(Id id) {
    const auto resource = frame_.resource(id);
    return readTextureStorage(get<ID3D11Resource>(id), resource);
}
NativeTextureMetadata Replay::observeTextureMetadata(Id id) {
    auto *source = get<ID3D11Resource>(id);
    NativeTextureMetadata result;
    result.resource = frame_.resource(id);
    D3D11_RESOURCE_DIMENSION dimension{};
    source->GetType(&dimension);
    auto describe = [&]<class T, class D>(uint16_t type) {
        Com<T> texture;
        check(source->QueryInterface(IID_PPV_ARGS(&texture)), "Observe native texture type");
        D desc{};
        texture->GetDesc(&desc);
        result.resource.type = type;
        result.resource.desc.resize(sizeof desc / sizeof(uint32_t));
        std::memcpy(result.resource.desc.data(), &desc, sizeof desc);
    };
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        describe.operator()<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>(0x84);
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        describe.operator()<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>(0x85);
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        describe.operator()<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>(0x86);
    else
        throw std::runtime_error("Native texture observation requires a texture resource");
    requireResourceLod(id);
    if (hasResourceLodClamp(result.resource))
        result.minLod = context_->GetResourceMinLOD(source);
    return result;
}
NativeTextureStorage Replay::observeTextureStorage(Id id, uint64_t maxBytes, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    auto metadata = observeTextureMetadata(id);
    NativeTextureStorage result{std::move(metadata.resource), {}, metadata.minLod};
    if (textureInfo(result.resource).samples != 1)
        throw std::runtime_error("Native texture observation requires non-MSAA storage");
    const auto layout = textureSubresources(result.resource);
    const auto size = layout.back().offset + layout.back().size;
    if (!maxBytes || size > maxBytes)
        throw std::runtime_error("Native texture observation byte budget exceeded");
    result.bytes = readTextureStorage(get<ID3D11Resource>(id), result.resource, cancelled);
    checkCancellation(cancelled);
    if (result.bytes.size() != size)
        throw std::runtime_error("Native texture observation size changed");
    return result;
}
std::vector<uint8_t> Replay::readTextureStorage(ID3D11Resource *source, const Resource &resource,
                                                const CancelCheck &cancelled) {
    checkCancellation(cancelled);
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
            checkCancellation(cancelled);
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
            if (cancelled)
                context_->Flush();
            for (;;) {
                checkCancellation(cancelled);
                const auto hr = context_->Map(staging.Get(), sub, D3D11_MAP_READ,
                                              cancelled ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &mapped);
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                    check(hr, "Map texture storage");
                    break;
                }
                check(device_->GetDeviceRemovedReason(), "Texture storage readback device status");
                Sleep(1);
            }
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
            checkCancellation(cancelled);
        }
    return result;
}
} // namespace flora
