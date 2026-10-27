// Independent D3D11 tessellation identity and line-rasterization observation.
#define NOMINMAX
#include <chrono>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
void check(HRESULT hr, const char *what) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + ": " + std::to_string(hr));
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *profile, const fs::path &path) {
    ComPtr<ID3DBlob> code, error;
    const auto hr = D3DCompile(source.data(), source.size(), "Independent identity probe", nullptr, nullptr,
                               "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(hr) && error)
        throw std::runtime_error(static_cast<const char *>(error->GetBufferPointer()));
    check(hr, "Compile shader");
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize());
    return code;
}
void query(ID3D11DeviceContext *context, ID3D11Query *q, void *data, UINT bytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto hr = context->GetData(q, data, bytes, 0);
        check(hr, "Read query");
        if (hr == S_OK)
            return;
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Query deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 5) {
        std::cerr << "NEW_DIRECTORY warp|nvidia|intel tri|isoline legacy|quadrilateral\n";
        return 2;
    }
    try {
        const std::wstring selected = argv[2], domain = argv[3], raster = argv[4];
        if ((selected != L"warp" && selected != L"nvidia" && selected != L"intel") ||
            (domain != L"tri" && domain != L"isoline") || (raster != L"legacy" && raster != L"quadrilateral"))
            throw std::runtime_error("Invalid probe selection");
        const auto directory = fs::absolute(argv[1]);
        if (!fs::create_directory(directory))
            throw std::runtime_error("Output must be new");
        const bool line = domain == L"isoline", warp = selected == L"warp", quad = raster == L"quadrilateral";
        ComPtr<IDXGIAdapter1> adapter;
        if (!warp) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create factory");
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate adapter");
                DXGI_ADAPTER_DESC1 d{};
                check(candidate->GetDesc1(&d), "Describe candidate");
                if (d.VendorId == (selected == L"intel" ? 32902u : 4318u)) {
                    adapter = candidate;
                    break;
                }
            }
            if (!adapter)
                throw std::runtime_error("Selected adapter is unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
        check(D3D11CreateDevice(adapter.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                                &context),
              "Create device");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actual;
        DXGI_ADAPTER_DESC desc{};
        check(device.As(&dxgi), "Query DXGI device");
        check(dxgi->GetAdapter(&actual), "Get actual adapter");
        check(actual->GetDesc(&desc), "Describe actual adapter");
        const std::string n = line ? "2" : "3", d = line ? "isoline" : "tri";
        const std::string v = "struct V{float4 p:SV_Position;};";
        const std::string constants =
            line ? "struct C{float e[2]:SV_TessFactor;};"
                 : "struct C{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;};";
        const auto vc = compile(
            "float4 main(uint v:SV_VertexID,uint inst:SV_InstanceID):SV_Position{uint cell=v/" + n +
                ";uint corner=v%" + n + ";float2 p=float2(2.5+cell*24,2.5+(inst%7)*12);p+=float2(" +
                std::string(line ? "corner==1?8:0" : "corner==2?8:0") + "," +
                std::string(line ? "0" : "corner==1?8:0") + ");return float4(p.x/64-1,1-p.y/64,.5,1);}",
            "vs_5_0", directory / "vs.dxbc");
        const auto hc =
            compile(v + constants + "C patch(InputPatch<V," + n + "> p){C c;" +
                        std::string(line ? "c.e[0]=c.e[1]=2;" : "c.e[0]=c.e[1]=c.e[2]=c.i=2;") +
                        "return c;}[domain(\"" + d + "\")][partitioning(\"integer\")][outputtopology(\"" +
                        std::string(line ? "line" : "triangle_cw") + "\")][outputcontrolpoints(" + n +
                        ")][patchconstantfunc(\"patch\")]V main(InputPatch<V," + n +
                        "> p,uint id:SV_OutputControlPointID){return p[id];}",
                    "hs_5_0", directory / "hs.dxbc");
        const auto dc = compile(
            v + constants + "[domain(\"" + d + "\")]float4 main(C c,float" + std::string(line ? "2" : "3") +
                " loc:SV_DomainLocation,const OutputPatch<V," + n + "> p):SV_Position{return " +
                std::string(line ? "lerp(p[0].p,p[1].p,loc.x)+float4(0,-loc.y*6/64,0,0)"
                                 : "p[0].p*loc.x+p[1].p*loc.y+p[2].p*loc.z") +
                ";}",
            "ds_5_0", directory / "ds.dxbc");
        const auto pc =
            compile("uint main(float4 p:SV_Position,uint id:SV_PrimitiveID):SV_Target{return id+1;}",
                    "ps_5_0", directory / "ps.dxbc");
        const auto gc = compile(v + "struct O{uint id:ID;};[maxvertexcount(1)]void main(" +
                                    std::string(line ? "line" : "triangle") + " V v[" + n +
                                    "],uint id:SV_PrimitiveID,inout PointStream<O> stream)"
                                    "{O o;o.id=id;stream.Append(o);}",
                                "gs_5_0", directory / "gs-id.dxbc");
        const auto ic = compile(v + constants + "[domain(\"" + d + "\")]uint main(C c,const OutputPatch<V," +
                                    n + "> p,uint id:SV_PrimitiveID):ID{return id;}",
                                "ds_5_0", directory / "ds-id.dxbc");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> ds, ids;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11GeometryShader> gs, identitySo;
        check(device->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &vs),
              "Create VS");
        check(device->CreateHullShader(hc->GetBufferPointer(), hc->GetBufferSize(), nullptr, &hs),
              "Create HS");
        check(device->CreateDomainShader(dc->GetBufferPointer(), dc->GetBufferSize(), nullptr, &ds),
              "Create DS");
        check(device->CreateDomainShader(ic->GetBufferPointer(), ic->GetBufferSize(), nullptr, &ids),
              "Create ID DS");
        check(device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps),
              "Create PS");
        const D3D11_SO_DECLARATION_ENTRY entry{0, "ID", 0, 0, 1, 0};
        const UINT stride = 4;
        check(device->CreateGeometryShaderWithStreamOutput(gc->GetBufferPointer(), gc->GetBufferSize(),
                                                           &entry, 1, &stride, 1,
                                                           D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs),
              "Create ID GS");
        check(device->CreateGeometryShaderWithStreamOutput(
                  ic->GetBufferPointer(), ic->GetBufferSize(), &entry, 1, &stride, 1,
                  D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &identitySo),
              "Create ID SO");
        D3D11_TEXTURE2D_DESC td{
            128, 128, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET,
            0,   0};
        ComPtr<ID3D11Texture2D> target, readback;
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateTexture2D(&td, nullptr, &target), "Create target");
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create RTV");
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&td, nullptr, &readback), "Create readback");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.MultisampleEnable = quad;
        ComPtr<ID3D11RasterizerState> rs;
        check(device->CreateRasterizerState(&rd, &rs), "Create rasterizer");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                      D3D11_COMPARISON_ALWAYS};
        ComPtr<ID3D11DepthStencilState> depth;
        check(device->CreateDepthStencilState(&dd, &depth), "Create depth state");
        const D3D11_VIEWPORT viewport{0, 0, 128, 128, 0, 1};
        const float zero[4]{};
        context->RSSetState(rs.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(rtv.Get(), zero);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(line ? 34 : 35));
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->HSSetShader(hs.Get(), nullptr, 0);
        context->DSSetShader(ds.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->DrawInstanced(line ? 4 : 6, 2, 3, 2);
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read original pixels");
        Json pixels = Json::array();
        for (UINT y = 0; y < 128; ++y)
            for (UINT x = 0; x < 128; ++x) {
                uint32_t value;
                std::memcpy(&value, static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * 4,
                            4);
                if (value)
                    pixels.push_back({x, y, value});
            }
        context->Unmap(readback.Get(), 0);
        auto observe = [&](bool patch) {
            D3D11_BUFFER_DESC bd{4096, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
            ComPtr<ID3D11Buffer> output, staging;
            check(device->CreateBuffer(&bd, nullptr, &output), "Create SO");
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateBuffer(&bd, nullptr, &staging), "Create SO readback");
            const D3D11_QUERY_DESC qd{D3D11_QUERY_SO_STATISTICS, 0};
            ComPtr<ID3D11Query> q;
            check(device->CreateQuery(&qd, &q), "Create SO query");
            context->DSSetShader(patch ? ids.Get() : ds.Get(), nullptr, 0);
            context->GSSetShader(patch ? identitySo.Get() : gs.Get(), nullptr, 0);
            const UINT offset = 0;
            context->SOSetTargets(1, output.GetAddressOf(), &offset);
            context->Begin(q.Get());
            context->DrawInstanced(line ? 4 : 6, 2, 3, 2);
            context->End(q.Get());
            D3D11_QUERY_DATA_SO_STATISTICS stats{};
            query(context.Get(), q.Get(), &stats, sizeof stats);
            context->SOSetTargets(0, nullptr, nullptr);
            context->CopyResource(staging.Get(), output.Get());
            const UINT factor = patch ? (line ? 2 : 3) : 1;
            if (stats.NumPrimitivesWritten != stats.PrimitivesStorageNeeded ||
                stats.NumPrimitivesWritten > 4096 / 4 / factor)
                throw std::runtime_error("Unexpected SO count");
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read identities");
            std::vector<uint32_t> values(stats.NumPrimitivesWritten);
            for (size_t i = 0; i < values.size(); ++i)
                std::memcpy(&values[i], static_cast<const uint8_t *>(mapped.pData) + i * factor * 4, 4);
            context->Unmap(staging.Get(), 0);
            return values;
        };
        const auto systemIds = observe(false), patchIds = observe(true);
        ComPtr<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Get debug queue");
        Json errors = Json::array();
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Size debug message");
            std::vector<uint8_t> data(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
            check(queue->GetMessage(i, message, &size), "Read debug message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                errors.push_back(message->pDescription);
        }
        Json result{{"schema", 1},
                    {"scope", "standalone_d3d11_identity_observation"},
                    {"vendor", desc.VendorId},
                    {"device", desc.DeviceId},
                    {"domain", d},
                    {"quadrilateral_lines", quad},
                    {"original_pixels", pixels},
                    {"system_ids", systemIds},
                    {"patch_ids", patchIds},
                    {"system_matches_patch", systemIds == patchIds},
                    {"debug_errors", errors}};
        std::ofstream report(directory / "report.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << result.dump(2) << '\n';
        std::cout << "pixels=" << pixels.size() << " system_matches_patch=" << (systemIds == patchIds)
                  << " debug_errors=" << errors.size() << '\n';
        return errors.empty() ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
