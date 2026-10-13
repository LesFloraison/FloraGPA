// Development-only CPU oracle and pixel-frequency invariants for half-float MSAA output.
// This does not use a capture, a replay library, or an Intel GPA component.
#define NOMINMAX
#include <array>
#include <bit>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    if ((argc != 3 && argc != 4) ||
        (std::wstring(argv[2]) != L"hardware" && std::wstring(argv[2]) != L"warp") ||
        (argc == 4 && std::wstring(argv[3]) != L"pixel-edges" && std::wstring(argv[3]) != L"pixel-normal")) {
        std::cerr << "NEW_OUT hardware|warp [pixel-edges|pixel-normal]\n";
        return 2;
    }
    try {
        const bool pixelEdges = argc == 4; // Both optional modes use a two-triangle pixel-frequency draw.
        const bool pixelNormal = argc == 4 && std::wstring(argv[3]) == L"pixel-normal";
        // This checks representability, not a CPU implementation of GPU normalize arithmetic.
        std::array<bool, 65536> integerHalf{};
        for (unsigned i = 0; i < 2048; ++i)
            integerHalf[half(i)] = true;
        const fs::path output = fs::absolute(argv[1]);
        if (!fs::create_directory(output))
            throw std::runtime_error("Output directory must be new");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(
            nullptr, std::wstring(argv[2]) == L"warp" ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
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
        ComPtr<ID3D11VertexShader> edgeVs;
        ComPtr<ID3D11PixelShader> edgePs;
        if (pixelEdges) {
            const std::string interpolation = pixelNormal ? "" : "nointerpolation ";
            const auto edgeVsCode =
                compile("struct O{float4 p:SV_Position;" + interpolation +
                            "float4 c:TEXCOORD0;};"
                            "O main(uint i:SV_VertexID){"
                            "float2 p[6]={float2(-1,-1),float2(-1,1),float2(1,-1),"
                            "float2(1,-1),float2(-1,1),float2(1,1)};"
                            "O o;o.p=float4(p[i],0,1);o.c=" +
                            (pixelNormal ? "float4(p[i],i<3?0.73:1.37,1)"
                                         : "i<3?float4(1,511,1023,2047):float4(2,512,1024,2047)") +
                            ";return o;}",
                        "vs_5_0");
            const auto edgePsCode =
                compile("float4 main(float4 p:SV_Position," + interpolation +
                            "float4 c:TEXCOORD0)"
                            ":SV_Target{return " +
                            (pixelNormal ? std::string("float4(asuint(normalize(c.xyz))&2047,2047)")
                                         : std::string("c")) +
                            ";}",
                        "ps_5_0");
            check(device->CreateVertexShader(edgeVsCode->GetBufferPointer(), edgeVsCode->GetBufferSize(),
                                             nullptr, &edgeVs));
            check(device->CreatePixelShader(edgePsCode->GetBufferPointer(), edgePsCode->GetBufferSize(),
                                            nullptr, &edgePs));
        }
        constexpr UINT width = 257, height = 131;
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = rd.MultisampleEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rd, &raster));
        std::ofstream report(output / L"results.csv");
        report.exceptions(std::ios::failbit | std::ios::badbit);
        report << "samples,quality,repeat,sample,source_bind_flags,read_path,different_words\n";
        uint64_t totalDifferences = 0;
        for (const UINT samples : {2u, 4u})
            for (const UINT quality : {0u, UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN)})
                for (const UINT sourceBind :
                     pixelEdges ? std::vector<UINT>{D3D11_BIND_RENDER_TARGET,
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
                    ComPtr<ID3D11Texture2D> rendered, readable, target, staging;
                    check(device->CreateTexture2D(&desc, nullptr, &rendered));
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
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
                    desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                    check(device->CreateTexture2D(&desc, nullptr, &target));
                    ComPtr<ID3D11RenderTargetView> outputView;
                    check(device->CreateRenderTargetView(target.Get(), nullptr, &outputView));
                    desc.BindFlags = 0;
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    check(device->CreateTexture2D(&desc, nullptr, &staging));
                    for (UINT repeat = 0; repeat < 3; ++repeat) {
                        context->ClearState();
                        context->VSSetShader(pixelEdges ? edgeVs.Get() : vs.Get(), nullptr, 0);
                        context->PSSetShader(pixelEdges ? edgePs.Get() : ps.Get(), nullptr, 0);
                        context->RSSetState(raster.Get());
                        context->RSSetViewports(1, &viewport);
                        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        auto view = rtv.Get();
                        context->OMSetRenderTargets(1, &view, nullptr);
                        context->Draw(pixelEdges ? 6 : 3, 0);
                        context->OMSetRenderTargets(0, nullptr, nullptr);
                        context->CopyResource(readable.Get(), rendered.Get());
                        context->VSSetShader(vs.Get(), nullptr, 0);
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
                                for (UINT y = 0; y < height; ++y) {
                                    const auto row = reinterpret_cast<const uint16_t *>(
                                        static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
                                    for (UINT x = 0; x < width; ++x) {
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
                                            for (unsigned k = 0; k < 3; ++k)
                                                differences += !integerHalf[row[x * 4 + k]];
                                            differences += row[x * 4 + 3] != half(2047);
                                        } else if (pixelEdges) {
                                            const auto color = row + x * 4;
                                            const std::array<uint16_t, 4> a{half(1), half(511), half(1023),
                                                                            half(2047)};
                                            const std::array<uint16_t, 4> b{half(2), half(512), half(1024),
                                                                            half(2047)};
                                            for (unsigned k = 0; k < 4; ++k)
                                                differences += color[k] != (color[0] == a[0] ? a[k] : b[k]);
                                        }
                                    }
                                }
                                context->Unmap(staging.Get(), 0);
                                const auto name =
                                    std::to_string(samples) + "-" + std::to_string(quality) + "-" +
                                    std::to_string(repeat) + "-" + std::to_string(sample) +
                                    (pixelEdges
                                         ? "-" + std::to_string(sourceBind) + (direct ? "-direct" : "-copy")
                                         : "") +
                                    ".bin";
                                std::ofstream raw(output / name, std::ios::binary);
                                raw.exceptions(std::ios::badbit | std::ios::failbit);
                                raw.write(reinterpret_cast<const char *>(bytes.data()),
                                          bytes.size() * sizeof(uint16_t));
                                raw.close();
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
        std::cout << "different_words=" << totalDifferences << " debug_failures=" << debugFailures << '\n';
        return totalDifferences || debugFailures ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
