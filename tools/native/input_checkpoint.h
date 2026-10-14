#pragma once
// Development-only input copies. No Map/Flush/wait until application replay completes.
#include "texel_checkpoint.h"
#include <nlohmann/json.hpp>

namespace flora::diagnostic {
class InputCheckpoint {
    using Json = nlohmann::json;
    struct Copy {
        Id id{};
        Com<ID3D11Resource> staging;
        Resource description;
        uint64_t bytes{};
        Json metadata;
    };
    std::vector<Copy> copies_;
    Json bindings_ = Json::object();
    bool captured_ = false;
public:
    void capture(ID3D11Device* device, ID3D11DeviceContext* context,
                 const std::map<Id, Com<IUnknown>>& objects) {
        if (captured_) throw std::runtime_error("Input checkpoint captured twice");
        captured_ = true;
        requireUnpredicated(context);
        std::map<IUnknown*, Id> ids;
        auto identity = [](IUnknown* object) {
            Com<IUnknown> result;
            if (object) check(object->QueryInterface(IID_PPV_ARGS(&result)), "Get input identity");
            return result;
        };
        for (const auto& [id, object] : objects)
            ids.try_emplace(identity(object.Get()).Get(), id);
        auto idOf = [&](IUnknown* object) -> Id {
            if (!object) return 0;
            const auto found = ids.find(identity(object).Get());
            if (found == ids.end()) throw std::runtime_error("Input has no captured identity");
            return found->second;
        };
        std::map<Id, Com<ID3D11Resource>> inputs;
        auto retain = [&](ID3D11Resource* resource) {
            const auto id = idOf(resource);
            if (id) inputs.try_emplace(id, resource);
            return id;
        };
        Com<ID3D11DeviceContext1> context1;
        check(context->QueryInterface(IID_PPV_ARGS(&context1)), "Get input CB ranges");
        for (const unsigned stage : {0u, 4u}) {
            const auto name = stage == 0 ? "vs" : "ps";
            ID3D11Buffer* raw[14]{}; UINT first[14]{}, count[14]{};
            if (!stage) context1->VSGetConstantBuffers1(0, 14, raw, first, count);
            else context1->PSGetConstantBuffers1(0, 14, raw, first, count);
            for (unsigned slot = 0; slot < 14; ++slot) {
                Com<ID3D11Buffer> buffer; buffer.Attach(raw[slot]);
                bindings_[name]["cb"].push_back({{"resource", retain(buffer.Get())}, {"first", first[slot]}, {"count", count[slot]}});
            }
            ID3D11ShaderResourceView* views[128]{};
            if (!stage) context->VSGetShaderResources(0, 128, views);
            else context->PSGetShaderResources(0, 128, views);
            for (auto* rawView : views) {
                Com<ID3D11ShaderResourceView> view; view.Attach(rawView);
                if (!view) { bindings_[name]["srv"].push_back(nullptr); continue; }
                Com<ID3D11Resource> resource; view->GetResource(&resource);
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; view->GetDesc(&desc);
                std::array<uint32_t, sizeof desc / 4> words{}; std::memcpy(words.data(), &desc, sizeof desc);
                bindings_[name]["srv"].push_back({{"view", idOf(view.Get())}, {"resource", retain(resource.Get())}, {"desc", words}});
            }
        }
        ID3D11Buffer* vertex[32]{}; UINT strides[32]{}, offsets[32]{};
        context->IAGetVertexBuffers(0, 32, vertex, strides, offsets);
        for (unsigned slot = 0; slot < 32; ++slot) {
            Com<ID3D11Buffer> buffer; buffer.Attach(vertex[slot]);
            bindings_["vb"].push_back({{"resource", retain(buffer.Get())}, {"stride", strides[slot]}, {"offset", offsets[slot]}});
        }
        Com<ID3D11Buffer> index; DXGI_FORMAT format{}; UINT offset{};
        context->IAGetIndexBuffer(&index, &format, &offset);
        bindings_["ib"] = {{"resource", retain(index.Get())}, {"format", format}, {"offset", offset}};
        // Validate every selected resource and total storage before enqueueing any copy.
        uint64_t total = 0;
        constexpr uint64_t budget = uint64_t(512) << 20;
        for (const auto& [id, source] : inputs) {
            Copy copy; copy.id = id; copy.description.id = id;
            Com<ID3D11Buffer> buffer; Com<ID3D11Texture2D> texture;
            if (SUCCEEDED(source.As(&buffer))) {
                D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
                std::array<uint32_t, sizeof desc / 4> words{}; std::memcpy(words.data(), &desc, sizeof desc);
                copy.bytes = desc.ByteWidth; copy.metadata = {{"kind", "buffer"}, {"desc", words}};
            } else if (SUCCEEDED(source.As(&texture))) {
                copy.description.type = 0x85;
                D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                if (desc.SampleDesc.Count != 1) throw std::runtime_error("Input checkpoint does not support MSAA SRVs");
                copy.description.desc.resize(sizeof desc / 4); std::memcpy(copy.description.desc.data(), &desc, sizeof desc);
                for (const auto& sub : textureSubresources(copy.description)) copy.bytes += sub.size;
                const auto minLod = (desc.MiscFlags & D3D11_RESOURCE_MISC_RESOURCE_CLAMP) ? context->GetResourceMinLOD(source.Get()) : 0.0f;
                copy.metadata = {{"kind", "texture2d"}, {"desc", copy.description.desc}, {"min_lod", minLod}};
            } else throw std::runtime_error("Input checkpoint requires buffer or single-sample Texture2D");
            if (copy.bytes > budget - total) throw std::runtime_error("Input checkpoint exceeds 512 MiB");
            total += copy.bytes; copies_.push_back(std::move(copy));
        }
        for (auto& copy : copies_) {
            const auto& source = inputs.at(copy.id);
            if (copy.metadata["kind"] == "buffer") {
                D3D11_BUFFER_DESC desc{}; desc.ByteWidth = UINT(copy.bytes);
                desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                Com<ID3D11Buffer> staging;
                check(device->CreateBuffer(&desc, nullptr, &staging), "Create input buffer checkpoint");
                copy.staging = staging;
            } else {
                D3D11_TEXTURE2D_DESC desc{}; std::memcpy(&desc, copy.description.desc.data(), sizeof desc);
                desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                Com<ID3D11Texture2D> staging;
                check(device->CreateTexture2D(&desc, nullptr, &staging), "Create input texture checkpoint");
                copy.staging = staging;
            }
            context->CopyResource(copy.staging.Get(), source.Get());
        }
    }
    Json read(ID3D11Device* device) const {
        if (!captured_) throw std::runtime_error("Input checkpoint was not captured");
        Json result{{"scope", "native VS/PS CB and SRV resources, IA VB and IB"},
                    {"all_draw_inputs_proven_equal", false}, {"bindings", bindings_}, {"resources", Json::array()}};
        Com<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
        for (const auto& copy : copies_) {
            std::vector<uint8_t> bytes(size_t(copy.bytes));
            if (copy.metadata["kind"] == "buffer") {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(copy.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read input buffer checkpoint");
                const bool valid = mapped.pData != nullptr;
                if (valid) std::memcpy(bytes.data(), mapped.pData, bytes.size());
                context->Unmap(copy.staging.Get(), 0);
                if (!valid) throw std::runtime_error("Null input buffer mapping");
            } else {
                for (const auto& sub : textureSubresources(copy.description)) {
                    const auto index = sub.layer * copy.description.desc[2] + sub.mip;
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    check(context->Map(copy.staging.Get(), index, D3D11_MAP_READ, 0, &mapped), "Read input texture checkpoint");
                    const bool valid = mapped.pData && mapped.RowPitch >= sub.rowPitch &&
                                       sub.offset <= bytes.size() && sub.size <= bytes.size() - sub.offset;
                    if (valid)
                        for (uint64_t row = 0; row < sub.slicePitch / sub.rowPitch; ++row)
                            std::memcpy(bytes.data() + sub.offset + row * sub.rowPitch,
                                        static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch, size_t(sub.rowPitch));
                    context->Unmap(copy.staging.Get(), index);
                    if (!valid) throw std::runtime_error("Invalid input texture mapping");
                }
            }
            auto metadata = copy.metadata;
            metadata.update({{"resource", copy.id}, {"bytes", bytes.size()}, {"sha256", sha256(bytes)}});
            result["resources"].push_back(std::move(metadata));
        }
        check(device->GetDeviceRemovedReason(), "Input checkpoint device status");
        return result;
    }
};
} // namespace flora::diagnostic
