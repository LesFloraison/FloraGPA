#pragma once
#include "StreamDirectory.h"
#include <nlohmann/json.hpp>
#include <memory>

struct ID3D12Device;
namespace flora::dx12 {
// Construct in the isolated Worker before any other D3D12 device. Older SDKs
// may need process-global SDK configuration; never configure the GUI process.
class NativeDevice final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    explicit NativeDevice(const StreamDirectory &directory);
    ~NativeDevice();
    ID3D12Device *device() const;
    nlohmann::json information() const;
};
}
