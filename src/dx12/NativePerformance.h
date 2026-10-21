#pragma once
#include "NativeResources.h"
#include <wrl/client.h>
namespace flora::dx12 {
// Timestamp-only batch owned by one submitted list instance. Resolve once at
// the list tail, and read only after all replay queues have completed.
class NativeDrawTimings final {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    D3D12_COMMAND_LIST_TYPE type_;
    UINT capacity_{}, count_{};
    bool open_{}, resolved_{};
    nlohmann::json entries_ = nlohmann::json::array();
  public:
    NativeDrawTimings(ID3D12Device *, D3D12_COMMAND_LIST_TYPE, size_t capacity);
    void begin(ID3D12GraphicsCommandList *, nlohmann::json identity);
    void end(ID3D12GraphicsCommandList *);
    void resolve(ID3D12GraphicsCommandList *);
    nlohmann::json result(ID3D12CommandQueue *) const;
};
// One selected execution instance. Queries belong to its original command list
// and are read only after the replay queue fence has completed.
class NativePerformance final {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_, statistics_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    D3D12_COMMAND_LIST_TYPE type_;
    D3D12_QUERY_TYPE statisticsType_ = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    uint64_t readbackSize_ = 16;
    nlohmann::json meshStatistics_;
    bool begun_{}, ended_{};

  public:
    NativePerformance(ID3D12Device *, D3D12_COMMAND_LIST_TYPE);
    void begin(ID3D12GraphicsCommandList *);
    void end(ID3D12GraphicsCommandList *);
    nlohmann::json result(ID3D12CommandQueue *) const;
};
} // namespace flora::dx12
