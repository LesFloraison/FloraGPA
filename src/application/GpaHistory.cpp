#include "GpaHistory.h"
#include "DxbcGpuCompletion.h"
#include <d3dcompiler.h>

namespace flora {
namespace {
using Json = nlohmann::json;
enum class Pass { Coverage, Shader, Depth, Stencil };

uint64_t diagnostic(const Replay &source, Id event, const NativeHistorySelection &selection,
                    Pass pass, const CancelCheck &cancelled) {
    auto options = source.options();
    options.until = event;
    options.before = false;
    options.prepareBeforeDraw = false;
    options.measurement.reset();
    options.timings = false;
    Replay replay(source.frame(), options);
    Com<ID3D11Query> query;
    Com<ID3D11PixelShader> marker;
    Com<ID3D11RasterizerState> raster;
    Com<ID3D11DepthStencilState> depth;
    Com<ID3D11BlendState> blend;
    uint64_t count = 0;
    bool observed = false;
    replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); },
        [&](Id current, bool after, ID3D11DeviceContext *context, const auto &) {
            if (current != event)
                return;
            checkCancellation(cancelled);
            if (after) {
                context->End(query.Get());
                dxbc_exec::GpuCompletion completion(replay.nativeDevice(), context, cancelled, 5000);
                completion.submit();
                completion.query(query.Get(), &count, sizeof(count));
                observed = true;
                return;
            }
            Com<ID3D11RasterizerState> originalRaster;
            context->RSGetState(&originalRaster);
            D3D11_RASTERIZER_DESC rd = CD3D11_RASTERIZER_DESC(D3D11_DEFAULT);
            if (originalRaster)
                originalRaster->GetDesc(&rd);
            rd.ScissorEnable = TRUE;
            check(replay.nativeDevice()->CreateRasterizerState(&rd, &raster), "Create GPA diagnostic rasterizer");
            UINT viewportCount = 0;
            context->RSGetViewports(&viewportCount, nullptr);
            const D3D11_RECT point{LONG(selection.x), LONG(selection.y), LONG(selection.x + 1), LONG(selection.y + 1)};
            std::vector<D3D11_RECT> scissors(viewportCount, point);
            context->RSSetState(raster.Get());
            context->RSSetScissorRects(viewportCount, scissors.data());

            Com<ID3D11DepthStencilState> originalDepth;
            UINT reference = 0;
            context->OMGetDepthStencilState(&originalDepth, &reference);
            D3D11_DEPTH_STENCIL_DESC dd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
            if (originalDepth)
                originalDepth->GetDesc(&dd);
            if (pass != Pass::Depth)
                dd.DepthEnable = FALSE;
            if (pass != Pass::Stencil)
                dd.StencilEnable = FALSE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.StencilWriteMask = 0;
            check(replay.nativeDevice()->CreateDepthStencilState(&dd, &depth), "Create GPA diagnostic depth state");
            context->OMSetDepthStencilState(depth.Get(), reference);
            D3D11_BLEND_DESC bd = CD3D11_BLEND_DESC(D3D11_DEFAULT);
            for (auto &target : bd.RenderTarget)
                target.RenderTargetWriteMask = 0;
            check(replay.nativeDevice()->CreateBlendState(&bd, &blend), "Create GPA diagnostic blend state");
            // Original GPA diagnostics replace the scissor and sample mask. These
            // counts deliberately do not describe original-fragment eligibility.
            context->OMSetBlendState(blend.Get(), nullptr, UINT_MAX);
            if (pass != Pass::Shader) {
                constexpr char hlsl[] = "float4 main():SV_Target { return 1; }";
                Com<ID3DBlob> code, errors;
                check(D3DCompile(hlsl, sizeof(hlsl) - 1, "GPA event diagnostic", nullptr, nullptr,
                                 "main", "ps_5_0", 0, 0, &code, &errors), "Compile GPA diagnostic marker");
                check(replay.nativeDevice()->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                               nullptr, &marker), "Create GPA diagnostic marker");
                context->PSSetShader(marker.Get(), nullptr, 0);
            }
            const D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
            check(replay.nativeDevice()->CreateQuery(&qd, &query), "Create GPA diagnostic query");
            context->Begin(query.Get());
        });
    if (!observed)
        throw std::runtime_error("GPA diagnostic event was not reached");
    return count;
}
}

Json collectGpaEventHistory(Replay &replay, const NativeHistorySelection &selection,
                            const CancelCheck &cancelled) {
    const auto info = textureInfo(replay.frame().resource(selection.resource));
    if (info.dimension != 3 || info.samples != 1 || selection.layer || selection.slice ||
        selection.x >= INT32_MAX || selection.y >= INT32_MAX)
        throw std::invalid_argument("GPA diagnostics currently require a single-sample 2D target, layer zero");
    auto result = collectNativeEventHistory(replay, selection, cancelled);
    Json records = Json::array();
    uint64_t replays = 1;
    for (const auto &snapshot : result.at("records")) {
        checkCancellation(cancelled);
        const auto event = snapshot.at("event").get<Id>();
        const auto type = replay.frame().entry(event).type;
        if (type < 0x37 || type > 0x3d)
            continue;
        if (!snapshot.at("footprint").contains("outputs"))
            continue;
        bool color = false;
        for (const auto &output : snapshot.at("footprint").at("outputs"))
            color |= output.at("kind") == "rtv";
        if (!color)
            throw std::runtime_error("GPA diagnostic depth-only targets are not yet validated");
        const auto count = diagnostic(replay, event, selection, Pass::Coverage, cancelled);
        ++replays;
        if (!count)
            continue;
        const auto shader = diagnostic(replay, event, selection, Pass::Shader, cancelled);
        const auto depth = diagnostic(replay, event, selection, Pass::Depth, cancelled);
        const auto stencil = diagnostic(replay, event, selection, Pass::Stencil, cancelled);
        replays += 3;
        const uint32_t status = !shader ? 5 : !depth ? 2 : !stencil ? 3 : 0;
        records.push_back({{"record_kind", "gpa_event_diagnostic"}, {"event", event},
                           {"api", snapshot.at("api")}, {"pixel", snapshot.at("after")},
                           {"diagnostic_status", status}, {"diagnostic_count", count},
                           {"observations", {{"shader_samples", shader}, {"depth_samples", depth},
                                              {"stencil_samples", stencil}}}});
    }
    return {{"schema", 1}, {"kind", "gpa_event_history"}, {"source", "native_dx11_replay"},
            {"selection", result.at("selection")}, {"records", records}, {"replay_count", replays},
            {"diagnostic_semantics", "counterfactual_scissor_and_sample_mask"},
            {"original_fragment_eligibility", nullptr},
            {"notes", {"One row per event; diagnostic counts do not describe actual pixel writes.",
                       "Pixel values are read after the unmodified event on separate storage.",
                       "The event diagnostic implementation requires differential validation against GPA."}}};
}
}
