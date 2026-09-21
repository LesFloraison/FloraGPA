#include "QuadResources.h"
#include "DxbcInspection.h"
#include "FrameOutput.h"
#include "core/StreamOutput.h"
#include "replay/Unpredicated.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
template <class T> Bytes bytes(const T &value) {
    return {reinterpret_cast<const uint8_t *>(&value), sizeof value};
}
} // namespace
QuadTarget QuadResources::select(const State &state, const std::string &target,
                                 std::optional<uint32_t> layer) {
    std::optional<uint32_t> color;
    const auto count = std::min({state.rtCount, state.omStart, 8u});
    if (target == "auto") {
        for (uint32_t i = 0; i < count; ++i)
            if (state.rtv[i]) {
                color = i;
                break;
            }
    } else if (target.size() == 3 && target.starts_with("rt") && target[2] >= '0' && target[2] <= '7') {
        color = uint32_t(target[2] - '0');
        if (*color >= count || !state.rtv[*color])
            throw std::runtime_error("Coverage target must name a bound RTV slot or depth/stencil view");
    } else if (target != "depth" || !state.dsv)
        throw std::runtime_error("Coverage target must name a bound RTV slot or depth/stencil view");
    const auto viewId = color ? state.rtv[*color] : state.dsv;
    QuadTarget result;
    if (!viewId) {
        result.metadata = {{"target_resource", nullptr},
                           {"target_view", nullptr},
                           {"target_slot", nullptr},
                           {"target_kind", "viewport"},
                           {"samples", 1},
                           {"sample_quality", 0},
                           {"requested_target", target},
                           {"array_index_selection", layer ? Json(*layer) : Json(nullptr)}};
        UINT n = 16;
        std::array<D3D11_VIEWPORT, 16> views{};
        r.context_->RSGetViewports(&n, views.data());
        double width = 1, height = 1;
        for (UINT i = 0; i < n; ++i) {
            const auto &v = views[i];
            for (const auto value : {v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth})
                if (!std::isfinite(value))
                    throw std::runtime_error(
                        "Viewport coverage requires finite bounds and nonnegative dimensions");
            if (v.Width < 0 || v.Height < 0)
                throw std::runtime_error(
                    "Viewport coverage requires finite bounds and nonnegative dimensions");
            width = std::max(width, std::ceil(double(v.TopLeftX) + v.Width));
            height = std::max(height, std::ceil(double(v.TopLeftY) + v.Height));
        }
        if (width > 16384 || height > 16384)
            throw std::runtime_error("Viewport coverage extent exceeds the DX11 texture limit");
        result.width = uint32_t(width);
        result.height = uint32_t(height);
        return result;
    }
    Reader record(r.frame_.payload(viewId));
    record.skip(16);
    const auto resourceId = record.read<Id>();
    const auto resource = r.frame_.resource(resourceId);
    std::array<ID3D11RenderTargetView *, 8> native{};
    Com<ID3D11DepthStencilView> nativeDepth;
    r.context_->OMGetRenderTargets(8, native.data(), &nativeDepth);
    const bool bound = color ? native[*color] == r.get<ID3D11RenderTargetView>(viewId)
                             : nativeDepth.Get() == r.get<ID3D11DepthStencilView>(viewId);
    for (auto view : native)
        if (view)
            view->Release();
    if (!bound)
        throw std::runtime_error(
            "Selected Quad output is not bound on the native context; incompatible output bindings");
    if (color) {
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        r.get<ID3D11RenderTargetView>(viewId)->GetDesc(&desc);
        result.selected = outputSubresource(resource, bytes(desc), layer, false);
    } else {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        r.get<ID3D11DepthStencilView>(viewId)->GetDesc(&desc);
        result.selected = outputSubresource(resource, bytes(desc), layer, true);
    }
    const auto dimension = result.selected.at("dimension").get<uint32_t>();
    const bool buffer = resource.type == 0x83;
    result.width = result.selected.at("width");
    result.height = result.selected.at("height");
    result.metadata = {
        {"target_resource", resourceId},
        {"target_view", viewId},
        {"target_slot", color ? Json(*color) : Json(nullptr)},
        {"target_kind", buffer  ? "buffer"
                        : color ? "color"
                                : "depth"},
        {"target_dimension", result.selected.at("resource_dimension")},
        {"coordinate_space", buffer ? "buffer_view_elements" : "texture_subresource"},
        {"target_buffer_view", buffer ? result.selected : Json(nullptr)},
        {"requested_target", target},
        {"samples", buffer ? 1u : textureInfo(resource).samples},
        {"sample_quality", resource.type == 0x85 ? resource.desc[6] : 0},
        {"array_index_selection", dimension == 3 || dimension == 5 || dimension == 7 || dimension == 8
                                      ? result.selected.at("relative_layer")
                                      : Json(nullptr)}};
    return result;
}
QuadRasterTarget QuadResources::dummy(const QuadTarget &target, bool preparedDepth) {
    QuadRasterTarget result;
    auto dimension = target.selected.is_null() ? 4u : target.selected.at("dimension").get<uint32_t>();
    const auto layers = target.selected.is_null() ? 1u : target.selected.at("layer_count").get<uint32_t>();
    auto samples = target.metadata.at("samples").get<uint32_t>();
    auto quality = target.metadata.at("sample_quality").get<uint32_t>();
    if (preparedDepth && (dimension == 1 || dimension == 8)) {
        dimension = dimension == 8 ? 5 : 4;
        samples = 1;
        quality = 0;
    }
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_RTV_DIMENSION(dimension);
    const auto width = target.width, height = target.height;
    if (!width || !height || !layers)
        throw std::runtime_error("Empty Quad raster target extent");
    if (dimension == 1) {
        if (width > UINT32_MAX / 4)
            throw std::runtime_error("Quad buffer target size overflow");
        result.resource.type = 0x83;
        result.resource.desc = {width * 4, 0, 32, 0, 0, 0};
        D3D11_BUFFER_DESC desc{};
        std::memcpy(&desc, result.resource.desc.data(), sizeof desc);
        Com<ID3D11Buffer> buffer;
        check(r.device_->CreateBuffer(&desc, nullptr, &buffer), "Create Quad dummy buffer");
        result.object = buffer;
        view.Buffer.NumElements = width;
    } else {
        if (dimension == 2 || dimension == 3) {
            result.resource.type = 0x84;
            result.resource.desc = {width, 1, layers, 28, 0, 32, 0, 0};
            if (dimension == 3)
                view.Texture1DArray.ArraySize = layers;
        } else if (dimension == 8) {
            result.resource.type = 0x86;
            result.resource.desc = {width, height, layers, 1, 28, 0, 32, 0, 0};
            view.Texture3D.WSize = layers;
        } else {
            result.resource.type = 0x85;
            result.resource.desc = {width, height, 1, layers, 28, samples, quality, 0, 32, 0, 0};
            if (dimension == 5)
                view.Texture2DArray.ArraySize = layers;
            if (dimension == 7)
                view.Texture2DMSArray.ArraySize = layers;
        }
        result.object = r.createEditTexture(result.resource);
    }
    check(r.device_->CreateRenderTargetView(result.object.Get(), &view, &result.view),
          "Create Quad dummy RTV");
    return result;
}
Json QuadResources::producer(const State &state) {
    for (const unsigned stage : {3u, 2u, 0u}) {
        const auto id = state.stages[stage].shader;
        if (!id || r.passthroughShaders_.contains(id))
            continue;
        const auto replacement = r.options_.shaders.find(id);
        const auto raw = replacement == r.options_.shaders.end() ? r.frame_.shader(r.frame_.resource(id).data)
                                                                 : Bytes(replacement->second);
        uint32_t stream = 0;
        if (stage == 3)
            if (const auto declaration = shaderStreamOutput(r.frame_, id))
                stream = readStreamOutputDeclaration(r.frame_, declaration).rasterizedStream;
        for (const auto &[tag, bytes] : readDxbcParts(raw))
            if (tag == 0x4e47534f || tag == 0x3547534f)
                for (const auto &field : dxbc_detail::signature(bytes, tag == 0x3547534f))
                    if (field.at("system_value") == 4 && field.value("stream", 0u) == stream)
                        return field;
        return nullptr;
    }
    return nullptr;
}
std::vector<uint8_t> QuadResources::storage(ID3D11Resource *object, const Resource &resource) {
    if (resource.type == 0x83) {
        Com<ID3D11Buffer> source;
        check(object->QueryInterface(IID_PPV_ARGS(&source)), "Quad buffer storage interface");
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;
        check(r.device_->CreateBuffer(&desc, nullptr, &staging), "Create Quad storage staging");
        Unpredicated guard(r.context_.Get());
        r.context_->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read Quad storage");
        std::vector<uint8_t> result;
        try {
            const auto data = static_cast<const uint8_t *>(mapped.pData);
            result.assign(data, data + desc.ByteWidth);
        } catch (...) {
            r.context_->Unmap(staging.Get(), 0);
            throw;
        }
        r.context_->Unmap(staging.Get(), 0);
        return result;
    }
    if (resource.type == 0x85 && resource.desc[5] > 1) {
        Replay::PredicateIsolation guard(r);
        std::vector<uint8_t> result;
        for (uint32_t sample = 0; sample < resource.desc[5]; ++sample) {
            const auto data = r.readMsaaStorage(object, resource, sample, resource.desc[4]);
            result.insert(result.end(), data.bytes.begin(), data.bytes.end());
        }
        return result;
    }
    return r.readTextureStorage(object, resource);
}
Json QuadResources::fingerprint(const State &state) {
    std::vector<Id> ids(state.rtv.begin(), state.rtv.end());
    ids.insert(ids.end(), state.omExtended.begin(), state.omExtended.end());
    if (state.rtCount > ids.size())
        throw std::runtime_error("Quad output view count exceeds capture storage");
    std::set<Id> views(ids.begin(), ids.begin() + state.rtCount);
    views.insert(state.dsv);
    views.erase(0);
    Json result = Json::object();
    auto record = [&](const std::string &key, ID3D11Resource *object, const Resource &resource) {
        const auto data = storage(object, resource);
        result[key] = {{"bytes", data.size()}, {"sha256", sha256(data)}};
    };
    for (const auto id : views) {
        Reader view(r.frame_.payload(id));
        view.skip(16);
        const auto resource = r.frame_.resource(view.read<Id>());
        Com<ID3D11Resource> owner;
        r.get<ID3D11View>(id)->GetResource(&owner);
        if (r.frame_.entry(id).type == 0x8f) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
            r.get<ID3D11UnorderedAccessView>(id)->GetDesc(&desc);
            if (desc.ViewDimension == D3D11_UAV_DIMENSION_BUFFER && (desc.Buffer.Flags & 6))
                result["counter:" + std::to_string(id)] = r.readCounter(id);
        }
        record(std::to_string(id), owner.Get(), resource);
    }
    // A prepared event has effective SO IDs in its snapshot. Read complete
    // storage without disturbing original native filled sizes or append cursors.
    if (state.soCount > 4)
        throw std::runtime_error("Quad SO target count exceeds four");
    for (uint32_t slot = 0; slot < state.soCount; ++slot)
        if (const auto id = state.so[slot])
            record("so:" + std::to_string(id), r.get<ID3D11Buffer>(id), r.frame_.resource(id));
    return result;
}
} // namespace flora
