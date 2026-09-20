#include "InspectionCopyBindings.h"
#include "Replay.h"
#include <algorithm>

namespace flora {
Com<ID3D11Resource> Replay::createEditTexture(const Resource &resource, std::optional<Bytes> data) {
    const auto subs = textureSubresources(resource);
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
    if (data) {
        if (textureInfo(resource).samples != 1 || data->size() != subs.back().offset + subs.back().size)
            throw std::runtime_error("Edited texture storage byte count mismatch");
        for (const auto &sub : subs) {
            if (sub.slicePitch > UINT32_MAX)
                throw std::runtime_error("Edited texture slice pitch overflow");
            initial.push_back({data->data() + size_t(sub.offset), sub.rowPitch, UINT(sub.slicePitch)});
        }
    }
    Reader desc(Bytes(reinterpret_cast<const uint8_t *>(resource.desc.data()), resource.desc.size() * 4));
    const auto init = data ? initial.data() : nullptr;
    if (resource.type == 0x84) {
        auto d = desc.read<D3D11_TEXTURE1D_DESC>();
        desc.end();
        Com<ID3D11Texture1D> object;
        check(device_->CreateTexture1D(&d, init, &object), "Create edited Texture1D");
        return object;
    }
    if (resource.type == 0x86) {
        auto d = desc.read<D3D11_TEXTURE3D_DESC>();
        desc.end();
        Com<ID3D11Texture3D> object;
        check(device_->CreateTexture3D(&d, init, &object), "Create edited Texture3D");
        return object;
    }
    auto d = desc.read<D3D11_TEXTURE2D_DESC>();
    desc.end();
    Com<ID3D11Texture2D> object;
    check(device_->CreateTexture2D(&d, init, &object), "Create edited Texture2D");
    return object;
}
void Replay::applyTextureEdits(const Event &event, const State &state, std::map<Id, Com<IUnknown>> &originals,
                               std::vector<std::pair<Com<ID3D11Resource>, Com<ID3D11Resource>>> &backups) {
    // Output preconditions precede input cloning, including when both edit the same resource.
    if (auto edits = options_.textureOutputs.find(event.id); edits != options_.textureOutputs.end())
        for (const auto &[id, patches] : edits->second) {
            const auto resource = frame_.resource(id);
            for (const auto &patch : patches) {
                validateTexturePatch(resource, patch);
                validateTextureBinding(frame_, event, state, id, patch, true);
            }
            Com<ID3D11Resource> target = get<ID3D11Resource>(id);
            auto backupResource = resource;
            auto &d = backupResource.desc;
            d[d.size() - 4] = D3D11_USAGE_DEFAULT;
            d[d.size() - 2] = 0;
            auto backup = createEditTexture(backupResource);
            {
                InspectionCopyBindings bindings(context_.Get(), options_.warp);
                context_->CopyResource(backup.Get(), target.Get());
            }
            backups.emplace_back(target, backup);
            for (const auto &patch : patches) {
                if (textureInfo(resource).samples > 1) {
                    writeMsaaSample(target.Get(), resource, patch);
                    continue;
                }
                const auto sub = validateTexturePatch(resource, patch);
                Resource upload = resource;
                const auto info = textureInfo(resource);
                const auto bind = d[d.size() - 3];
                if (info.dimension == 2)
                    upload.desc = {sub.width, 1, 1, info.format, 0, bind, 0, 0};
                else if (info.dimension == 4)
                    upload.desc = {sub.width, sub.height, sub.depth, 1, info.format, 0, bind, 0, 0};
                else
                    upload.desc = {sub.width, sub.height, 1, 1, info.format, 1, 0, 0, bind, 0, 0};
                auto source = createEditTexture(upload, Bytes(patch.bytes));
                InspectionCopyBindings bindings(context_.Get(), options_.warp);
                context_->CopySubresourceRegion(target.Get(), sub.index, 0, 0, 0, source.Get(), 0, nullptr);
            }
        }
    if (auto edits = options_.textureInputs.find(event.id); edits != options_.textureInputs.end())
        for (const auto &[id, patches] : edits->second) {
            const auto resource = frame_.resource(id);
            const auto info = textureInfo(resource);
            for (const auto &patch : patches) {
                validateTexturePatch(resource, patch);
                validateTextureBinding(frame_, event, state, id, patch, false);
            }
            Com<ID3D11Resource> target = get<ID3D11Resource>(id);
            auto cloneResource = resource;
            auto &d = cloneResource.desc;
            d[d.size() - 4] = D3D11_USAGE_DEFAULT;
            d[d.size() - 3] = D3D11_BIND_SHADER_RESOURCE |
                              (info.samples > 1 ? d[d.size() - 3] & D3D11_BIND_DEPTH_STENCIL : 0);
            d[d.size() - 2] = 0;
            d.back() = info.samples > 1 ? 0 : d.back() & D3D11_RESOURCE_MISC_TEXTURECUBE;
            Com<ID3D11Resource> clone;
            if (info.samples == 1) {
                auto bytes = readTextureStorage(target.Get(), resource);
                for (const auto &patch : patches) {
                    const auto sub = validateTexturePatch(resource, patch);
                    std::copy(patch.bytes.begin(), patch.bytes.end(), bytes.begin() + size_t(sub.offset));
                }
                clone = createEditTexture(cloneResource, Bytes(bytes));
            } else {
                clone = createEditTexture(cloneResource);
                {
                    InspectionCopyBindings bindings(context_.Get(), options_.warp);
                    context_->CopyResource(clone.Get(), target.Get());
                }
                for (const auto &patch : patches)
                    writeMsaaSample(clone.Get(), cloneResource, patch);
            }
            // Keep output and inactive-pipeline aliases attached to original storage.
            std::set<Id> aliases{state.dsv};
            for (auto &stage : state.stages)
                aliases.insert(stage.srv.begin(), stage.srv.end());
            aliases.insert(state.rtv.begin(), state.rtv.end());
            aliases.insert(state.csUav.begin(), state.csUav.end());
            aliases.insert(state.omExtended.begin(), state.omExtended.end());
            aliases.insert(state.csExtended.begin(), state.csExtended.end());
            for (auto view : aliases)
                if (view) {
                    Reader reader(frame_.payload(view, 5));
                    reader.skip(16);
                    if (reader.read<Id>() == id)
                        object(view);
                }
            originals.emplace(id, target);
            objects_.at(id) = clone;
            for (auto view : textureInputViews(frame_, event, state, id)) {
                originals.emplace(view, objects_.at(view));
                Reader reader(frame_.payload(view, 5, 0x8c));
                reader.skip(24);
                auto desc = reader.read<D3D11_SHADER_RESOURCE_VIEW_DESC>();
                reader.end();
                Com<ID3D11ShaderResourceView> replacement;
                check(device_->CreateShaderResourceView(clone.Get(), &desc, &replacement),
                      "Create edited texture SRV");
                objects_.at(view) = replacement;
            }
        }
}
} // namespace flora
