#pragma once
#define NOMINMAX
#include <d3d11.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <wrl/client.h>
namespace flora {
struct Dx11Device {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    bool warp{};
    nlohmann::json information() const;
};
Dx11Device createDx11Device(bool warp = false, bool debug = false, std::optional<uint32_t> vendor = {});
} // namespace flora
