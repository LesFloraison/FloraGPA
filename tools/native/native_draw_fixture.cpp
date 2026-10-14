// Development fixture runner. Intentionally has no Flora or GPA headers/libraries.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <bit>
#include <cstring>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <psapi.h>
#include <sstream>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
namespace fs = std::filesystem;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void check(HRESULT hr, const char *message) {
    if (FAILED(hr)) {
        std::ostringstream text;
        text << message << " (0x" << std::hex << unsigned(hr) << ")";
        throw std::runtime_error(text.str());
    }
}
uint32_t number(const wchar_t *text) {
    const std::wstring value(text);
    require(!value.empty() && value.find_first_not_of(L"0123456789") == std::wstring::npos,
            "Expected unsigned decimal argument");
    auto result = std::stoull(value);
    require(result <= UINT32_MAX, "Argument overflow");
    return uint32_t(result);
}
std::string hash(const Bytes &bytes) {
    unsigned char digest[32]{};
    check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                     ULONG(bytes.size()), digest, 32),
          "SHA256");
    std::string result;
    for (auto c : digest) {
        result += "0123456789abcdef"[c >> 4];
        result += "0123456789abcdef"[c & 15];
    }
    return result;
}
Bytes read(const fs::path &path, size_t limit) {
    auto length = fs::file_size(path);
    require(length <= limit, "Input exceeds size limit");
    Bytes bytes(size_t(length), 0);
    std::ifstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    return bytes;
}
Bytes payload(const fs::path &root, const Json &entry) {
    auto name = entry.at("file").get<std::string>();
    require(!name.empty() && name != "." && name != ".." &&
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") ==
                    std::string::npos,
            "Payload must have a flat filename");
    const auto path = root / name;
    require(!fs::is_symlink(path), "Payload symlink is unsupported");
    auto bytes = read(path, 256ull * 1024 * 1024);
    require(bytes.size() == entry.at("bytes").get<uint64_t>() &&
                hash(bytes) == entry.at("sha256").get<std::string>(),
            "Payload size or SHA256 mismatch");
    return bytes;
}
template <class T> T pod(const Json &entry) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 4 == 0);
    require(entry.is_array() && entry.size() == sizeof(T) / 4, "Descriptor size mismatch");
    std::array<uint32_t, sizeof(T) / 4> words{};
    for (size_t i = 0; i < words.size(); ++i)
        words[i] = entry.at(i).get<uint32_t>();
    T value{};
    std::memcpy(&value, words.data(), sizeof(T));
    return value;
}
void count(const Json &entry, size_t expected) {
    require(entry.is_array() && entry.size() == expected, "Binding count mismatch");
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *profile) {
    ComPtr<ID3DBlob> code, error;
    const auto hr = D3DCompile(source.data(), source.size(), "fixture", nullptr, nullptr, "main", profile,
                               D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(hr) && error)
        std::cerr << static_cast<const char *>(error->GetBufferPointer());
    check(hr, "Compile fixture helper");
    return code;
}
struct Resource {
    ComPtr<ID3D11Resource> object;
    D3D11_TEXTURE2D_DESC desc{};
    std::vector<Bytes> samples;
};
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11VertexShader> fullscreen;
    std::map<std::string, ComPtr<ID3D11PixelShader>> pixelShaders;
    Json adapter;
    explicit Gpu(uint32_t vendor) {
        ComPtr<IDXGIAdapter1> selected;
        if (vendor) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create factory");
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc), "Describe adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            require(bool(selected), "Requested hardware vendor unavailable");
        }
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create debug device");
        check(context.As(&context1), "Get context1");
        check(device.As(&debug), "Get debug queue");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actual;
        check(device.As(&dxgi), "Get DXGI device");
        check(dxgi->GetAdapter(&actual), "Get actual adapter");
        DXGI_ADAPTER_DESC desc{};
        check(actual->GetDesc(&desc), "Describe actual adapter");
        char name[1024]{};
        require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, desc.Description, -1, name, sizeof(name),
                                    nullptr, nullptr) > 0,
                "Invalid adapter name");
        adapter = {{"name", name}, {"vendor", desc.VendorId}, {"device", desc.DeviceId}};
        const auto code = compile(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        check(
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &fullscreen),
            "Create fullscreen VS");
    }
    ComPtr<ID3D11PixelShader> pixel(const std::string &source) {
        auto &result = pixelShaders[source];
        if (!result) {
            const auto code = compile(source, "ps_5_0");
            check(
                device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &result),
                "Create helper PS");
        }
        return result;
    }
    void begin(UINT width, UINT height, bool msaa) {
        context->ClearState();
        context->VSSetShader(fullscreen.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        rs.MultisampleEnable = msaa;
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rs, &raster), "Create helper rasterizer");
        context->RSSetState(raster.Get());
    }
    void draw(const std::string &source) {
        auto ps = pixel(source);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->Draw(3, 0);
    }
    ComPtr<ID3D11ShaderResourceView> srv(ID3D11Resource *resource, DXGI_FORMAT format, bool msaa) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = msaa ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (!msaa)
            desc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        check(device->CreateShaderResourceView(resource, &desc, &view), "Create helper SRV");
        return view;
    }
    static DXGI_FORMAT access(const Resource &r) {
        if (r.desc.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS)
            return DXGI_FORMAT_R32G32B32A32_UINT;
        if (r.desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
            return DXGI_FORMAT_R16G16B16A16_UINT;
        if (r.desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            return DXGI_FORMAT_R8G8B8A8_UINT;
        if (r.desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
            return DXGI_FORMAT_R32G32_UINT;
        throw std::runtime_error("Unsupported MSAA storage format");
    }
    static UINT pixelBytes(const Resource &r) {
        return r.desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS       ? 4
               : r.desc.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS ? 16
                                                                    : 8;
    }
    void restore(Resource &r) {
        const bool depth = r.desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
            check(device->CreateDepthStencilView(r.object.Get(), &d, &dsv), "Create restore DSV");
        } else {
            D3D11_RENDER_TARGET_VIEW_DESC d{};
            d.Format = access(r);
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            check(device->CreateRenderTargetView(r.object.Get(), &d, &rtv), "Create integer restore RTV");
        }
        begin(r.desc.Width, r.desc.Height, true);
        if (depth)
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0, 0);
        auto rawRtv = rtv.Get();
        context->OMSetRenderTargets(depth ? 0 : 1, depth ? nullptr : &rawRtv, dsv.Get());
        for (UINT sample = 0; sample < r.samples.size(); ++sample) {
            D3D11_TEXTURE2D_DESC d{r.desc.Width,
                                   r.desc.Height,
                                   1,
                                   1,
                                   access(r),
                                   {1, 0},
                                   D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_SHADER_RESOURCE,
                                   0,
                                   0};
            const D3D11_SUBRESOURCE_DATA data{r.samples[sample].data(), d.Width * pixelBytes(r), 0};
            ComPtr<ID3D11Texture2D> upload;
            check(device->CreateTexture2D(&d, &data, &upload), "Create sample upload");
            auto view = srv(upload.Get(), d.Format, false);
            auto raw = view.Get();
            context->PSSetShaderResources(0, 1, &raw);
            context->OMSetBlendState(nullptr, nullptr, 1u << sample);
            if (!depth) {
                draw("Texture2D<uint4> t:register(t0);uint4 main(float4 p:SV_Position):SV_Target{return "
                     "t.Load(int3(int2(p.xy),0));}");
            } else {
                D3D11_DEPTH_STENCIL_DESC ds{};
                ds.DepthEnable = TRUE;
                ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
                ds.StencilReadMask = ds.StencilWriteMask = 255;
                ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                              D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
                ComPtr<ID3D11DepthStencilState> state;
                check(device->CreateDepthStencilState(&ds, &state), "Create depth restore state");
                context->OMSetDepthStencilState(state.Get(), 0);
                draw("Texture2D<uint2> t:register(t0);float main(float4 p:SV_Position):SV_Depth{return "
                     "asfloat(t.Load(int3(int2(p.xy),0)).x);}");
                ds.DepthEnable = FALSE;
                ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                ds.StencilEnable = TRUE;
                ds.FrontFace.StencilPassOp = ds.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
                for (UINT bit = 0; bit < 8; ++bit) {
                    ds.StencilWriteMask = UINT8(1u << bit);
                    state.Reset();
                    check(device->CreateDepthStencilState(&ds, &state), "Create stencil restore state");
                    context->OMSetDepthStencilState(state.Get(), 255);
                    draw("Texture2D<uint2> t:register(t0);float main(float4 "
                         "p:SV_Position):SV_Depth{if((t.Load(int3(int2(p.xy),0)).y&" +
                         std::to_string(1u << bit) + ")==0)discard;return 0;}");
                }
            }
        }
        context->ClearState();
    }
    Bytes sample(Resource &r, UINT index) {
        begin(r.desc.Width, r.desc.Height, false);
        auto copyDesc = r.desc;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                             (r.desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? D3D11_BIND_DEPTH_STENCIL : 0);
        copyDesc.CPUAccessFlags = copyDesc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> copy;
        check(device->CreateTexture2D(&copyDesc, nullptr, &copy), "Create sample-readable copy");
        context->CopyResource(copy.Get(), r.object.Get());
        const bool depth = r.desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
        auto view = srv(copy.Get(), depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : access(r), true);
        ComPtr<ID3D11ShaderResourceView> stencil;
        if (depth)
            stencil = srv(copy.Get(), DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, true);
        ID3D11ShaderResourceView *views[]{view.Get(), stencil.Get()};
        context->PSSetShaderResources(0, 2, views);
        D3D11_TEXTURE2D_DESC desc{r.desc.Width,
                                  r.desc.Height,
                                  1,
                                  1,
                                  access(r),
                                  {1, 0},
                                  D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET,
                                  0,
                                  0};
        ComPtr<ID3D11Texture2D> target;
        check(device->CreateTexture2D(&desc, nullptr, &target), "Create sample read target");
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create sample read RTV");
        auto raw = rtv.Get();
        context->OMSetRenderTargets(1, &raw, nullptr);
        const auto s = std::to_string(index);
        if (depth)
            draw("Texture2DMS<float2> t:register(t0);Texture2DMS<uint2> u:register(t1);uint2 main(float4 "
                 "p:SV_Position):SV_Target{return uint2(asuint(t.Load(int2(p.xy)," +
                 s + ").x),u.Load(int2(p.xy)," + s + ").y&255);}");
        else
            draw("Texture2DMS<uint4> t:register(t0);uint4 main(float4 p:SV_Position):SV_Target{return "
                 "t.Load(int2(p.xy)," +
                 s + ");}");
        context->ClearState();
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Create sample staging");
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map sample result");
        const size_t row = size_t(desc.Width) * pixelBytes(r);
        Bytes result(row * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(result.data() + y * row,
                        static_cast<const unsigned char *>(mapped.pData) + size_t(y) * mapped.RowPitch, row);
        context->Unmap(staging.Get(), 0);
        return result;
    }
    Json samplePositions(const Resource &r) {
        require(r.desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS, "Sample-position probe requires color");
        begin(r.desc.SampleDesc.Count, 1, false);
        auto desc = r.desc;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> source;
        check(device->CreateTexture2D(&desc, nullptr, &source), "Create sample-pattern resource");
        auto view = srv(source.Get(), access(r), true);
        auto rawView = view.Get();
        context->PSSetShaderResources(0, 1, &rawView);
        D3D11_TEXTURE2D_DESC out{
            desc.SampleDesc.Count,    1, 1, 1, DXGI_FORMAT_R32G32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
            D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> target;
        check(device->CreateTexture2D(&out, nullptr, &target), "Create sample-position target");
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create sample-position RTV");
        auto raw = rtv.Get();
        context->OMSetRenderTargets(1, &raw, nullptr);
        draw("Texture2DMS<uint4> t:register(t0);float2 main(float4 p:SV_Position):SV_Target{"
             "return t.GetSamplePosition((int)p.x);}");
        context->ClearState();
        out.BindFlags = 0;
        out.Usage = D3D11_USAGE_STAGING;
        out.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&out, nullptr, &staging), "Create sample-position staging");
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map sample positions");
        std::array<std::array<float, 2>, 8> positions{};
        std::memcpy(positions.data(), mapped.pData, out.Width * sizeof(positions[0]));
        context->Unmap(staging.Get(), 0);
        Json result = Json::array();
        for (UINT i = 0; i < out.Width; ++i)
            result.push_back(positions[i]);
        return result;
    }
    Json messages() {
        Json result = Json::array();
        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            check(debug->GetMessage(i, nullptr, &size), "Size debug message");
            Bytes bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(debug->GetMessage(i, message, &size), "Read debug message");
            result.push_back(
                {{"severity", message->Severity},
                 {"id", message->ID},
                 {"text", std::string(message->pDescription, message->DescriptionByteLength - 1)}});
        }
        debug->ClearStoredMessages();
        return result;
    }
};
Json compare(const Bytes &actual, const Bytes &expected, UINT stride) {
    require(actual.size() == expected.size(), "Comparison size differs");
    size_t byteCount = 0, pixels = 0;
    for (size_t i = 0; i < actual.size(); i += stride) {
        bool differs = false;
        for (UINT j = 0; j < stride; ++j)
            if (actual[i + j] != expected[i + j]) {
                ++byteCount;
                differs = true;
            }
        pixels += differs;
    }
    return {{"exact", byteCount == 0},
            {"different_bytes", byteCount},
            {"different_pixels", pixels},
            {"sha256", hash(actual)},
            {"expected_sha256", hash(expected)}};
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc < 5) {
        std::cerr << "BUNDLE NEW_OUT PCI_VENDOR|warp REPEATS [--skip-draw] [--indices FIRST COUNT] "
                     "[--pixel RESOURCE X Y] [--sample-positions] [--float-rt0 | --unblended-rt0] [--summary-only]\n";
        return 2;
    }
    Json report{{"completed", false}, {"acceptance_established", false}, {"comparisons", Json::array()}};
    fs::path output;
    auto save = [&] {
        if (!output.empty()) {
            std::ofstream f(output / "report.json");
            f.exceptions(std::ios::badbit | std::ios::failbit);
            f << report.dump(2) << '\n';
            f.close();
        }
    };
    try {
        const auto repeats = number(argv[4]);
        bool skipDraw = false, summaryOnly = false, samplePositions = false, floatRt0 = false,
             unblendedRt0 = false;
        std::optional<std::array<UINT, 2>> indices;
        std::optional<std::array<UINT, 3>> pixel;
        for (int i = 5; i < argc; ++i) {
            const std::wstring option(argv[i]);
            if (option == L"--skip-draw" && !skipDraw)
                skipDraw = true;
            else if (option == L"--summary-only" && !summaryOnly)
                summaryOnly = true;
            else if (option == L"--sample-positions" && !samplePositions)
                samplePositions = true;
            else if (option == L"--float-rt0" && !floatRt0)
                floatRt0 = true;
            else if (option == L"--unblended-rt0" && !unblendedRt0)
                unblendedRt0 = true;
            else if (option == L"--indices" && !indices && i + 2 < argc) {
                indices = std::array<UINT, 2>{number(argv[i + 1]), number(argv[i + 2])};
                i += 2;
            } else if (option == L"--pixel" && !pixel && i + 3 < argc) {
                pixel = std::array<UINT, 3>{number(argv[i + 1]), number(argv[i + 2]), number(argv[i + 3])};
                i += 3;
            } else
                throw std::runtime_error("Unknown, duplicate or incomplete option");
        }
        require(!(skipDraw && indices), "Skip Draw cannot select an index range");
        require(!samplePositions || pixel.has_value(), "Sample positions require --pixel");
        require(!floatRt0 || pixel.has_value(), "Float output requires --pixel");
        require(!unblendedRt0 || pixel.has_value(), "Unblended output requires --pixel");
        require(!(floatRt0 && unblendedRt0), "Output transformations are mutually exclusive");
        report["skip_draw"] = skipDraw;
        report["summary_only"] = summaryOnly;
        report["float_rt0"] = floatRt0;
        report["unblended_rt0"] = unblendedRt0;
        require(repeats > 0 && repeats <= 20, "Repeat count must be 1..20");
        const auto vendor = std::wstring(argv[3]) == L"warp" ? 0 : number(argv[3]);
        require(vendor <= 65535, "Invalid vendor");
        const auto root = fs::absolute(argv[1]);
        const auto text = read(root / "bundle.json", 4 * 1024 * 1024);
        auto bundle = Json::parse(text);
        require(bundle.at("schema") == "Flora native draw fixture 1" && bundle.at("completed") == true,
                "Incomplete or unsupported fixture");
        if (indices) {
            const UINT count = bundle.at("draw").at(0).get<UINT>();
            require(bundle.at("topology").get<UINT>() == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
                        (*indices)[0] % 3 == 0 && (*indices)[1] && (*indices)[1] % 3 == 0 &&
                        (*indices)[0] <= count && (*indices)[1] <= count - (*indices)[0],
                    "Index range must select complete triangles within the original draw");
            require(bundle.at("draw").at(1).get<UINT>() <= UINT32_MAX - (*indices)[0],
                    "Draw start index overflow");
        }
        if (pixel) {
            const auto &item = bundle.at("resources").at(std::to_string((*pixel)[0]));
            require(item.contains("samples"), "Pixel inspection requires an MSAA output");
            const auto desc = pod<D3D11_TEXTURE2D_DESC>(item.at("desc"));
            require((*pixel)[1] < desc.Width && (*pixel)[2] < desc.Height,
                    "Pixel coordinate exceeds texture bounds");
            report["pixel"] = *pixel;
        }
        if (floatRt0 || unblendedRt0) {
            require(!bundle.at("rtv").at(0).is_null() &&
                        bundle.at("rtv").at(0).at("resource").get<uint64_t>() == (*pixel)[0],
                    "Unblended output requires the selected pixel in RTV 0");
            require(pod<D3D11_BLEND_DESC>(bundle.at("blend")).IndependentBlendEnable,
                    "Unblended output requires independent blend state");
            if (unblendedRt0) {
                const auto desc = pod<D3D11_TEXTURE2D_DESC>(
                    bundle.at("resources").at(std::to_string((*pixel)[0])).at("desc"));
                const auto view = pod<D3D11_RENDER_TARGET_VIEW_DESC>(bundle.at("rtv").at(0).at("desc"));
                require(desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS &&
                            view.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
                        "Unblended mode requires typeless RGBA16 with FLOAT RTV");
            }
        }
        const auto requestedOutput = fs::absolute(argv[2]);
        require(fs::create_directory(requestedOutput), "Output directory must be new");
        output = requestedOutput;
        report["bundle_sha256"] = hash(text);
        report["event"] = bundle.at("event");
        save();
        Gpu gpu(vendor);
        report["adapter"] = gpu.adapter;
        std::map<uint64_t, Resource> resources;
        require(bundle.at("resources").is_object() && bundle.at("resources").size() <= 256,
                "Resource count exceeds bound");
        uint64_t total = 0;
        for (const auto &[key, item] : bundle.at("resources").items()) {
            require(!key.empty() && key.find_first_not_of("0123456789") == std::string::npos,
                    "Invalid resource label");
            const auto id = std::stoull(key);
            require(id && !resources.contains(id), "Duplicate resource label");
            auto &r = resources[id];
            if (item.at("kind") == "buffer") {
                auto desc = pod<D3D11_BUFFER_DESC>(item.at("desc"));
                auto bytes = payload(root, item.at("data"));
                require(bytes.size() == desc.ByteWidth && desc.ByteWidth, "Buffer length mismatch");
                total += bytes.size();
                require(total <= 2ull * 1024 * 1024 * 1024, "Fixture exceeds memory bound");
                const D3D11_SUBRESOURCE_DATA data{bytes.data(), 0, 0};
                ComPtr<ID3D11Buffer> buffer;
                check(gpu.device->CreateBuffer(&desc, &data, &buffer), "Create fixture buffer");
                r.object = buffer;
            } else {
                require(item.at("kind") == "texture2d", "Unsupported fixture resource");
                r.desc = pod<D3D11_TEXTURE2D_DESC>(item.at("desc"));
                const auto &d = r.desc;
                require(d.Width && d.Height && d.Width <= 8192 && d.Height <= 8192 && d.ArraySize &&
                            d.ArraySize <= 6 && d.MipLevels && d.MipLevels <= 14,
                        "Texture dimensions exceed bound");
                ComPtr<ID3D11Texture2D> texture;
                if (d.SampleDesc.Count > 1) {
                    require(d.SampleDesc.Count <= 8 && d.SampleDesc.Quality == 0 && d.ArraySize == 1 &&
                                d.MipLevels == 1,
                            "Unsupported MSAA shape");
                    Gpu::access(r);
                    count(item.at("samples"), d.SampleDesc.Count);
                    count(item.at("expected_after_draw"), d.SampleDesc.Count);
                    for (const auto &sample : item.at("samples")) {
                        auto bytes = payload(root, sample);
                        require(bytes.size() == uint64_t(d.Width) * d.Height * Gpu::pixelBytes(r),
                                "Sample size mismatch");
                        total += bytes.size();
                        require(total <= 2ull * 1024 * 1024 * 1024, "Fixture exceeds memory bound");
                        r.samples.push_back(std::move(bytes));
                    }
                    check(gpu.device->CreateTexture2D(&d, nullptr, &texture), "Create fixture MSAA texture");
                } else {
                    require(d.SampleDesc.Count == 1, "Invalid sample count");
                    auto bytes = payload(root, item.at("data"));
                    total += bytes.size();
                    require(total <= 2ull * 1024 * 1024 * 1024, "Fixture exceeds memory bound");
                    count(item.at("subresources"), size_t(d.ArraySize) * d.MipLevels);
                    std::vector<D3D11_SUBRESOURCE_DATA> data;
                    size_t expectedOffset = 0;
                    for (const auto &sub : item.at("subresources")) {
                        auto offset = sub.at("offset").get<uint64_t>(), size = sub.at("size").get<uint64_t>();
                        require(offset <= bytes.size() && size <= bytes.size() - offset && size &&
                                    size <= UINT32_MAX,
                                "Subresource range exceeds payload");
                        auto row = sub.at("row").get<UINT>();
                        const UINT mip = UINT(data.size()) % d.MipLevels;
                        const UINT width = std::max(1u, d.Width >> mip),
                                   height = std::max(1u, d.Height >> mip);
                        UINT expectedRow{}, rows = height;
                        switch (d.Format) {
                        case DXGI_FORMAT_R16G16B16A16_FLOAT:
                            expectedRow = width * 8;
                            break;
                        case DXGI_FORMAT_R16_TYPELESS:
                            expectedRow = width * 2;
                            break;
                        case DXGI_FORMAT_BC6H_UF16:
                        case DXGI_FORMAT_BC7_UNORM:
                        case DXGI_FORMAT_BC7_UNORM_SRGB:
                            expectedRow = ((width + 3) / 4) * 16;
                            rows = (height + 3) / 4;
                            break;
                        default:
                            throw std::runtime_error("Unsupported fixture input texture format");
                        }
                        require(row == expectedRow && size == uint64_t(row) * rows &&
                                    offset == expectedOffset,
                                "Subresource layout differs from its descriptor");
                        expectedOffset += size_t(size);
                        data.push_back({bytes.data() + offset, row, UINT(size)});
                    }
                    require(expectedOffset == bytes.size(), "Unexpected trailing texture bytes");
                    check(gpu.device->CreateTexture2D(&d, data.data(), &texture), "Create fixture texture");
                }
                r.object = texture;
                const float lod = item.at("min_lod").get<float>();
                if (d.MiscFlags & D3D11_RESOURCE_MISC_RESOURCE_CLAMP)
                    gpu.context->SetResourceMinLOD(texture.Get(), lod);
                else
                    require(lod == 0, "MinLOD requires resource-clamp flag");
            }
        }
        auto resource = [&](const Json &id) -> ID3D11Resource * {
            auto value = id.get<uint64_t>();
            return value ? resources.at(value).object.Get() : nullptr;
        };
        auto buffer = [&](const Json &id) {
            ComPtr<ID3D11Buffer> value;
            if (auto raw = resource(id))
                check(raw->QueryInterface(IID_PPV_ARGS(&value)), "Expected buffer");
            return value;
        };
        auto vsBytes = payload(root, bundle.at("vs").at("shader")),
             psBytes = payload(root, bundle.at("ps").at("shader"));
        for (const auto *bytes : {&vsBytes, &psBytes}) {
            ComPtr<ID3D11ShaderReflection> reflection;
            check(D3DReflect(bytes->data(), bytes->size(), IID_PPV_ARGS(&reflection)),
                  "Reflect fixture shader");
            D3D11_SHADER_DESC desc{};
            check(reflection->GetDesc(&desc), "Get shader description");
            require(D3D11_SHVER_GET_MAJOR(desc.Version) == 4, "Fixture requires shader model 4");
        }
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        check(gpu.device->CreateVertexShader(vsBytes.data(), vsBytes.size(), nullptr, &vs),
              "Create fixture VS");
        check(gpu.device->CreatePixelShader(psBytes.data(), psBytes.size(), nullptr, &ps),
              "Create fixture PS");
        auto signature = payload(root, bundle.at("layout_signature"));
        const auto &layout = bundle.at("layout");
        require(layout.is_array() && !layout.empty() && layout.size() <= 32, "Invalid layout count");
        std::vector<std::string> semantics;
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        semantics.reserve(layout.size());
        for (const auto &element : layout) {
            semantics.push_back(element.at("semantic").get<std::string>());
            require(semantics.back().size() < 128 && semantics.back().find('\0') == std::string::npos,
                    "Invalid semantic");
            count(element.at("fields"), 6);
            auto f = element.at("fields").get<std::array<UINT, 6>>();
            elements.push_back({semantics.back().c_str(), f[0], DXGI_FORMAT(f[1]), f[2], f[3],
                                D3D11_INPUT_CLASSIFICATION(f[4]), f[5]});
        }
        ComPtr<ID3D11InputLayout> input;
        check(gpu.device->CreateInputLayout(elements.data(), UINT(elements.size()), signature.data(),
                                            signature.size(), &input),
              "Create fixture layout");
        auto rs = pod<D3D11_RASTERIZER_DESC>(bundle.at("raster"));
        ComPtr<ID3D11RasterizerState> raster;
        check(gpu.device->CreateRasterizerState(&rs, &raster), "Create fixture rasterizer");
        auto bs = pod<D3D11_BLEND_DESC>(bundle.at("blend"));
        if (floatRt0 || unblendedRt0)
            bs.RenderTarget[0].BlendEnable = FALSE;
        ComPtr<ID3D11BlendState> blend;
        check(gpu.device->CreateBlendState(&bs, &blend), "Create fixture blend");
        auto ds = pod<D3D11_DEPTH_STENCIL_DESC>(bundle.at("depth"));
        ComPtr<ID3D11DepthStencilState> depth;
        check(gpu.device->CreateDepthStencilState(&ds, &depth), "Create fixture depth state");
        std::vector<D3D11_VIEWPORT> viewports;
        std::vector<D3D11_RECT> scissors;
        require(bundle.at("viewports").size() <= 16 && bundle.at("scissors").size() <= 16,
                "Raster count exceeds bound");
        for (const auto &v : bundle.at("viewports"))
            viewports.push_back(pod<D3D11_VIEWPORT>(v));
        for (const auto &v : bundle.at("scissors"))
            scissors.push_back(pod<D3D11_RECT>(v));
        std::array<ComPtr<ID3D11RenderTargetView>, 8> rtvs;
        count(bundle.at("rtv"), 8);
        for (size_t i = 0; i < 8; ++i)
            if (!bundle.at("rtv").at(i).is_null()) {
                const auto &v = bundle.at("rtv").at(i);
                require(!resources.at(v.at("resource").get<uint64_t>()).samples.empty(),
                        "Every output must have restorable MSAA samples");
                auto d = pod<D3D11_RENDER_TARGET_VIEW_DESC>(v.at("desc"));
                check(gpu.device->CreateRenderTargetView(resource(v.at("resource")), &d, &rtvs[i]),
                      "Create fixture RTV");
            }
        ComPtr<ID3D11DepthStencilView> dsv;
        if (!bundle.at("dsv").is_null()) {
            const auto &v = bundle.at("dsv");
            require(!resources.at(v.at("resource").get<uint64_t>()).samples.empty(),
                    "Depth output must have restorable MSAA samples");
            auto d = pod<D3D11_DEPTH_STENCIL_VIEW_DESC>(v.at("desc"));
            check(gpu.device->CreateDepthStencilView(resource(v.at("resource")), &d, &dsv),
                  "Create fixture DSV");
        }
        Resource floatOutput;
        if (floatRt0) {
            floatOutput.desc = resources.at((*pixel)[0]).desc;
            floatOutput.desc.Format = DXGI_FORMAT_R32G32B32A32_TYPELESS;
            floatOutput.desc.Usage = D3D11_USAGE_DEFAULT;
            floatOutput.desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            floatOutput.desc.CPUAccessFlags = floatOutput.desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> texture;
            check(gpu.device->CreateTexture2D(&floatOutput.desc, nullptr, &texture), "Create float output");
            floatOutput.object = texture;
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            rtvs[0].Reset();
            check(gpu.device->CreateRenderTargetView(texture.Get(), &desc, &rtvs[0]),
                  "Create float output RTV");
        }
        struct Stage {
            std::array<ComPtr<ID3D11SamplerState>, 16> samplers;
            std::array<ComPtr<ID3D11ShaderResourceView>, 128> srvs;
        } stages[2];
        for (UINT s = 0; s < 2; ++s) {
            const auto &st = bundle.at(s ? "ps" : "vs");
            count(st.at("samplers"), 16);
            count(st.at("srv"), 128);
            count(st.at("cb"), 14);
            for (size_t i = 0; i < 16; ++i)
                if (!st.at("samplers").at(i).is_null()) {
                    auto d = pod<D3D11_SAMPLER_DESC>(st.at("samplers").at(i));
                    check(gpu.device->CreateSamplerState(&d, &stages[s].samplers[i]),
                          "Create fixture sampler");
                }
            for (size_t i = 0; i < 128; ++i)
                if (!st.at("srv").at(i).is_null()) {
                    const auto &v = st.at("srv").at(i);
                    auto d = pod<D3D11_SHADER_RESOURCE_VIEW_DESC>(v.at("desc"));
                    check(gpu.device->CreateShaderResourceView(resource(v.at("resource")), &d,
                                                               &stages[s].srvs[i]),
                          "Create fixture SRV");
                }
        }
        count(bundle.at("vb"), 32);
        count(bundle.at("blend_factor"), 4);
        count(bundle.at("draw"), 3);
        auto draw = bundle.at("draw").get<std::array<UINT, 3>>();
        require(draw[0] && draw[0] <= 1000000, "Draw count exceeds bound");
        report["original_draw"] = draw;
        if (indices) {
            draw[0] = (*indices)[1];
            draw[1] += (*indices)[0];
        }
        report["applied_draw"] = draw;
        auto addPixel = [&](Json &result, uint64_t id, const Resource &r, const Bytes &actual,
                            const Bytes &expected) {
            if (pixel && (*pixel)[0] == id) {
                const size_t first = (size_t((*pixel)[2]) * r.desc.Width + (*pixel)[1]) * Gpu::pixelBytes(r);
                const auto length = Gpu::pixelBytes(r);
                result["actual_pixel_bytes"] = Bytes(actual.begin() + first, actual.begin() + first + length);
                result["expected_pixel_bytes"] =
                    Bytes(expected.begin() + first, expected.begin() + first + length);
            }
        };
        auto bind = [&] {
            auto *c = gpu.context.Get();
            c->ClearState();
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            c->IASetInputLayout(input.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(bundle.at("topology").get<UINT>()));
            for (UINT i = 0; i < 32; ++i) {
                const auto &v = bundle.at("vb").at(i);
                auto b = buffer(v.at("resource"));
                auto raw = b.Get();
                UINT stride = v.at("stride"), offset = v.at("offset");
                c->IASetVertexBuffers(i, 1, &raw, &stride, &offset);
            }
            const auto &ib = bundle.at("ib");
            auto index = buffer(ib.at("resource"));
            c->IASetIndexBuffer(index.Get(), DXGI_FORMAT(ib.at("format").get<UINT>()), ib.at("offset"));
            for (UINT s = 0; s < 2; ++s) {
                const auto &st = bundle.at(s ? "ps" : "vs");
                for (UINT i = 0; i < 14; ++i) {
                    const auto &cb = st.at("cb").at(i);
                    auto b = buffer(cb.at("resource"));
                    auto raw = b.Get();
                    UINT first = cb.at("first"), n = cb.at("count");
                    if (s)
                        gpu.context1->PSSetConstantBuffers1(i, 1, &raw, &first, &n);
                    else
                        gpu.context1->VSSetConstantBuffers1(i, 1, &raw, &first, &n);
                }
                ID3D11SamplerState *samplers[16]{};
                ID3D11ShaderResourceView *srvs[128]{};
                for (UINT i = 0; i < 16; ++i)
                    samplers[i] = stages[s].samplers[i].Get();
                for (UINT i = 0; i < 128; ++i)
                    srvs[i] = stages[s].srvs[i].Get();
                if (s) {
                    c->PSSetSamplers(0, 16, samplers);
                    c->PSSetShaderResources(0, 128, srvs);
                } else {
                    c->VSSetSamplers(0, 16, samplers);
                    c->VSSetShaderResources(0, 128, srvs);
                }
            }
            c->RSSetState(raster.Get());
            c->RSSetViewports(UINT(viewports.size()), viewports.data());
            c->RSSetScissorRects(UINT(scissors.size()), scissors.data());
            auto factor = bundle.at("blend_factor").get<std::array<float, 4>>();
            c->OMSetBlendState(blend.Get(), factor.data(), bundle.at("sample_mask"));
            c->OMSetDepthStencilState(depth.Get(), bundle.at("stencil_ref"));
            ID3D11RenderTargetView *targets[8]{};
            for (size_t i = 0; i < 8; ++i)
                targets[i] = rtvs[i].Get();
            c->OMSetRenderTargets(8, targets, dsv.Get());
        };
        if (samplePositions) {
            report["sample_positions"] = gpu.samplePositions(resources.at((*pixel)[0]));
            report["sample_positions_source"] =
                "Private resource with the same dimensions/format/sample descriptor";
        }
        report["initialization_debug"] = gpu.messages();
        save();
        for (const auto &message : report["initialization_debug"])
            require(message.at("severity").get<UINT>() > D3D11_MESSAGE_SEVERITY_WARNING,
                    "Fixture initialization emitted debug warnings/errors");
        bool allEqual = true;
        for (UINT repeat = 0; repeat < repeats; ++repeat) {
            for (auto &[id, r] : resources)
                if (!r.samples.empty())
                    gpu.restore(r);
            for (auto &[id, r] : resources)
                for (UINT sample = 0; sample < r.samples.size(); ++sample) {
                    auto actual = gpu.sample(r, sample);
                    auto result = compare(actual, r.samples[sample], Gpu::pixelBytes(r));
                    addPixel(result, id, r, actual, r.samples[sample]);
                    result["phase"] = "restore";
                    result["repeat"] = repeat;
                    result["resource"] = id;
                    result["sample"] = sample;
                    report["comparisons"].push_back(result);
                    if (!result.at("exact").get<bool>()) {
                        std::ofstream file(output / "restore-mismatch.bin", std::ios::binary);
                        file.exceptions(std::ios::badbit | std::ios::failbit);
                        file.write(reinterpret_cast<const char *>(actual.data()), actual.size());
                        file.close();
                        report["restore_debug"] = gpu.messages();
                    }
                    save();
                    require(result.at("exact"), "MSAA restoration is not byte exact");
                }
            bind();
            if (floatRt0) {
                const float zero[4]{};
                gpu.context->ClearRenderTargetView(rtvs[0].Get(), zero);
            }
            if (!skipDraw)
                gpu.context->DrawIndexed(draw[0], draw[1], std::bit_cast<INT>(draw[2]));
            if (floatRt0) {
                for (UINT sample = 0; sample < floatOutput.desc.SampleDesc.Count; ++sample) {
                    auto bytes = gpu.sample(floatOutput, sample);
                    const size_t offset = (size_t((*pixel)[2]) * floatOutput.desc.Width + (*pixel)[1]) * 16;
                    std::array<uint32_t, 4> words{};
                    std::memcpy(words.data(), bytes.data() + offset, 16);
                    report["float_output"].push_back({{"repeat", repeat},
                                                      {"sample", sample},
                                                      {"sha256", hash(bytes)},
                                                      {"pixel_words", words}});
                }
                save();
            }
            for (auto &[id, r] : resources)
                for (UINT sample = 0; sample < r.samples.size(); ++sample) {
                    auto actual = gpu.sample(r, sample);
                    const auto expected = payload(
                        root,
                        bundle.at("resources").at(std::to_string(id)).at("expected_after_draw").at(sample));
                    auto result = compare(actual, expected, Gpu::pixelBytes(r));
                    addPixel(result, id, r, actual, expected);
                    allEqual &= result.at("exact").get<bool>();
                    result["phase"] = "draw";
                    result["repeat"] = repeat;
                    result["resource"] = id;
                    result["sample"] = sample;
                    if (!result.at("exact").get<bool>() && !summaryOnly) {
                        const auto name = std::to_string(repeat) + "-" + std::to_string(id) + "-s" +
                                          std::to_string(sample) + ".bin";
                        std::ofstream f(output / name, std::ios::binary);
                        f.exceptions(std::ios::badbit | std::ios::failbit);
                        f.write(reinterpret_cast<const char *>(actual.data()), actual.size());
                        f.close();
                        result["actual_file"] = name;
                    }
                    report["comparisons"].push_back(result);
                    save();
                }
            check(gpu.device->GetDeviceRemovedReason(), "Fixture device status");
            auto messages = gpu.messages();
            report["debug"].push_back(messages);
            save();
            for (const auto &m : messages)
                require(m.at("severity").get<UINT>() > D3D11_MESSAGE_SEVERITY_WARNING,
                        "Fixture draw emitted debug warnings/errors");
            std::cout << "Completed repeat " << repeat << '\n';
        }
        HMODULE modules[1024]{};
        DWORD needed{};
        require(EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed) &&
                    needed <= sizeof(modules),
                "Module inventory failed");
        for (UINT i = 0; i < needed / sizeof(HMODULE); ++i) {
            wchar_t name[32768]{};
            require(GetModuleFileNameW(modules[i], name, 32768) > 0, "Module path unavailable");
            report["loaded_modules"].push_back(fs::path(name).generic_string());
        }
        report["completed"] = true;
        report["all_exact"] = allEqual;
        save();
        return allEqual ? 0 : 3;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        report["error"] = error.what();
        try {
            save();
        } catch (...) {
        }
        return 1;
    }
}
