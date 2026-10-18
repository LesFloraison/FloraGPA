#pragma once
#include "NativeResources.h"
#include <wrl/client.h>
namespace flora::dx12 {
// One selected execution instance. Queries belong to its original command list
// and are read only after the replay queue fence has completed.
class NativePerformance final {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_, statistics_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    D3D12_COMMAND_LIST_TYPE type_;
    bool begun_{}, ended_{};

  public:
    NativePerformance(ID3D12Device *, D3D12_COMMAND_LIST_TYPE);
    void begin(ID3D12GraphicsCommandList *);
    void end(ID3D12GraphicsCommandList *);
    nlohmann::json result(ID3D12CommandQueue *) const;
};
} // namespace flora::dx12
