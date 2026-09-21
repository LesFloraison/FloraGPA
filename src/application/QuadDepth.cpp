#include "QuadDepth.h"
#include "FrameOutput.h"
#include "QuadBindings.h"
#include "replay/InspectionCopyBindings.h"
#include "replay/Unpredicated.h"
namespace flora {
void QuadDepth::suspendStreamOutput() { r.unbindStreamOutput(); }
std::string QuadPreparedDepth::digest(Replay &replay) const {
    return sha256(QuadResources(replay).storage(object.Get(), resource));
}
QuadPreparedDepth QuadDepth::prepare(const Event &event, const State &state, const QuadTarget &target,
                                     const std::function<uint64_t()> &submit, bool copyStreamOutput) {
    auto context = r.context_.Get();
    Com<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    if (!ps)
        throw std::runtime_error("Recovered phase-zero helper is gated off when the original PS is null");
    QuadPreparedDepth result;
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    std::string stencil;
    if (state.dsv) {
        const auto original = r.get<ID3D11DepthStencilView>(state.dsv);
        Com<ID3D11Resource> owner;
        original->GetResource(&owner);
        D3D11_RESOURCE_DIMENSION dimension;
        owner->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
            Com<ID3D11Texture1D> texture;
            check(owner.As(&texture), "Query Quad depth texture");
            D3D11_TEXTURE1D_DESC desc;
            texture->GetDesc(&desc);
            result.resource.type = 0x84;
            result.resource.desc.resize(sizeof desc / 4);
            std::memcpy(result.resource.desc.data(), &desc, sizeof desc);
        } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            Com<ID3D11Texture2D> texture;
            check(owner.As(&texture), "Query Quad depth texture");
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            result.resource.type = 0x85;
            result.resource.desc.resize(sizeof desc / 4);
            std::memcpy(result.resource.desc.data(), &desc, sizeof desc);
        } else
            throw std::runtime_error("Unsupported Quad depth resource dimension");
        original->GetDesc(&view);
        const auto selected = outputSubresource(
            result.resource, {reinterpret_cast<const uint8_t *>(&view), sizeof view}, {}, true);
        if (selected.at("width") != target.width || selected.at("height") != target.height)
            throw std::runtime_error("Prepared depth extent must match the selected diagnostic target");
        auto &desc = result.resource.desc;
        desc[desc.size() - 4] = D3D11_USAGE_DEFAULT;
        desc[desc.size() - 2] = 0;
        result.object = r.createEditTexture(result.resource);
        {
            Unpredicated guard(context);
            InspectionCopyBindings bindings(context, r.options_.warp);
            context->CopyResource(result.object.Get(), owner.Get());
        }
        view.Flags = 0;
        stencil = "copied from the original bound depth resource; not recovered GPA initialization";
    } else {
        const auto dimension =
            target.selected.is_null() ? 0u : target.selected.at("dimension").get<uint32_t>();
        const auto layers =
            target.selected.is_null() ? 1u : target.selected.at("layer_count").get<uint32_t>();
        const auto samples = target.metadata.value("samples", 1u);
        const auto quality = target.metadata.value("sample_quality", 0u);
        const bool array = dimension == 3 || dimension == 5 || dimension == 7 || dimension == 8;
        view.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        if (dimension == 2 || dimension == 3) {
            result.resource.type = 0x84;
            result.resource.desc = {target.width, 1, layers, 45, 0, 64, 0, 0};
            view.ViewDimension = array ? D3D11_DSV_DIMENSION_TEXTURE1DARRAY : D3D11_DSV_DIMENSION_TEXTURE1D;
            if (array)
                view.Texture1DArray.ArraySize = layers;
        } else {
            result.resource.type = 0x85;
            result.resource.desc = {
                target.width, target.height, 1, layers, 45, samples, quality, 0, 64, 0, 0};
            if (samples > 1) {
                view.ViewDimension =
                    array ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
                if (array)
                    view.Texture2DMSArray.ArraySize = layers;
            } else {
                view.ViewDimension =
                    array ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D;
                if (array)
                    view.Texture2DArray.ArraySize = layers;
            }
        }
        result.object = r.createEditTexture(result.resource);
        stencil = "explicitly initialized to zero in local D24S8 allocation; not recovered GPA format";
    }
    check(r.device_->CreateDepthStencilView(result.object.Get(), &view, &result.view),
          "Create Quad private DSV");
    {
        Unpredicated guard(context);
        if (!state.dsv)
            context->ClearDepthStencilView(result.view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f,
                                           0);
        context->ClearDepthStencilView(result.view.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
    }
    const auto cleared = result.digest(r);
    uint64_t submissions = 0;
    {
        QuadHighUavs preserve(context, state);
        r.withPrivateOutputs(
            event, state,
            [&] {
                r.bind(state, false);
                r.applyGraphicsEdits(event, state);
                context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, result.view.Get(), 0, 0,
                                                                   nullptr, nullptr);
                Com<ID3D11DepthStencilView> bound;
                context->OMGetRenderTargets(0, nullptr, &bound);
                if (bound != result.view)
                    throw std::runtime_error("Private phase-zero DSV bind rejected");
                submissions = submit();
            },
            copyStreamOutput);
    }
    std::array<uint32_t, 6> fields{};
    std::memcpy(fields.data(), &view, sizeof view);
    result.metadata = {{"strategy", "single_original_draw_on_cleared_private_depth"},
                       {"submissions", submissions},
                       {"clear_depth", 1.0},
                       {"stencil_basis", stencil},
                       {"resource_type", result.resource.type},
                       {"resource_desc", result.resource.desc},
                       {"view_desc", fields},
                       {"cleared_sha256", cleared},
                       {"prepared_sha256", result.digest(r)},
                       {"original_ps_preserved", true},
                       {"original_depth_stencil_state_preserved", true},
                       {"color_and_uav_outputs_removed", true},
                       {"scope", "Recovered helper behavior with local allocation and per-event scheduling"}};
    return result;
}
} // namespace flora
