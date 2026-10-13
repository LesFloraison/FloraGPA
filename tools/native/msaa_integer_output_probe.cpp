// Development-only CPU oracle and pixel-frequency invariants for half-float MSAA output.
// This does not use a capture, a replay library, or an Intel GPA component.
#define NOMINMAX
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("D3D operation failed: " + std::to_string(hr));
}
ComPtr<ID3DBlob> compile(const std::string &text, const char *profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(text.data(), text.size(), "MSAA integer oracle", nullptr, nullptr, "main",
                               profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr);
    return code;
}
uint16_t half(unsigned value) {
    // All oracle values are integers in [0, 2047], represented exactly by binary16.
    if (!value)
        return 0;
    const auto exponent = std::bit_width(value) - 1;
    return uint16_t(((exponent + 15) << 10) | ((value - (1u << exponent)) << (10 - exponent)));
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if ((argc < 3 || argc > 7) ||
        (std::wstring(argv[2]) != L"hardware" && std::wstring(argv[2]) != L"warp" &&
         std::wstring(argv[2]) != L"intel") ||
        (argc >= 4 && std::wstring(argv[3]) != L"sample" && std::wstring(argv[3]) != L"pixel-edges" &&
         std::wstring(argv[3]) != L"pixel-normal" && std::wstring(argv[3]) != L"pixel-clusters" &&
         std::wstring(argv[3]) != L"pixel-textured" && std::wstring(argv[3]) != L"pixel-mesh") ||
        (argc >= 4 && std::wstring(argv[3]) == L"pixel-mesh" && argc < 5) ||
        (argc == 7 && std::wstring(argv[6]) != L"resolve-source") ||
        (argc >= 6 &&
         (std::wstring(argv[3]) != L"pixel-mesh" ||
          (std::wstring(argv[5]) != L"summary-only" && std::wstring(argv[5]) != L"summary-rt-copy" &&
           std::wstring(argv[5]) != L"summary-region-copy" &&
           std::wstring(argv[5]) != L"summary-overwrite-0" &&
           std::wstring(argv[5]) != L"summary-overwrite-1"))) ||
        (argc == 5 && std::wstring(argv[3]) != L"pixel-mesh" &&
         std::wstring(argv[4]) != L"resolve-before-read" && std::wstring(argv[4]) != L"flush-before-read" &&
         std::wstring(argv[4]) != L"large")) {
        std::cerr
            << "NEW_OUT hardware|warp|intel [sample|pixel-edges|pixel-normal|pixel-clusters|pixel-textured] "
               "[resolve-before-read|flush-before-read|large]\n"
               "NEW_OUT hardware|warp|intel pixel-mesh POSTVS_BIN "
               "[summary-only|summary-rt-copy|summary-region-copy|summary-overwrite-0|summary-overwrite-1] "
               "[resolve-source]\n";
        return 2;
    }
    try {
        const bool pixelEdges = argc >= 4 && std::wstring(argv[3]) != L"sample";
        const bool pixelTextured = argc >= 4 && std::wstring(argv[3]) == L"pixel-textured";
        const bool pixelMesh = argc >= 4 && std::wstring(argv[3]) == L"pixel-mesh";
        const bool summaryOnly = argc >= 6;
        const bool resolveSource = argc == 7;
        const bool copyRenderTarget = argc >= 6 && std::wstring(argv[5]) == L"summary-rt-copy";
        const bool copyRegion = argc >= 6 && std::wstring(argv[5]) == L"summary-region-copy";
        const int overwriteSample = argc >= 6 && std::wstring(argv[5]) == L"summary-overwrite-0"   ? 0
                                    : argc >= 6 && std::wstring(argv[5]) == L"summary-overwrite-1" ? 1
                                                                                                   : -1;
        const bool pixelClusters = (argc >= 4 && std::wstring(argv[3]) == L"pixel-clusters") || pixelTextured;
        const bool pixelNormal =
            (argc >= 4 && std::wstring(argv[3]) == L"pixel-normal") || pixelClusters || pixelMesh;
        const bool resolveBeforeRead =
            !pixelMesh && argc == 5 && std::wstring(argv[4]) == L"resolve-before-read";
        const bool flushBeforeRead = !pixelMesh && argc == 5 && std::wstring(argv[4]) == L"flush-before-read";
        const bool large = (argc == 5 && std::wstring(argv[4]) == L"large") || pixelMesh;
        const bool compareBindings = pixelEdges || argc == 5;
        // This checks representability, not a CPU implementation of GPU normalize arithmetic.
        std::array<bool, 65536> integerHalf{};
        for (unsigned i = 0; i < 2048; ++i)
            integerHalf[half(i)] = true;
        const fs::path output = fs::absolute(argv[1]);
        if (!fs::create_directory(output))
            throw std::runtime_error("Output directory must be new");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGIAdapter1> selectedAdapter;
        const bool intel = std::wstring(argv[2]) == L"intel";
        if (intel) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr);
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc));
                if (desc.VendorId == 0x8086 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selectedAdapter = candidate;
                    break;
                }
            }
            if (!selectedAdapter)
                throw std::runtime_error("No Intel hardware adapter found");
        }
        check(D3D11CreateDevice(selectedAdapter.Get(),
                                intel                              ? D3D_DRIVER_TYPE_UNKNOWN
                                : std::wstring(argv[2]) == L"warp" ? D3D_DRIVER_TYPE_WARP
                                                                   : D3D_DRIVER_TYPE_HARDWARE,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> actualAdapter;
        check(device.As(&dxgiDevice));
        check(dxgiDevice->GetAdapter(&actualAdapter));
        DXGI_ADAPTER_DESC actual{};
        check(actualAdapter->GetDesc(&actual));
        ComPtr<ID3D11InfoQueue> debug;
        check(device.As(&debug));
        const auto vsCode = compile("float4 main(uint i:SV_VertexID):SV_Position{"
                                    "return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                                    "vs_5_0");
        const auto psCode = compile("float4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{"
                                    "uint x=(uint)p.x,y=(uint)p.y;"
                                    "return float4((x+y*3+s*101)&2047,(x*7+y+s*103)&2047,"
                                    "(x+y*11+s*107)&2047,(x*13+y*17+s*109)&2047);}",
                                    "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        ComPtr<ID3D11PixelShader> overwritePs;
        if (overwriteSample >= 0) {
            const auto code = compile("float4 main(float4 p:SV_Position):SV_Target{"
                                      "return float4(64,128,256,2047);}",
                                      "ps_5_0");
            check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                            &overwritePs));
            std::ofstream file(output / "overwrite-ps.dxbc", std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize());
            file.close();
        }
        ComPtr<ID3DBlob> renderVsCode = vsCode, renderPsCode = psCode;
        ComPtr<ID3D11VertexShader> edgeVs;
        ComPtr<ID3D11PixelShader> edgePs;
        ComPtr<ID3D11Buffer> meshBuffer;
        ComPtr<ID3D11InputLayout> meshLayout;
        UINT meshVertexCount = 0;
        ComPtr<ID3D11ShaderResourceView> normalSrv;
        ComPtr<ID3D11SamplerState> normalSampler;
        if (pixelTextured) {
            std::array<uint8_t, 16 * 16 * 4> texels{};
            for (size_t i = 0; i < texels.size(); ++i)
                texels[i] = uint8_t((i * 73 + i / 5) & 255);
            const D3D11_TEXTURE2D_DESC desc{16,
                                            16,
                                            1,
                                            1,
                                            DXGI_FORMAT_R8G8B8A8_UNORM,
                                            {1, 0},
                                            D3D11_USAGE_IMMUTABLE,
                                            D3D11_BIND_SHADER_RESOURCE,
                                            0,
                                            0};
            const D3D11_SUBRESOURCE_DATA initial{texels.data(), 16 * 4, 16 * 16 * 4};
            ComPtr<ID3D11Texture2D> normal;
            check(device->CreateTexture2D(&desc, &initial, &normal));
            check(device->CreateShaderResourceView(normal.Get(), nullptr, &normalSrv));
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            check(device->CreateSamplerState(&sampler, &normalSampler));
        }
        if (pixelEdges) {
            const std::string interpolation = pixelNormal ? "" : "nointerpolation ";
            const std::string geometry =
                pixelClusters
                    ? "uint prim=i/3,v=i%3,cell=prim/3;"
                      "float2 p[3]={float2(-0.013,-0.025),float2(0.018,-0.01),float2(-0.004,0.032)};"
                      "float2 xy=float2(cell%64,cell/64)/float2(64,32)*2-1+p[v]+"
                      "float2((prim%3)*0.0023,(prim%3)*0.0051);"
                      "O o;o.p=float4(xy,0,1);o.c=float4(xy*(v+1.7),0.73+(prim%3)*0.19,1);return o;}"
                    : "float2 p[6]={float2(-1,-1),float2(-1,1),float2(1,-1),"
                      "float2(1,-1),float2(-1,1),float2(1,1)};"
                      "O o;o.p=float4(p[i],0,1);o.c=" +
                          std::string(pixelNormal ? "float4(p[i],i<3?0.73:1.37,1)"
                                                  : "i<3?float4(1,511,1023,2047):float4(2,512,1024,2047)") +
                          ";return o;}";
            const auto edgeVsCode =
                compile("struct O{float4 p:SV_Position;" + interpolation + "float4 c:TEXCOORD0;};" +
                            (pixelMesh ? "struct I{float4 p:POSITION;float4 c:NORMAL;};"
                                         "O main(I i){O o;o.p=i.p;o.c=i.c;return o;}"
                                       : "O main(uint i:SV_VertexID){" + geometry),
                        pixelClusters || pixelMesh ? "vs_4_0" : "vs_5_0");
            const auto edgePsCode = compile(
                std::string(pixelTextured ? "Texture2D<float4> tex:register(t0);SamplerState sm:register(s0);"
                                          : "") +
                    "float4 main(float4 p:SV_Position," + interpolation +
                    "float4 c:TEXCOORD0)"
                    ":SV_Target{return " +
                    (pixelMesh ? std::string("float4(asuint(c.xyz)&2047,2047)")
                     : pixelTextured
                         ? std::string("float4(asuint(normalize(c.xyz+tex.Sample(sm,c.xy).xyz))&2047,2047)")
                     : pixelNormal ? std::string("float4(asuint(normalize(c.xyz))&2047,2047)")
                                   : std::string("c")) +
                    ";}",
                pixelClusters || pixelMesh ? "ps_4_0" : "ps_5_0");
            check(device->CreateVertexShader(edgeVsCode->GetBufferPointer(), edgeVsCode->GetBufferSize(),
                                             nullptr, &edgeVs));
            check(device->CreatePixelShader(edgePsCode->GetBufferPointer(), edgePsCode->GetBufferSize(),
                                            nullptr, &edgePs));
            renderVsCode = edgeVsCode;
            renderPsCode = edgePsCode;
            if (pixelMesh) {
                std::ifstream input(fs::path(argv[4]), std::ios::binary | std::ios::ate);
                if (!input || input.tellg() <= 0 || input.tellg() > 32000000 || input.tellg() % 96 != 0)
                    throw std::runtime_error("Expected bounded position/normal triangle-list bytes");
                std::vector<uint8_t> vertices(size_t(input.tellg()));
                input.seekg(0);
                input.read(reinterpret_cast<char *>(vertices.data()), vertices.size());
                if (!input)
                    throw std::runtime_error("Mesh read failed");
                for (size_t at = 0; at < vertices.size(); at += sizeof(float)) {
                    float value;
                    std::memcpy(&value, vertices.data() + at, sizeof(value));
                    if (!std::isfinite(value))
                        throw std::runtime_error("Mesh contains a nonfinite attribute");
                }
                meshVertexCount = UINT(vertices.size() / 32);
                const D3D11_BUFFER_DESC desc{
                    UINT(vertices.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
                const D3D11_SUBRESOURCE_DATA data{vertices.data(), 0, 0};
                check(device->CreateBuffer(&desc, &data, &meshBuffer));
                const D3D11_INPUT_ELEMENT_DESC elements[]{
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
                check(device->CreateInputLayout(elements, 2, edgeVsCode->GetBufferPointer(),
                                                edgeVsCode->GetBufferSize(), &meshLayout));
            }
        }
        const UINT width = large ? 3456 : 257, height = large ? 1944 : 131;
        for (const auto &[name, code] : std::array<std::pair<const char *, ID3DBlob *>, 2>{
                 {{"render-vs.dxbc", renderVsCode.Get()}, {"render-ps.dxbc", renderPsCode.Get()}}}) {
            std::ofstream file(output / name, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize());
            file.close();
        }
        std::ofstream configuration(output / "configuration.txt");
        configuration.exceptions(std::ios::badbit | std::ios::failbit);
        configuration << "vendor_id=" << actual.VendorId << "\ndevice_id=" << actual.DeviceId
                      << "\nwidth=" << width << "\nheight=" << height << "\nmesh_vertices=" << meshVertexCount
                      << "\nsummary_only=" << summaryOnly << "\ncopy_render_target=" << copyRenderTarget
                      << "\ncopy_region=" << copyRegion << "\nresolve_before_read=" << resolveBeforeRead
                      << "\noverwrite_sample=" << overwriteSample << "\nflush_before_read=" << flushBeforeRead
                      << "\nresolve_source=" << resolveSource << '\n';
        configuration.close();
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = rd.MultisampleEnable = TRUE;
        rd.FrontCounterClockwise = pixelMesh;
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rd, &raster));
        std::ofstream report(output / L"results.csv");
        report.exceptions(std::ios::failbit | std::ios::badbit);
        report << "samples,quality,repeat,sample,source_bind_flags,read_path,different_words\n";
        std::ofstream anomalies(output / L"anomalies.csv");
        anomalies.exceptions(std::ios::failbit | std::ios::badbit);
        anomalies
            << "samples,quality,repeat,sample,source_bind_flags,read_path,x,y,r_bits,g_bits,b_bits,a_bits\n";
        std::ofstream witnesses(output / L"witnesses.csv");
        witnesses.exceptions(std::ios::failbit | std::ios::badbit);
        witnesses
            << "samples,quality,repeat,sample,source_bind_flags,read_path,x,y,r_bits,g_bits,b_bits,a_bits\n";
        std::set<std::pair<UINT, UINT>> witnessPixels;
        // Fixed witness from the documented reduced GF2 geometry; never used as an oracle.
        if (pixelMesh)
            witnessPixels.emplace(815, 843);
        std::ofstream resolveReport(output / "resolved.csv");
        resolveReport.exceptions(std::ios::badbit | std::ios::failbit);
        resolveReport << "source_bind_flags,x,y,r_bits,g_bits,b_bits,a_bits\n";
        uint64_t totalDifferences = 0;
        for (const UINT samples : large ? std::vector<UINT>{2} : std::vector<UINT>{2, 4})
            for (const UINT quality : large ? std::vector<UINT>{0}
                                            : std::vector<UINT>{0, UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN)})
                for (const UINT sourceBind :
                     compareBindings
                         ? std::vector<UINT>{D3D11_BIND_RENDER_TARGET,
                                             D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE}
                         : std::vector<UINT>{D3D11_BIND_RENDER_TARGET}) {
                    D3D11_TEXTURE2D_DESC desc{width,
                                              height,
                                              1,
                                              1,
                                              DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                              {samples, quality},
                                              D3D11_USAGE_DEFAULT,
                                              sourceBind,
                                              0,
                                              0};
                    ComPtr<ID3D11Texture2D> rendered, readable, target, staging, resolved;
                    check(device->CreateTexture2D(&desc, nullptr, &rendered));
                    desc.BindFlags =
                        D3D11_BIND_SHADER_RESOURCE | (copyRenderTarget ? D3D11_BIND_RENDER_TARGET : 0);
                    check(device->CreateTexture2D(&desc, nullptr, &readable));
                    D3D11_RENDER_TARGET_VIEW_DESC vd{};
                    vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
                    ComPtr<ID3D11RenderTargetView> rtv;
                    check(device->CreateRenderTargetView(rendered.Get(), &vd, &rtv));
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                    sd.Format = DXGI_FORMAT_R16G16B16A16_UINT;
                    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                    sd.Texture2DMSArray.ArraySize = 1;
                    ComPtr<ID3D11ShaderResourceView> srv;
                    check(device->CreateShaderResourceView(readable.Get(), &sd, &srv));
                    desc.SampleDesc = {1, 0};
                    if (resolveBeforeRead || resolveSource) {
                        auto resolvedDesc = desc;
                        resolvedDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        check(device->CreateTexture2D(&resolvedDesc, nullptr, &resolved));
                    }
                    desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                    check(device->CreateTexture2D(&desc, nullptr, &target));
                    ComPtr<ID3D11RenderTargetView> outputView;
                    check(device->CreateRenderTargetView(target.Get(), nullptr, &outputView));
                    desc.BindFlags = 0;
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    check(device->CreateTexture2D(&desc, nullptr, &staging));
                    for (UINT repeat = 0; repeat < (summaryOnly ? 1u : 3u); ++repeat) {
                        context->ClearState();
                        context->VSSetShader(pixelEdges ? edgeVs.Get() : vs.Get(), nullptr, 0);
                        if (pixelMesh) {
                            auto buffer = meshBuffer.Get();
                            const UINT stride = 32, offset = 0;
                            context->IASetInputLayout(meshLayout.Get());
                            context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
                        }
                        context->PSSetShader(pixelEdges ? edgePs.Get() : ps.Get(), nullptr, 0);
                        if (pixelTextured) {
                            auto srv = normalSrv.Get();
                            auto sampler = normalSampler.Get();
                            context->PSSetShaderResources(0, 1, &srv);
                            context->PSSetSamplers(0, 1, &sampler);
                        }
                        context->RSSetState(raster.Get());
                        context->RSSetViewports(1, &viewport);
                        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        auto view = rtv.Get();
                        context->OMSetRenderTargets(1, &view, nullptr);
                        if (pixelClusters || pixelMesh) {
                            const float clear[4]{0, 0, 0, 2047};
                            context->ClearRenderTargetView(view, clear);
                        }
                        context->Draw(pixelMesh       ? meshVertexCount
                                      : pixelClusters ? 64 * 32 * 9
                                      : pixelEdges    ? 6
                                                      : 3,
                                      0);
                        if (overwriteSample >= 0) {
                            context->VSSetShader(vs.Get(), nullptr, 0);
                            context->IASetInputLayout(nullptr);
                            context->PSSetShader(overwritePs.Get(), nullptr, 0);
                            context->OMSetBlendState(nullptr, nullptr, 1u << overwriteSample);
                            context->Draw(3, 0);
                            context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
                        }
                        context->OMSetRenderTargets(0, nullptr, nullptr);
                        if (resolveSource) {
                            context->ResolveSubresource(resolved.Get(), 0, rendered.Get(), 0,
                                                        DXGI_FORMAT_R16G16B16A16_FLOAT);
                            context->CopyResource(staging.Get(), resolved.Get());
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                            if (mapped.RowPitch < width * 8) {
                                context->Unmap(staging.Get(), 0);
                                throw std::runtime_error("Resolved row is too short");
                            }
                            for (const auto &[x, y] : witnessPixels) {
                                const auto row = reinterpret_cast<const uint16_t *>(
                                    static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
                                resolveReport << sourceBind << ',' << x << ',' << y;
                                for (unsigned k = 0; k < 4; ++k)
                                    resolveReport << ',' << row[x * 4 + k];
                                resolveReport << '\n';
                            }
                            context->Unmap(staging.Get(), 0);
                        }
                        if (resolveBeforeRead)
                            context->ResolveSubresource(resolved.Get(), 0, rendered.Get(), 0,
                                                        DXGI_FORMAT_R16G16B16A16_FLOAT);
                        if (flushBeforeRead)
                            context->Flush();
                        if (copyRegion)
                            context->CopySubresourceRegion(readable.Get(), 0, 0, 0, 0, rendered.Get(), 0,
                                                           nullptr);
                        else
                            context->CopyResource(readable.Get(), rendered.Get());
                        context->VSSetShader(vs.Get(), nullptr, 0);
                        context->IASetInputLayout(nullptr);
                        for (const bool direct : (sourceBind & D3D11_BIND_SHADER_RESOURCE) != 0
                                                     ? std::vector<bool>{false, true}
                                                     : std::vector<bool>{false}) {
                            if (direct) {
                                srv.Reset();
                                check(device->CreateShaderResourceView(rendered.Get(), &sd, &srv));
                            } else {
                                srv.Reset();
                                check(device->CreateShaderResourceView(readable.Get(), &sd, &srv));
                            }
                            for (UINT sample = 0; sample < samples; ++sample) {
                                const auto code = compile("Texture2DMSArray<uint4> tex:register(t0);"
                                                          "uint4 main(float4 p:SV_Position):SV_Target{"
                                                          "return tex.Load(int3(int2(p.xy),0)," +
                                                              std::to_string(sample) + ");}",
                                                          "ps_5_0");
                                ComPtr<ID3D11PixelShader> reader;
                                check(device->CreatePixelShader(code->GetBufferPointer(),
                                                                code->GetBufferSize(), nullptr, &reader));
                                context->PSSetShader(reader.Get(), nullptr, 0);
                                auto input = srv.Get();
                                context->PSSetShaderResources(0, 1, &input);
                                view = outputView.Get();
                                context->OMSetRenderTargets(1, &view, nullptr);
                                context->Draw(3, 0);
                                context->CopyResource(staging.Get(), target.Get());
                                D3D11_MAPPED_SUBRESOURCE mapped{};
                                check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                                if (mapped.RowPitch < width * 8) {
                                    context->Unmap(staging.Get(), 0);
                                    throw std::runtime_error("Oracle readback row is too short");
                                }
                                std::vector<uint16_t> bytes(width * height * 4);
                                uint64_t differences = 0;
                                unsigned recordedPixels = 0;
                                for (UINT y = 0; y < height; ++y) {
                                    const auto row = reinterpret_cast<const uint16_t *>(
                                        static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
                                    for (UINT x = 0; x < width; ++x) {
                                        const auto previousDifferences = differences;
                                        const std::array<unsigned, 4> values{
                                            x + y * 3 + sample * 101, x * 7 + y + sample * 103,
                                            x + y * 11 + sample * 107, x * 13 + y * 17 + sample * 109};
                                        for (unsigned k = 0; k < 4; ++k) {
                                            const auto value = row[x * 4 + k];
                                            bytes[(y * width + x) * 4 + k] = value;
                                            if (!pixelEdges)
                                                differences += value != half(values[k] & 2047);
                                        }
                                        if (pixelNormal) {
                                            differences += row[x * 4 + 3] != half(2047);
                                            if (overwriteSample == int(sample)) {
                                                const std::array<uint16_t, 3> sentinel{half(64), half(128),
                                                                                       half(256)};
                                                for (unsigned k = 0; k < 3; ++k)
                                                    differences += row[x * 4 + k] != sentinel[k];
                                            } else
                                                for (unsigned k = 0; k < 3; ++k)
                                                    differences += !integerHalf[row[x * 4 + k]];
                                        } else if (pixelEdges) {
                                            const auto color = row + x * 4;
                                            const std::array<uint16_t, 4> a{half(1), half(511), half(1023),
                                                                            half(2047)};
                                            const std::array<uint16_t, 4> b{half(2), half(512), half(1024),
                                                                            half(2047)};
                                            for (unsigned k = 0; k < 4; ++k)
                                                differences += color[k] != (color[0] == a[0] ? a[k] : b[k]);
                                        }
                                        if (differences != previousDifferences && recordedPixels++ < 32) {
                                            if (witnessPixels.size() < 32)
                                                witnessPixels.emplace(x, y);
                                            anomalies << samples << ',' << quality << ',' << repeat << ','
                                                      << sample << ',' << sourceBind << ','
                                                      << (direct ? "direct" : "copy") << ',' << x << ',' << y;
                                            for (unsigned k = 0; k < 4; ++k)
                                                anomalies << ',' << row[x * 4 + k];
                                            anomalies << '\n';
                                        }
                                    }
                                }
                                for (const auto &[x, y] : witnessPixels) {
                                    const auto row = reinterpret_cast<const uint16_t *>(
                                        static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
                                    witnesses << samples << ',' << quality << ',' << repeat << ',' << sample
                                              << ',' << sourceBind << ',' << (direct ? "direct" : "copy")
                                              << ',' << x << ',' << y;
                                    for (unsigned k = 0; k < 4; ++k)
                                        witnesses << ',' << row[x * 4 + k];
                                    witnesses << '\n';
                                }
                                context->Unmap(staging.Get(), 0);
                                const auto name =
                                    std::to_string(samples) + "-" + std::to_string(quality) + "-" +
                                    std::to_string(repeat) + "-" + std::to_string(sample) +
                                    (compareBindings
                                         ? "-" + std::to_string(sourceBind) + (direct ? "-direct" : "-copy")
                                         : "") +
                                    ".bin";
                                if (!summaryOnly) {
                                    std::ofstream raw(output / name, std::ios::binary);
                                    raw.exceptions(std::ios::badbit | std::ios::failbit);
                                    raw.write(reinterpret_cast<const char *>(bytes.data()),
                                              bytes.size() * sizeof(uint16_t));
                                    raw.close();
                                }
                                report << samples << ',' << quality << ',' << repeat << ',' << sample << ','
                                       << sourceBind << ',' << (direct ? "direct" : "copy") << ','
                                       << differences << '\n';
                                totalDifferences += differences;
                            }
                        }
                    }
                }
        check(device->GetDeviceRemovedReason());
        uint64_t debugFailures = 0;
        for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(debug->GetMessage(i, nullptr, &size));
            std::vector<uint8_t> storage(size);
            auto message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(debug->GetMessage(i, message, &size));
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                ++debugFailures;
                std::cerr << "D3D11 debug: " << message->pDescription << '\n';
            }
        }
        report.close();
        anomalies.close();
        witnesses.close();
        resolveReport.close();
        std::cout << "different_words=" << totalDifferences << " debug_failures=" << debugFailures << '\n';
        return totalDifferences || debugFailures ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
