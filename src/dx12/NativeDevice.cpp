#include "NativeDevice.h"
#include "StreamArguments.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <psapi.h>
#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <charconv>
#include <sstream>
#include <algorithm>
#include <map>

namespace flora::dx12 {
using Microsoft::WRL::ComPtr;
namespace {
std::string hex(HRESULT hr) { std::ostringstream out; out << "0x" << std::hex << uint32_t(hr); return out.str(); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed: " + hex(hr));
}
struct Sdk { uint32_t version{}; std::filesystem::path path; };
Sdk sdkForCapture(const std::filesystem::path &capture) {
    Sdk selected;
    const auto root = capture / "D3D12" / "AgilitySDK";
    if (!std::filesystem::is_directory(root)) throw std::runtime_error("Capture has no Agility SDK directory");
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / "D3D12Core.dll")) continue;
        const auto name = entry.path().filename().string(); uint32_t version{};
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), version);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || !version) continue;
        if (selected.version) throw std::runtime_error("Ambiguous capture SDK: multiple versions need explicit selection");
        selected = {version, std::filesystem::canonical(entry.path())};
    }
    if (!selected.version) throw std::runtime_error("Capture Agility SDK was not found");
    return selected;
}
std::string ansiPath(const std::filesystem::path &path) {
    // The SDK API accepts a narrow Windows path. Reject lossy conversion.
    auto wide = path.wstring() + L"\\"; BOOL substituted = FALSE;
    const auto page = GetACP(); const auto flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    auto substitution = page == CP_UTF8 ? nullptr : &substituted;
    const auto length = WideCharToMultiByte(page, flags, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, substitution);
    if (!length || substituted) throw std::runtime_error("SDK path cannot be represented in the Windows code page");
    std::string out(size_t(length), '\0');
    if (!WideCharToMultiByte(page, flags, wide.c_str(), int(wide.size()), out.data(), length, nullptr, substitution) || substituted)
        throw std::runtime_error("SDK path conversion failed");
    return out;
}
}
struct NativeDevice::Impl {
    ComPtr<ID3D12SDKConfiguration> sdk;
    ComPtr<ID3D12SDKConfiguration1> sdk1;
    ComPtr<ID3D12DeviceFactory> factory;
    ComPtr<ID3D12Device> device;
    nlohmann::json report;
    ~Impl() { device.Reset(); factory.Reset(); if (sdk1) sdk1->FreeUnusedSDKs(); }
};
NativeDevice::NativeDevice(const StreamDirectory &directory, bool debug) : impl_(std::make_unique<Impl>()) {
    const auto selected = sdkForCapture(directory.path());
    auto &state = *impl_;
    state.report = {{"requested_sdk_version", selected.version},
        {"sdk_directory", QString::fromStdWString(selected.path.wstring()).toStdString()},
        {"status", "device_created"}, {"replay_executed", false}};
    D3D_FEATURE_LEVEL minimum = D3D_FEATURE_LEVEL_11_0;
    bool capturedMinimum = false;
    for (const auto &packet : directory.packets()) {
        if (packet.type != 0x1000016b94da9ULL && packet.type != 0x11fac3bb041bfULL) continue;
        const auto args = decodeArguments(packet.type, directory.payload(packet)).values;
        if (int32_t(args.at("HRESULT").get<uint32_t>()) < 0) continue;
        minimum = D3D_FEATURE_LEVEL(std::max(uint32_t(minimum), args.at("MinimumFeatureLevel").get<uint32_t>()));
        capturedMinimum = true;
    }
    if (!capturedMinimum) throw std::runtime_error("Captured DX12 device feature-level requirement is unavailable");
    state.report["captured_minimum_feature_level"] = uint32_t(minimum);
    check(D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&state.sdk)), "D3D12 SDK configuration");
    HRESULT factoryResult = state.sdk.As(&state.sdk1);
    if (SUCCEEDED(factoryResult))
        factoryResult = state.sdk1->CreateDeviceFactory(selected.version, ansiPath(selected.path).c_str(), IID_PPV_ARGS(&state.factory));
    state.report["factory_configuration_hresult"] = hex(factoryResult);
    if (state.factory) state.report["sdk_configuration"] = "ID3D12SDKConfiguration1::CreateDeviceFactory";
    else {
        if (state.sdk1) state.sdk1->FreeUnusedSDKs();
        const auto executable = std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString());
        const auto relative = std::filesystem::relative(selected.path, executable);
        if (relative.empty() || relative.is_absolute()) throw std::runtime_error("Legacy SDK configuration requires a path relative to the Worker executable");
        // SetSDKVersion is intentionally before CreateDevice. It may require
        // Windows Developer Mode; failure is reported, never silently ignored.
        check(state.sdk->SetSDKVersion(selected.version, ansiPath(relative).c_str()), "SetSDKVersion (legacy SDK; requires Developer Mode)");
        state.report["sdk_configuration"] = "ID3D12SDKConfiguration::SetSDKVersion";
    }
    if (debug) {
        ComPtr<ID3D12Debug> validation;
        const auto hr = state.factory ? state.factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&validation)) : D3D12GetDebugInterface(IID_PPV_ARGS(&validation));
        check(hr, "Enable DX12 debug layer"); validation->EnableDebugLayer();
    }
    state.report["debug_layer"] = debug;
    ComPtr<IDXGIFactory6> dxgi;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const auto hr = dxgi->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        check(hr, "EnumAdapterByGpuPreference"); check(candidate->GetDesc1(&description), "GetDesc1");
        if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = std::move(candidate); break; }
    }
    if (!adapter) throw std::runtime_error("No hardware DX12 adapter is available");
    const auto created = state.factory ? state.factory->CreateDevice(adapter.Get(), minimum, IID_PPV_ARGS(&state.device)) :
                                        D3D12CreateDevice(adapter.Get(), minimum, IID_PPV_ARGS(&state.device));
    check(created, "Create DX12 device at the captured feature level");
    state.report["adapter"] = {{"description", QString::fromWCharArray(description.Description).toStdString()},
        {"vendor_id", description.VendorId}, {"device_id", description.DeviceId},
        {"dedicated_video_memory", std::to_string(description.DedicatedVideoMemory)}};
    ComPtr<ID3D12DeviceConfiguration> configuration;
    if (SUCCEEDED(state.device.As(&configuration))) state.report["actual_sdk_version"] = configuration->GetDesc().SDKVersion;
    else {
        std::vector<HMODULE> modules(512); DWORD bytes{};
        HMODULE core{}; size_t cores{};
        if (EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size()*sizeof(HMODULE)), &bytes) && bytes <= modules.size()*sizeof(HMODULE))
            for (size_t i=0; i<bytes/sizeof(HMODULE); ++i) {
                wchar_t name[32768];const auto size=GetModuleFileNameW(modules[i],name,DWORD(std::size(name)));
                if (size && size<std::size(name) && _wcsicmp(std::filesystem::path(name).filename().c_str(),L"D3D12Core.dll")==0) {
                    ++cores;core=modules[i];
                }
            }
        const auto version = cores == 1 ? reinterpret_cast<const UINT *>(GetProcAddress(core, "D3D12SDKVersion")) : nullptr;
        if (version) state.report["actual_sdk_version"] = *version;
        else { state.report["actual_sdk_version"] = nullptr; state.report["sdk_version_note"] = "Runtime does not expose its version"; }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        state.report["features"] = {{"resource_binding_tier", uint32_t(options.ResourceBindingTier)},
            {"resource_heap_tier", uint32_t(options.ResourceHeapTier)}, {"tiled_resources_tier", uint32_t(options.TiledResourcesTier)}};
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
    if (SUCCEEDED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2))))
        state.report["features"]["depth_bounds_test"] = bool(options2.DepthBoundsTestSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        state.report["features"]["mesh_shader_tier"] = uint32_t(options7.MeshShaderTier);
    if (SUCCEEDED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))) {
        state.report["features"]["variable_shading_rate_tier"] = uint32_t(options6.VariableShadingRateTier);
        state.report["features"]["additional_shading_rates"] = bool(options6.AdditionalShadingRatesSupported);
        state.report["features"]["shading_rate_image_tile_size"] = options6.ShadingRateImageTileSize;
    }
    state.report["note"] = "Device initialization only; resource restoration, GPU replay and analysis are not validated";
}
NativeDevice::~NativeDevice() = default;
ID3D12Device *NativeDevice::device() const { return impl_->device.Get(); }
nlohmann::json NativeDevice::information() const {
    auto result = impl_->report; ComPtr<ID3D12InfoQueue> queue;
    if (impl_->device && SUCCEEDED(impl_->device.As(&queue))) {
        auto messages = nlohmann::json::array(); const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
        std::map<std::pair<UINT, UINT>, nlohmann::json> unique; uint64_t errors{};
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length{}; if (FAILED(queue->GetMessage(i, nullptr, &length))) continue;
            std::vector<uint8_t> bytes(length); auto message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
            if (FAILED(queue->GetMessage(i, message, &length)) || message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
            errors += message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
            auto &entry = unique[{UINT(message->Severity), UINT(message->ID)}];
            if (entry.is_null()) entry = {{"severity", UINT(message->Severity)}, {"id", UINT(message->ID)}, {"description", message->pDescription}, {"count", 0ULL}};
            entry["count"] = entry["count"].get<uint64_t>() + 1;
        }
        for (const auto &[key, entry] : unique) if (messages.size() < 32) messages.push_back(entry);
        result["validation_error_count"] = std::to_string(errors);
        result["validation_messages"] = std::move(messages); result["validation_message_count"] = std::to_string(count);
    }
    return result;
}
}
