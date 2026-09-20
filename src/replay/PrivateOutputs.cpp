#include "InspectionCopyBindings.h"
#include "Replay.h"
#include "Unpredicated.h"
#include <set>
namespace flora {
void Replay::withPrivateOutputs(const Event &event, const State &state,
                                const std::function<void()> &inspect) {
    std::vector<Id> om(state.rtv.begin(), state.rtv.end());
    om.insert(om.end(), state.omExtended.begin(), state.omExtended.end());
    if (state.rtCount > om.size())
        throw std::runtime_error("Private output view count exceeds capture storage");
    std::set<Id> outputs(om.begin(), om.begin() + state.rtCount);
    outputs.insert(state.dsv);
    outputs.erase(0);
    std::set<Id> views = outputs;
    views.insert(state.csUav.begin(), state.csUav.end());
    views.insert(state.csExtended.begin(), state.csExtended.end());
    for (const auto &stage : state.stages)
        if (stage.shader && !passthroughShaders_.contains(stage.shader))
            views.insert(stage.srv.begin(), stage.srv.end());
    views.erase(0);
    std::map<Id, Com<ID3D11Resource>> owners;
    std::map<ID3D11Resource *, Com<ID3D11Resource>> clones;
    std::map<Id, Com<IUnknown>> originals;
    std::map<Id, uint32_t> counters;
    auto restore = [&] {
        for (auto &[id, original] : originals)
            objects_[id] = original;
        bind(state, false);
        applyGraphicsEdits(event, state);
    };
    try {
        for (auto id : views) {
            auto view = get<ID3D11View>(id);
            view->GetResource(&owners[id]);
        }
        for (auto id : outputs) {
            auto source = owners.at(id);
            if (!clones.contains(source.Get())) {
                D3D11_RESOURCE_DIMENSION kind;
                source->GetType(&kind);
                Com<ID3D11Resource> clone;
#define CLONE_RESOURCE(Kind, Type, Desc, Create)                                                             \
    case Kind: {                                                                                             \
        Com<Type> typed;                                                                                     \
        check(source.As(&typed), "Query private resource");                                                  \
        Desc desc;                                                                                           \
        typed->GetDesc(&desc);                                                                               \
        desc.Usage = D3D11_USAGE_DEFAULT;                                                                    \
        desc.CPUAccessFlags = 0;                                                                             \
        Com<Type> target;                                                                                    \
        check(device_->Create(&desc, nullptr, &target), "Create private output resource");                   \
        clone = target;                                                                                      \
        break;                                                                                               \
    }
                switch (kind) {
                    CLONE_RESOURCE(D3D11_RESOURCE_DIMENSION_BUFFER, ID3D11Buffer, D3D11_BUFFER_DESC,
                                   CreateBuffer)
                    CLONE_RESOURCE(D3D11_RESOURCE_DIMENSION_TEXTURE1D, ID3D11Texture1D, D3D11_TEXTURE1D_DESC,
                                   CreateTexture1D)
                    CLONE_RESOURCE(D3D11_RESOURCE_DIMENSION_TEXTURE2D, ID3D11Texture2D, D3D11_TEXTURE2D_DESC,
                                   CreateTexture2D)
                    CLONE_RESOURCE(D3D11_RESOURCE_DIMENSION_TEXTURE3D, ID3D11Texture3D, D3D11_TEXTURE3D_DESC,
                                   CreateTexture3D)
                default:
                    throw std::runtime_error("Unsupported private resource dimension");
                }
#undef CLONE_RESOURCE
                {
                    Unpredicated guard(context_.Get());
                    InspectionCopyBindings bindings(context_.Get(), options_.warp);
                    context_->CopyResource(clone.Get(), source.Get());
                }
                clones.emplace(source.Get(), clone);
            }
            if (frame_.entry(id).type == 0x8f) {
                D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
                get<ID3D11UnorderedAccessView>(id)->GetDesc(&desc);
                if (desc.ViewDimension == D3D11_UAV_DIMENSION_BUFFER && (desc.Buffer.Flags & 6))
                    counters[id] = readCounter(id);
            }
        }
        // Key by actual COM resource identity: an input edit can already have
        // replaced a resource-ID cache entry while an output view still owns
        // the original storage.
        for (auto &[id, object] : objects_) {
            Com<ID3D11Resource> resource;
            if (SUCCEEDED(object.As(&resource)))
                if (auto clone = clones.find(resource.Get()); clone != clones.end()) {
                    originals.emplace(id, object);
                    object = clone->second;
                }
        }
        for (const auto &[id, owner] : owners) {
            const auto clone = clones.find(owner.Get());
            if (clone == clones.end())
                continue;
            auto original = objects_.at(id);
            originals.emplace(id, original);
#define CLONE_VIEW(Tag, Type, Desc, Create)                                                                  \
    case Tag: {                                                                                              \
        Com<Type> view;                                                                                      \
        check(original.As(&view), "Query original output alias");                                            \
        Desc desc;                                                                                           \
        view->GetDesc(&desc);                                                                                \
        Com<Type> copy;                                                                                      \
        check(device_->Create(clone->second.Get(), &desc, &copy), "Create private output alias");            \
        objects_[id] = copy;                                                                                 \
        break;                                                                                               \
    }
            switch (frame_.entry(id).type) {
                CLONE_VIEW(0x8c, ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC,
                           CreateShaderResourceView)
                CLONE_VIEW(0x8d, ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC,
                           CreateRenderTargetView)
                CLONE_VIEW(0x8e, ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC,
                           CreateDepthStencilView)
                CLONE_VIEW(0x8f, ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC,
                           CreateUnorderedAccessView)
            default:
                throw std::runtime_error("Unsupported private output view");
            }
#undef CLONE_VIEW
        }
        {
            PredicateIsolation guard(*this);
            for (const auto &[id, count] : counters)
                writeCounter(id, count);
            inspect();
        }
    } catch (...) {
        restore();
        throw;
    }
    // The diagnostic redirects/unbinds SO before every submission, retaining
    // original buffers and hidden cursors. Rebinding the unchanged state appends.
    restore();
}
} // namespace flora
