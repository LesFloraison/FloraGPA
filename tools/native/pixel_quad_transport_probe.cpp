// Independent D3D11 evidence for lossless four-lane pixel register transport.
#define NOMINMAX
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Word = std::array<uint32_t, 4>;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Quad transport D3D11 failure: " + std::to_string(hr));
}
void save(const std::filesystem::path &path, const void *data, size_t size) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char *>(data), size);
    if (!file)
        throw std::runtime_error("Cannot save quad transport evidence");
}
void saveJson(const std::filesystem::path &path, const Json &value) {
    const auto text = value.dump(2);
    save(path, text.data(), text.size());
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *entry, const char *profile, unsigned samples,
                         bool sampleRate, const std::filesystem::path &path) {
    const auto count = std::to_string(samples);
    const D3D_SHADER_MACRO defines[]{
        {"SAMPLE_COUNT", count.c_str()}, {"SAMPLE_RATE", sampleRate ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "Independent quad transport", defines, nullptr,
                               entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr);
    save(path, code->GetBufferPointer(), code->GetBufferSize());
    ComPtr<ID3DBlob> assembly;
    check(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &assembly));
    save(path.wstring() + L".asm.txt", assembly->GetBufferPointer(), assembly->GetBufferSize());
    return code;
}
void wait(ID3D11Device *device, ID3D11DeviceContext *context) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    check(device->CreateQuery(&desc, &query));
    context->End(query.Get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        BOOL done = false;
        const auto hr = context->GetData(query.Get(), &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && done)
            return;
        check(hr);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Quad transport GPU completion timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void validate(ID3D11InfoQueue *queue) {
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
uint32_t randomWord(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
Json modules() {
    std::array<HMODULE, 1024> values{};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), values.data(), sizeof values, &needed) ||
        needed > sizeof values)
        throw std::runtime_error("Cannot enumerate quad transport modules");
    Json result = Json::array();
    for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
        std::array<wchar_t, 32768> name{};
        const auto length = GetModuleFileNameW(values[i], name.data(), DWORD(name.size()));
        if (!length || length >= name.size())
            throw std::runtime_error("Cannot read quad transport module path");
        const auto size =
            WideCharToMultiByte(CP_UTF8, 0, name.data(), int(length), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            throw std::runtime_error("Cannot encode quad transport module path");
        std::string path(size, '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, name.data(), int(length), path.data(), size, nullptr, nullptr) !=
            size)
            throw std::runtime_error("Cannot encode quad transport module path");
        result.push_back(path);
    }
    return result;
}
constexpr Word multipliers{0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu};
struct Constants {
    std::array<Word, 256> bits;
    std::array<std::array<float, 4>, 3> vertices;
};
struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        std::cerr << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP HLSL_SOURCE\n";
        return 2;
    }
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        std::ifstream input(std::filesystem::path(argv[3]), std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(input)), {});
        if (source.empty())
            throw std::runtime_error("Missing quad transport shader source");
        save(directory / "source.hlsl", source.data(), source.size());
        const auto vendor = uint32_t(std::stoul(argv[2]));
        ComPtr<IDXGIAdapter1> selected;
        if (vendor) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr);
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc));
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested quad transport adapter unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        check(device.As(&dxgi));
        check(dxgi->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC desc{};
        check(adapter->GetDesc(&desc));
        LARGE_INTEGER driver{};
        check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
        ComPtr<ID3D11InfoQueue> queue;
        check(device.As(&queue));
        Json report{{"schema", 1},
                    {"scope", "lossless_quad_transport_research"},
                    {"complete_release_acceptance", false},
                    {"requested_vendor", vendor},
                    {"adapter_vendor", desc.VendorId},
                    {"adapter_device", desc.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart},
                    {"driver_version", driver.QuadPart},
                    {"cases", Json::array()},
                    {"unsupported_sample_counts", Json::array()}};
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = true;
        rd.ScissorEnable = true;
        rd.MultisampleEnable = true;
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rd, &raster));
        D3D11_DEPTH_STENCIL_DESC dd{};
        ComPtr<ID3D11DepthStencilState> depth;
        check(device->CreateDepthStencilState(&dd, &depth));
        D3D11_BUFFER_DESC cbd{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&cbd, nullptr, &constants));
        auto vsCode = compile(source, "VSMain", "vs_5_0", 1, false, directory / "vertex.dxbc");
        ComPtr<ID3D11VertexShader> vs;
        check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        bool all = true;
        for (unsigned samples : {1u, 2u, 4u, 8u}) {
            UINT quality = 0;
            check(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32G32B32A32_UINT, samples, &quality));
            if (!quality) {
                report["unsupported_sample_counts"].push_back(samples);
                continue;
            }
            std::array<Target, 9> targets;
            for (auto &target : targets) {
                const D3D11_TEXTURE2D_DESC td{16,
                                              16,
                                              1,
                                              1,
                                              DXGI_FORMAT_R32G32B32A32_UINT,
                                              {samples, 0},
                                              D3D11_USAGE_DEFAULT,
                                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                              0,
                                              0};
                check(device->CreateTexture2D(&td, nullptr, &target.texture));
                check(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv));
                check(device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv));
            }
            const UINT bytes = 16 * 16 * samples * sizeof(Word);
            D3D11_BUFFER_DESC bd{bytes,
                                 D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_UNORDERED_ACCESS,
                                 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                 sizeof(Word)};
            ComPtr<ID3D11Buffer> output, staging;
            check(device->CreateBuffer(&bd, nullptr, &output));
            ComPtr<ID3D11UnorderedAccessView> uav;
            check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bd.MiscFlags = bd.StructureByteStride = 0;
            check(device->CreateBuffer(&bd, nullptr, &staging));
            auto csCode = compile(source, "ReadbackMain", "cs_5_0", samples, false,
                                  directory / ("readback-" + std::to_string(samples) + ".dxbc"));
            ComPtr<ID3D11ComputeShader> cs;
            check(device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr,
                                              &cs));
            for (bool sampleRate : {false, true}) {
                const auto prefix = std::to_string(samples) + "-" + std::to_string(sampleRate);
                auto psCode = compile(source, "PSMain", "ps_5_0", samples, sampleRate,
                                      directory / (prefix + "-capture.dxbc"));
                auto refCode = compile(source, "ReferenceMain", "ps_5_0", samples, sampleRate,
                                       directory / (prefix + "-reference.dxbc"));
                ComPtr<ID3D11PixelShader> ps, reference;
                check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr,
                                                &ps));
                check(device->CreatePixelShader(refCode->GetBufferPointer(), refCode->GetBufferSize(),
                                                nullptr, &reference));
                for (unsigned mode = 0; mode < 8; ++mode)
                    for (unsigned dataset = 0; dataset < 2; ++dataset) {
                        const auto name = prefix + "-" + std::to_string(mode) + "-" + std::to_string(dataset);
                        Constants data{};
                        uint32_t random = 0x13579bdfu;
                        const std::array<uint32_t, 16> special{
                            0,          0x80000000, 1,          0x007fffff, 0x00800000, 0x7f800000,
                            0xff800000, 0x7fc12345, 0x7f812345, 0xffffffff, 0xdeadbeef, 0x01234567,
                            0x89abcdef, 0xaaaaaaaa, 0x55555555, 0x3f800000};
                        for (unsigned p = 0; p < 256; ++p)
                            for (unsigned c = 0; c < 4; ++c)
                                data.bits[p][c] =
                                    dataset ? randomWord(random) : special[(p * 7 + c * 3) % special.size()];
                        data.vertices = {{{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
                        const D3D11_VIEWPORT viewport{
                            mode == 7 ? 1.25f : 0.f, mode == 7 ? 1.75f : 0.f, 12, 12, 0, 1};
                        if (mode == 5 || mode == 6) {
                            const std::array<std::array<float, 2>, 3> points =
                                mode == 5 ? std::array<std::array<float, 2>, 3>{{{4.1f, 4.1f},
                                                                                 {4.95f, 4.1f},
                                                                                 {4.1f, 4.95f}}}
                                          : std::array<std::array<float, 2>, 3>{
                                                {{5.3f, 5.2f}, {7.1f, 5.2f}, {5.3f, 7.1f}}};
                            for (unsigned i = 0; i < 3; ++i)
                                data.vertices[i] = {2 * points[i][0] / 12 - 1, 1 - 2 * points[i][1] / 12, 0,
                                                    1};
                        }
                        D3D11_RECT scissor{3, 3, 10, 10};
                        if (mode >= 1 && mode <= 4) {
                            const LONG x = 4 + LONG((mode - 1) & 1), y = 4 + LONG((mode - 1) >> 1);
                            scissor = {x, y, x + 1, y + 1};
                        }
                        const UINT sampleMask = mode == 6 ? 0x5u : UINT32_MAX;
                        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
                        save(directory / (name + "-constants.bin"), &data, sizeof data);
                        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
                        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
                        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        context->VSSetShader(vs.Get(), nullptr, 0);
                        context->RSSetState(raster.Get());
                        context->RSSetViewports(1, &viewport);
                        context->RSSetScissorRects(1, &scissor);
                        context->OMSetDepthStencilState(depth.Get(), 0);
                        context->OMSetBlendState(nullptr, nullptr, sampleMask);
                        const float clear[4]{16777216, 16777216, 16777216, 16777216};
                        for (auto &target : targets)
                            context->ClearRenderTargetView(target.rtv.Get(), clear);
                        std::array<ID3D11RenderTargetView *, 8> views{};
                        for (unsigned i = 0; i < 8; ++i)
                            views[i] = targets[i].rtv.Get();
                        context->OMSetRenderTargets(8, views.data(), nullptr);
                        context->PSSetShader(ps.Get(), nullptr, 0);
                        context->Draw(3, 0);
                        context->OMSetRenderTargets(1, targets[8].rtv.GetAddressOf(), nullptr);
                        context->PSSetShader(reference.Get(), nullptr, 0);
                        context->Draw(3, 0);
                        context->OMSetRenderTargets(0, nullptr, nullptr);
                        context->CSSetShader(cs.Get(), nullptr, 0);
                        std::array<std::vector<Word>, 9> observed;
                        for (unsigned target = 0; target < 9; ++target) {
                            context->CSSetShaderResources(0, 1, targets[target].srv.GetAddressOf());
                            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                            context->Dispatch(2, 2, 1);
                            ID3D11UnorderedAccessView *nullUav = nullptr;
                            ID3D11ShaderResourceView *nullSrv = nullptr;
                            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
                            context->CSSetShaderResources(0, 1, &nullSrv);
                            context->CopyResource(staging.Get(), output.Get());
                            wait(device.Get(), context.Get());
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
                                               &mapped));
                            observed[target].resize(256 * samples);
                            std::memcpy(observed[target].data(), mapped.pData, bytes);
                            context->Unmap(staging.Get(), 0);
                            save(directory / (name + "-target-" + std::to_string(target) + ".bin"),
                                 observed[target].data(), bytes);
                        }
                        validate(queue.Get());
                        Json item{{"name", name},
                                  {"samples", samples},
                                  {"sample_rate", sampleRate},
                                  {"mode", mode},
                                  {"dataset", dataset},
                                  {"sample_mask", sampleMask},
                                  {"written_samples", 0},
                                  {"neighbor_components", 0},
                                  {"nonwriting_neighbors", 0},
                                  {"origins", Json::array()},
                                  {"mismatches", Json::array()}};
                        std::set<std::pair<unsigned, unsigned>> origins;
                        auto fail = [&](const std::string &why, unsigned address) {
                            all = false;
                            if (item["mismatches"].size() < 16)
                                item["mismatches"].push_back({{"reason", why}, {"address", address}});
                        };
                        for (unsigned address = 0; address < 256 * samples; ++address) {
                            const auto location = observed[4][address];
                            const bool captured = location[0] < 16 && location[1] < 16;
                            const bool referenced =
                                observed[8][address] != Word{16777216, 16777216, 16777216, 16777216};
                            if (captured != referenced)
                                fail("Capture/reference write footprints differ", address);
                            if (!captured)
                                continue;
                            const unsigned p = address / samples, x = p % 16, y = p / 16,
                                           sample = address % samples;
                            item["written_samples"] = item["written_samples"].get<unsigned>() + 1;
                            if (location[0] != x || location[1] != y || location[2] > 1 || location[3] > 1 ||
                                x < location[2] || y < location[3]) {
                                fail("Invalid measured location/role", address);
                                continue;
                            }
                            const auto ox = x - location[2], oy = y - location[3];
                            origins.emplace(ox, oy);
                            const auto direction = observed[7][address];
                            if (direction[0] != std::bit_cast<uint32_t>(location[2] ? -1.f : 1.f) ||
                                direction[1] != std::bit_cast<uint32_t>(location[3] ? -1.f : 1.f) ||
                                (direction[2] & 0x7fffffffu) || (direction[3] & 0x7fffffffu))
                                fail("Invalid orientation derivatives", address);
                            const auto currentLane = location[2] + 2 * location[3];
                            if (observed[5][address][currentLane] != (sampleRate ? sample : 0u) ||
                                !(observed[6][address][currentLane] & (1u << sample)) ||
                                !(sampleMask & (1u << sample)))
                                fail("Current sample identity or coverage differs", address);
                            if (observed[location[2] + 2 * location[3]][address] != observed[8][address])
                                fail("Current lane differs from independent shader", address);
                            for (unsigned lane = 0; lane < 4; ++lane) {
                                const unsigned nx = ox + (lane & 1), ny = oy + (lane >> 1),
                                               sid = observed[5][address][lane];
                                if (nx >= 16 || ny >= 16 || sid >= (sampleRate ? samples : 1u)) {
                                    fail("Neighbor coordinates or sample ID outside probe domain", address);
                                    continue;
                                }
                                Word expected = data.bits[ny * 16 + nx];
                                for (unsigned c = 0; c < 4; ++c)
                                    expected[c] ^= sid * multipliers[c];
                                if (observed[lane][address] != expected)
                                    fail("Transported neighbor bits differ", address);
                                item["neighbor_components"] = item["neighbor_components"].get<unsigned>() + 4;
                                const auto neighbor = (ny * 16 + nx) * samples + sample;
                                if (observed[4][neighbor][0] >= 16)
                                    item["nonwriting_neighbors"] =
                                        item["nonwriting_neighbors"].get<unsigned>() + 1;
                            }
                        }
                        for (const auto &[x, y] : origins)
                            item["origins"].push_back({x, y});
                        if (!item["written_samples"].get<unsigned>())
                            fail("No observed output in a positive probe", 0);
                        item["passed"] = item["mismatches"].empty();
                        report["cases"].push_back(item);
                        report["passed"] = all;
                        saveJson(directory / "report.json", report);
                    }
            }
        }
        report["loaded_modules"] = modules();
        saveJson(directory / "report.json", report);
        std::cout << Json{{"passed", all}, {"cases", report["cases"].size()}}.dump() << "\n";
        return all ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
