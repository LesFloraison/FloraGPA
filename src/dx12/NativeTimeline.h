#pragma once
#include "NativeResources.h"
#include <map>
#include <wrl/client.h>
namespace flora::dx12 {
// Timestamp brackets belong to submitted instances, never recording generations.
// CPU calibration pairs are collected around each actual replay submission.
class NativeTimeline final {
    struct Storage {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        uint32_t used{};
    };
    struct Entry {
        nlohmann::json identity;
        D3D12_COMMAND_LIST_TYPE type{};
        uint32_t slot{};
        bool supported{}, ended{};
    };
    ID3D12Device *device_;
    uint32_t capacity_;
    uint64_t origin_{}, cpuFrequency_{};
    bool copySupported_{};
    std::map<D3D12_COMMAND_LIST_TYPE, Storage> storage_;
    std::vector<Entry> entries_;
    std::map<uint64_t, nlohmann::json> submissions_;
    nlohmann::json synchronization_ = nlohmann::json::array();
    nlohmann::json calibrate(ID3D12CommandQueue *) const;
  public:
    NativeTimeline(ID3D12Device *, size_t capacity);
    size_t begin(ID3D12GraphicsCommandList *, nlohmann::json identity);
    void end(ID3D12GraphicsCommandList *, size_t entry);
    void beforeSubmit(uint64_t submission, ID3D12CommandQueue *);
    void afterComplete(uint64_t submission, ID3D12CommandQueue *);
    void synchronization(uint64_t record, uint64_t queue, uint64_t fence, uint64_t value, bool wait);
    nlohmann::json result() const;
};
}
