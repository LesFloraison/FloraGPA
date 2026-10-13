// Development-only same-execution comparison of MSAA inspection paths.
#include "replay/Replay.h"
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> readFile(const fs::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0 || f.tellg() > 1048576)
        throw std::runtime_error("Expected a bounded shader file");
    std::vector<uint8_t> bytes(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!f)
        throw std::runtime_error("Shader read failed");
    return bytes;
}
Com<ID3DBlob> compile(const std::string &text, const char *profile) {
    Com<ID3DBlob> code, errors;
    check(D3DCompile(text.data(), text.size(), "MSAA diagnosis", nullptr, nullptr, "main", profile,
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors),
          "Compile MSAA diagnosis");
    return code;
}
std::vector<uint8_t> read(ID3D11Device *device, ID3D11Texture2D *source, bool integer, UINT sample,
                          bool copySource = true) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count <= 1 || desc.SampleDesc.Count <= sample)
        throw std::runtime_error("Probe expects a single-layer half-float MSAA target");
    const auto width = desc.Width, height = desc.Height;
    const bool directAvailable = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    if (!copySource && !directAvailable)
        throw std::runtime_error("Direct MSAA probe requires a shader-readable source");
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> readable, target, staging;
    if (copySource)
        check(device->CreateTexture2D(&desc, nullptr, &readable), "Create probe copy");
    else
        readable = source;
    const auto format = integer ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    sd.Texture2DMSArray.ArraySize = 1;
    Com<ID3D11ShaderResourceView> srv;
    check(device->CreateShaderResourceView(readable.Get(), &sd, &srv), "Create probe SRV");
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&desc, nullptr, &target), "Create probe target");
    Com<ID3D11RenderTargetView> rtv;
    check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create probe RTV");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "Create probe staging");
    const auto vsCode = compile(
        "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const std::string kind = integer ? "uint4" : "float4";
    const auto psCode =
        compile("Texture2DMSArray<" + kind + "> tex:register(t0);" + kind +
                    " main(float4 p:SV_Position):SV_Target{return tex.Load(int3(int2(p.xy),0)," +
                    std::to_string(sample) + ");}",
                "ps_5_0");
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs),
          "Create probe VS");
    check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps),
          "Create probe PS");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = raster.MultisampleEnable = TRUE;
    Com<ID3D11RasterizerState> rs;
    check(device->CreateRasterizerState(&raster, &rs), "Create probe RS");
    Com<ID3D11DeviceContext> deferred, immediate;
    check(device->CreateDeferredContext(0, &deferred), "Create probe context");
    device->GetImmediateContext(&immediate);
    if (copySource)
        deferred->CopyResource(readable.Get(), source);
    deferred->VSSetShader(vs.Get(), nullptr, 0);
    deferred->PSSetShader(ps.Get(), nullptr, 0);
    deferred->RSSetState(rs.Get());
    const D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    deferred->RSSetViewports(1, &vp);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto input = srv.Get();
    deferred->PSSetShaderResources(0, 1, &input);
    auto output = rtv.Get();
    deferred->OMSetRenderTargets(1, &output, nullptr);
    deferred->Draw(3, 0);
    deferred->CopyResource(staging.Get(), target.Get());
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish probe context");
    immediate->ExecuteCommandList(commands.Get(), TRUE);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map probe output");
    if (mapped.RowPitch < uint64_t(width) * 8) {
        immediate->Unmap(staging.Get(), 0);
        throw std::runtime_error("Probe row pitch is shorter than the texture row");
    }
    std::vector<uint8_t> bytes(size_t(width) * height * 8);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(bytes.data() + size_t(y) * width * 8,
                    static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                    size_t(width) * 8);
    immediate->Unmap(staging.Get(), 0);
    check(device->GetDeviceRemovedReason(), "Probe device status");
    return bytes;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if ((argc != 8 && argc != 9) ||
        (argc == 9 && std::wstring(argv[8]) != L"at-boundary" && std::wstring(argv[8]) != L"fresh-target" &&
         std::wstring(argv[8]) != L"fresh-target-srv" && std::wstring(argv[8]) != L"single-target" &&
         std::wstring(argv[8]) != L"no-depth")) {
        std::cerr << "FRAME NEW_OUT EVENT RESOURCE SAMPLE SHADER_ID SHADER_DXBC "
                     "[at-boundary|fresh-target|fresh-target-srv|single-target|no-depth]\n";
        return 2;
    }
    try {
        const std::wstring mode = argc == 9 ? argv[8] : L"default";
        const bool atBoundary = mode == L"at-boundary";
        const bool freshTarget = mode == L"fresh-target" || mode == L"fresh-target-srv";
        std::string modeName;
        for (const auto c : mode)
            modeName.push_back(static_cast<char>(c)); // Validated ASCII mode names only.
        const auto number = [](const wchar_t *value) {
            const std::wstring text(value);
            if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::invalid_argument("Expected unsigned decimal argument");
            return std::stoull(text);
        };
        const auto event = number(argv[3]), resource = number(argv[4]), sample = number(argv[5]),
                   shader = number(argv[6]);
        if (!event || !resource || !shader || sample > UINT32_MAX)
            throw std::invalid_argument("Invalid probe selection");
        const fs::path output = fs::absolute(argv[2]);
        if (!fs::create_directory(output))
            throw std::runtime_error("Output directory must be new");
        Frame frame(fs::absolute(argv[1]));
        ReplayOptions options;
        options.until = event;
        options.shaders[shader] = readFile(argv[7]);
        Replay replay(frame, options);
        Com<ID3D11Texture2D> source;
        Json hashes = Json::object();
        auto save = [&](const std::string &name, const std::vector<uint8_t> &bytes) {
            std::ofstream f(output / (name + ".bin"), std::ios::binary);
            f.exceptions(std::ios::badbit | std::ios::failbit);
            f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            f.close();
            hashes[name] = sha256(bytes);
        };
        replay.run({}, [&](Id id, bool after, ID3D11DeviceContext *context, const auto &objects) {
            if (id == event && !after && (mode == L"single-target" || mode == L"no-depth")) {
                std::array<ID3D11RenderTargetView *, 8> raw{};
                std::array<Com<ID3D11RenderTargetView>, 8> held;
                Com<ID3D11DepthStencilView> depth;
                context->OMGetRenderTargets(UINT(raw.size()), raw.data(), &depth);
                for (size_t i = 0; i < raw.size(); ++i)
                    held[i].Attach(raw[i]);
                context->OMSetRenderTargets(mode == L"single-target" ? 1 : UINT(raw.size()), raw.data(),
                                            mode == L"no-depth" ? nullptr : depth.Get());
            }
            if (id == event && !after && freshTarget) {
                Com<ID3D11Texture2D> original;
                check(objects.at(resource).As(&original), "Retain original target");
                D3D11_TEXTURE2D_DESC desc{};
                original->GetDesc(&desc);
                if (mode == L"fresh-target-srv")
                    desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                check(replay.nativeDevice()->CreateTexture2D(&desc, nullptr, &source), "Create fresh target");
                std::array<ID3D11RenderTargetView *, 8> raw{};
                std::array<Com<ID3D11RenderTargetView>, 8> held;
                Com<ID3D11DepthStencilView> depth;
                context->OMGetRenderTargets(UINT(raw.size()), raw.data(), &depth);
                for (size_t i = 0; i < raw.size(); ++i)
                    held[i].Attach(raw[i]);
                if (!held[0])
                    throw std::runtime_error("Fresh-target probe requires RTV slot zero");
                Com<ID3D11Resource> bound;
                held[0]->GetResource(&bound);
                if (bound.Get() != static_cast<ID3D11Resource *>(original.Get()))
                    throw std::runtime_error("Selected resource is not RTV slot zero");
                D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
                held[0]->GetDesc(&viewDesc);
                Com<ID3D11RenderTargetView> replacement;
                check(replay.nativeDevice()->CreateRenderTargetView(source.Get(), &viewDesc, &replacement),
                      "Create fresh target RTV");
                context->OMSetRenderTargets(0, nullptr, nullptr);
                context->CopyResource(source.Get(), original.Get());
                raw[0] = replacement.Get();
                context->OMSetRenderTargets(UINT(raw.size()), raw.data(), depth.Get());
            }
            if (id == event && after) {
                if (!freshTarget)
                    check(objects.at(resource).As(&source), "Retain selected texture for probe");
                if (atBoundary) {
                    save("boundary-float", read(replay.nativeDevice(), source.Get(), false, UINT(sample)));
                    save("boundary-integer", read(replay.nativeDevice(), source.Get(), true, UINT(sample)));
                }
            }
        });
        if (!source)
            throw std::runtime_error("Selected draw/resource not observed");
        D3D11_TEXTURE2D_DESC descriptor{};
        source->GetDesc(&descriptor);
        const bool directAvailable = (descriptor.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
        if (directAvailable) {
            save("direct-float", read(replay.nativeDevice(), source.Get(), false, UINT(sample), false));
            save("direct-integer", read(replay.nativeDevice(), source.Get(), true, UINT(sample), false));
        }
        if (!freshTarget)
            save("native-first",
                 replay.readMsaa(resource, UINT(sample), DXGI_FORMAT_R16G16B16A16_FLOAT).bytes);
        save("float", read(replay.nativeDevice(), source.Get(), false, UINT(sample)));
        save("integer", read(replay.nativeDevice(), source.Get(), true, UINT(sample)));
        if (!freshTarget)
            save("native-last",
                 replay.readMsaa(resource, UINT(sample), DXGI_FORMAT_R16G16B16A16_FLOAT).bytes);
        Json report{{"completed", true},
                    {"acceptance_established", false},
                    {"reference_pixels_used", false},
                    {"frame_sha256", frame.sha256()},
                    {"adapter", replay.adapter()},
                    {"event", event},
                    {"resource", resource},
                    {"sample", sample},
                    {"shader", shader},
                    {"direct_readback_available", directAvailable},
                    {"read_at_draw_boundary", atBoundary},
                    {"fresh_target_at_draw", freshTarget},
                    {"diagnostic_mode", modeName},
                    {"source_bind_flags", descriptor.BindFlags},
                    {"shader_sha256", sha256(options.shaders.at(shader))},
                    {"output_sha256", hashes}};
        std::ofstream f(output / L"report.json");
        f.exceptions(std::ios::badbit | std::ios::failbit);
        f << report.dump(2) << '\n';
        f.close();
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
