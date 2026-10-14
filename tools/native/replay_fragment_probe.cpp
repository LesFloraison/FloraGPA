// Development-only unblended output observation at one original replay Draw boundary.
#include "replay/Replay.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
uint32_t number(const wchar_t *text) {
    const std::wstring value(text);
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error("Expected unsigned decimal argument");
    const auto n = std::stoull(value);
    if (n > UINT32_MAX)
        throw std::runtime_error("Argument overflow");
    return uint32_t(n);
}
std::vector<uint8_t> shader(const fs::path &path) {
    const auto size = fs::file_size(path);
    if (!size || size > 1048576)
        throw std::runtime_error("Expected bounded shader file");
    std::vector<uint8_t> result(size);
    std::ifstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.read(reinterpret_cast<char *>(result.data()), result.size());
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(result.data(), result.size(), IID_PPV_ARGS(&reflection)), "Reflect diagnostic shader");
    D3D11_SHADER_DESC desc{};
    check(reflection->GetDesc(&desc), "Describe diagnostic shader");
    if (D3D11_SHVER_GET_MAJOR(desc.Version) != 4 ||
        D3D11_SHVER_GET_TYPE(desc.Version) != D3D11_SHVER_PIXEL_SHADER)
        throw std::runtime_error("Expected shader model 4 pixel shader");
    return result;
}
Json messages(ID3D11InfoQueue *queue) {
    Json result = Json::array();
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
        SIZE_T size{};
        check(queue->GetMessage(i, nullptr, &size), "Get debug message size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read debug message");
        result.push_back({{"severity", message->Severity},
                          {"id", message->ID},
                          {"text", std::string(message->pDescription, message->DescriptionByteLength - 1)}});
    }
    queue->ClearStoredMessages();
    return result;
}
std::vector<uint8_t> readSample(ID3D11Texture2D *source, UINT sample, bool half) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != (half ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R32G32B32A32_TYPELESS) ||
        desc.ArraySize != 1 || desc.MipLevels != 1 ||
        sample >= desc.SampleDesc.Count)
        throw std::runtime_error("Unexpected private float texture");
    Com<ID3D11Device> device;
    source->GetDevice(&device);
    Com<ID3D11DeviceContext> context, deferred;
    device->GetImmediateContext(&context);
    check(device->CreateDeferredContext(0, &deferred), "Create float read context");
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> copy, target, staging;
    check(device->CreateTexture2D(&desc, nullptr, &copy), "Create float read copy");
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = half ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    Com<ID3D11ShaderResourceView> srv;
    check(device->CreateShaderResourceView(copy.Get(), &sd, &srv), "Create float bit SRV");
    desc.Format = sd.Format;
    desc.SampleDesc = {1, 0};
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&desc, nullptr, &target), "Create float bit target");
    Com<ID3D11RenderTargetView> rtv;
    check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create float bit RTV");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "Create float bit staging");
    auto compile = [&](const std::string &text, const char *profile) {
        Com<ID3DBlob> code, errors;
        check(D3DCompile(text.data(), text.size(), "fragment read", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors),
              "Compile float read shader");
        return code;
    };
    const auto vsCode = compile(
        "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const auto psCode = compile("Texture2DMS<uint4> t:register(t0);uint4 main(float4 "
                                "p:SV_Position):SV_Target{return t.Load(int2(p.xy)," +
                                    std::to_string(sample) + ");}",
                                "ps_5_0");
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs),
          "Create float read VS");
    check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps),
          "Create float read PS");
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    Com<ID3D11RasterizerState> raster;
    check(device->CreateRasterizerState(&rs, &raster), "Create float read RS");
    deferred->CopyResource(copy.Get(), source);
    deferred->VSSetShader(vs.Get(), nullptr, 0);
    deferred->PSSetShader(ps.Get(), nullptr, 0);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    deferred->RSSetState(raster.Get());
    deferred->RSSetViewports(1, &viewport);
    auto input = srv.Get();
    auto output = rtv.Get();
    deferred->PSSetShaderResources(0, 1, &input);
    deferred->OMSetRenderTargets(1, &output, nullptr);
    deferred->Draw(3, 0);
    deferred->CopyResource(staging.Get(), target.Get());
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish float read");
    context->ExecuteCommandList(commands.Get(), TRUE);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map float sample");
    const size_t row = size_t(desc.Width) * (half ? 8 : 16);
    if (mapped.RowPitch < row) {
        context->Unmap(staging.Get(), 0);
        throw std::runtime_error("Invalid float row pitch");
    }
    std::vector<uint8_t> bytes(row * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(bytes.data() + y * row,
                    static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch, row);
    context->Unmap(staging.Get(), 0);
    check(device->GetDeviceRemovedReason(), "Float read device status");
    return bytes;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc < 8 || argc > 10) {
        std::cerr << "FRAME NEW_OUT EVENT RTV0_RESOURCE X Y PCI_VENDOR [DIAGNOSTIC_PS_DXBC] [--unblended-rt0]\n";
        return 2;
    }
    Json report{{"completed", false},
                {"acceptance_established", false},
                {"transform", "Replace RTV0 with cleared RGBA32F and disable its blending"}};
    fs::path output;
    auto save = [&] {
        if (output.empty())
            return;
        std::ofstream f(output / "report.json");
        f.exceptions(std::ios::badbit | std::ios::failbit);
        f << report.dump(2) << '\n';
        f.close();
    };
    try {
        bool unblendedRt0 = false;
        fs::path replacementPath;
        for (int i = 8; i < argc; ++i) {
            const std::wstring option(argv[i]);
            if (option == L"--unblended-rt0" && !unblendedRt0)
                unblendedRt0 = true;
            else if (!option.starts_with(L"--") && replacementPath.empty())
                replacementPath = argv[i];
            else
                throw std::runtime_error("Unknown or duplicate option");
        }
        report["unblended_rt0"] = unblendedRt0;
        if (unblendedRt0)
            report["transform"] = "Disable RTV0 blending; preserve original RGBA16 resource and initial contents";
        const auto eid = number(argv[3]), rid = number(argv[4]), x = number(argv[5]), y = number(argv[6]),
                   vendor = number(argv[7]);
        if (!eid || !rid || !vendor || vendor > UINT16_MAX)
            throw std::runtime_error("Invalid event/resource/vendor");
        Frame frame(fs::absolute(argv[1]));
        const auto event = frame.event(eid);
        const auto state = frame.state(event.state);
        if (event.type != 0x39 || state.stages[1].shader || state.stages[2].shader ||
            state.stages[3].shader || state.soCount || state.predicate || !state.stages[0].shader ||
            !state.stages[4].shader)
            throw std::runtime_error("Expected VS/PS DrawIndexed without tessellation/GS/SO/predicate");
        auto resource = frame.resource(rid);
        if (resource.type != 0x85 || resource.desc.size() != 11 || resource.desc[2] != 1 ||
            resource.desc[3] != 1 || resource.desc[5] <= 1 || resource.desc[5] > 8 || resource.desc[6] != 0 ||
            x >= resource.desc[0] || y >= resource.desc[1])
            throw std::runtime_error("Expected in-bounds single-layer quality-zero MSAA output");
        if (unblendedRt0 && resource.desc[4] != DXGI_FORMAT_R16G16B16A16_TYPELESS)
            throw std::runtime_error("Unblended mode requires typeless RGBA16");
        std::vector<uint8_t> replacement;
        if (!replacementPath.empty())
            replacement = shader(replacementPath);
        const auto requested = fs::absolute(argv[2]);
        if (!fs::create_directory(requested))
            throw std::runtime_error("Output directory must be new");
        output = requested;
        report["event"] = eid;
        report["resource"] = rid;
        report["pixel"] = {x, y};
        report["frame_sha256"] = frame.sha256();
        report["original_ps"] = state.stages[4].shader;
        report["original_ps_sha256"] = sha256(frame.shader(frame.resource(state.stages[4].shader).data));
        report["diagnostic_ps_sha256"] = replacement.empty() ? Json(nullptr) : Json(sha256(replacement));
        save();
        ReplayOptions options;
        options.until = eid;
        options.vendor = vendor;
        options.debug = true;
        Replay replay(frame, options);
        report["adapter"] = replay.adapter();
        Com<ID3D11Texture2D> target;
        Com<ID3D11InfoQueue> debug;
        bool observed = false;
        replay.run({}, [&](Id id, bool after, ID3D11DeviceContext *context, const auto &objects) {
            if (id != eid || after)
                return;
            if (observed)
                throw std::runtime_error("Repeated draw boundary");
            observed = true;
            Com<ID3D11Device> device;
            context->GetDevice(&device);
            check(device.As(&debug), "Get debug queue");
            report["prefix_debug"] = messages(debug.Get());
            ID3D11RenderTargetView *rawViews[8]{};
            Com<ID3D11DepthStencilView> depth;
            context->OMGetRenderTargets(8, rawViews, &depth);
            std::array<Com<ID3D11RenderTargetView>, 8> views;
            for (UINT i = 0; i < 8; ++i)
                views[i].Attach(rawViews[i]);
            if (!views[0])
                throw std::runtime_error("RTV0 is unbound");
            Com<ID3D11Resource> bound;
            views[0]->GetResource(&bound);
            Com<IUnknown> a, b;
            check(bound.As(&a), "Get target identity");
            check(objects.at(rid).As(&b), "Get captured target identity");
            if (a.Get() != b.Get())
                throw std::runtime_error("Selected resource differs from RTV0");
            Com<ID3D11Texture2D> texture;
            check(bound.As(&texture), "Get RTV0 texture");
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (std::memcmp(&desc, resource.desc.data(), sizeof(desc)))
                throw std::runtime_error("Native texture descriptor differs");
            if (unblendedRt0) {
                D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
                views[0]->GetDesc(&viewDesc);
                if (viewDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
                    viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS)
                    throw std::runtime_error("Unblended mode requires RGBA16 FLOAT MSAA RTV");
                target = texture;
            } else {
                desc.Format = DXGI_FORMAT_R32G32B32A32_TYPELESS;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                desc.CPUAccessFlags = desc.MiscFlags = 0;
                std::memcpy(resource.desc.data(), &desc, sizeof(desc));
                resource.data = 0;
                check(device->CreateTexture2D(&desc, nullptr, &target), "Create private float target");
                D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
                viewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
                views[0].Reset();
                check(device->CreateRenderTargetView(target.Get(), &viewDesc, &views[0]), "Create float RTV");
                const float zero[4]{};
                context->ClearRenderTargetView(views[0].Get(), zero);
                for (UINT i = 0; i < 8; ++i)
                    rawViews[i] = views[i].Get();
                context->OMSetRenderTargets(8, rawViews, depth.Get());
            }
            Com<ID3D11BlendState> blend;
            float factor[4]{};
            UINT mask{};
            context->OMGetBlendState(&blend, factor, &mask);
            if (!blend)
                throw std::runtime_error("Explicit blend state required");
            Com<ID3D11BlendState1> blend1;
            if (SUCCEEDED(blend.As(&blend1))) {
                D3D11_BLEND_DESC1 desc1{};
                blend1->GetDesc1(&desc1);
                for (const auto &rt : desc1.RenderTarget)
                    if (rt.LogicOpEnable)
                        throw std::runtime_error("Logic operation unsupported");
            }
            D3D11_BLEND_DESC blendDesc{};
            blend->GetDesc(&blendDesc);
            if (!blendDesc.IndependentBlendEnable)
                throw std::runtime_error("Independent blending required");
            blendDesc.RenderTarget[0].BlendEnable = FALSE;
            blend.Reset();
            check(device->CreateBlendState(&blendDesc, &blend), "Create unblended state");
            context->OMSetBlendState(blend.Get(), factor, mask);
            if (!replacement.empty()) {
                Com<ID3D11PixelShader> ps;
                check(device->CreatePixelShader(replacement.data(), replacement.size(), nullptr, &ps),
                      "Create diagnostic PS");
                context->PSSetShader(ps.Get(), nullptr, 0);
            }
        });
        if (!observed || !target)
            throw std::runtime_error("Draw was not observed");
        for (UINT sample = 0; sample < resource.desc[5]; ++sample) {
            const auto data = readSample(target.Get(), sample, unblendedRt0);
            const size_t pixelBytes = unblendedRt0 ? 8 : 16;
            const size_t offset = (size_t(y) * resource.desc[0] + x) * pixelBytes;
            if (data.size() != size_t(resource.desc[0]) * resource.desc[1] * pixelBytes)
                throw std::runtime_error("Unexpected sample size");
            std::array<uint32_t, 4> words{};
            if (unblendedRt0) {
                std::array<uint16_t, 4> halves{};
                std::memcpy(halves.data(), data.data() + offset, 8);
                std::copy(halves.begin(), halves.end(), words.begin());
            } else
                std::memcpy(words.data(), data.data() + offset, 16);
            report[unblendedRt0 ? "half_output" : "float_output"].push_back(
                {{"sample", sample}, {"sha256", sha256(data)}, {"pixel_words", words}});
        }
        report["scoped_debug"] = messages(debug.Get());
        save();
        for (const auto &message : report["scoped_debug"])
            if (message.at("severity").get<UINT>() <= D3D11_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error("Fragment observation emitted debug warnings/errors");
        report["completed"] = true;
        save();
        std::cout << "Observed unblended fragment output\n";
        return 0;
    } catch (const std::exception &e) {
        report["error"] = e.what();
        try {
            save();
        } catch (...) {
        }
        std::cerr << e.what() << '\n';
        return 1;
    }
}
