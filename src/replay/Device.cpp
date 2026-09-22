#include "Device.h"
#include "Replay.h"
#include <dxgi1_2.h>
#include <sstream>
namespace flora {
Dx11Device createDx11Device(bool warp, bool debug, std::optional<uint32_t> vendor) {
    if (warp && vendor)
        throw std::runtime_error("Vendor selection cannot use a software device");
    Dx11Device result;
    result.warp = warp;
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        for (UINT index = 0; index < 16; ++index) {
            Com<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(index, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "GetDesc1");
            if (desc.VendorId == *vendor) {
                selected = adapter;
                break;
            }
        }
        if (!selected) {
            std::ostringstream message;
            message << "No DXGI adapter for vendor 0x" << std::hex << *vendor;
            throw std::runtime_error(message.str());
        }
    }
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                     D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                     D3D_FEATURE_LEVEL_9_1};
    const auto driver = vendor ? D3D_DRIVER_TYPE_UNKNOWN
                        : warp ? D3D_DRIVER_TYPE_WARP
                               : D3D_DRIVER_TYPE_HARDWARE;
    const auto flags = debug ? D3D11_CREATE_DEVICE_DEBUG : 0u;
    auto hr = D3D11CreateDevice(selected.Get(), driver, nullptr, flags, levels, vendor ? 2 : 7,
                                D3D11_SDK_VERSION, &result.device, &result.level, &result.context);
    if (hr == E_INVALIDARG && !vendor)
        hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &result.device,
                               &result.level, &result.context);
    check(hr, "D3D11CreateDevice");
    return result;
}
nlohmann::json Dx11Device::information() const {
    Com<IDXGIDevice> dxgi;
    Com<IDXGIAdapter> adapter;
    check(device.As(&dxgi), "IDXGIDevice");
    check(dxgi->GetAdapter(&adapter), "GetAdapter");
    DXGI_ADAPTER_DESC desc{};
    check(adapter->GetDesc(&desc), "GetAdapterDesc");
    const int size = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        throw std::runtime_error("Cannot convert DXGI adapter description");
    std::string description(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, description.data(), size, nullptr, nullptr);
    description.pop_back();
    std::ostringstream feature;
    feature << "0x" << std::hex << level;
    return {{"selected", warp ? "warp" : "hardware"},
            {"software", warp},
            {"measurement_scope", warp ? "software_device" : "hardware_device"},
            {"feature_level", feature.str()},
            {"adapter",
             {{"description", description},
              {"vendor_id", desc.VendorId},
              {"device_id", desc.DeviceId},
              {"revision", desc.Revision},
              {"dedicated_video_memory", desc.DedicatedVideoMemory},
              {"shared_system_memory", desc.SharedSystemMemory}}}};
}
} // namespace flora
