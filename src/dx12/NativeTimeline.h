#pragma once
#include "NativeResources.h"
#include <map>
#include <wrl/client.h>
namespace flora::dx12 {
// Timestamp brackets belong to submitted instances, never recording generations.
// CPU calibration pairs are collected around each actual replay submission.
// Separate timestamp-only lists bracket queue Waits. Their elapsed time includes
// helper/submission overhead and is not an attribution of pure Fence stall time.
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
    struct WaitBracket {
        uint64_t queue{};
        D3D12_COMMAND_LIST_TYPE type{};
        uint32_t slot{};
        bool supported{}, ended{};
        // Both lists use one allocator on the same queue; never reset it while
        // a captured Wait or its post-Wait timestamp can still be in flight.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> before, after;
        nlohmann::json calibration;
    };
    ID3D12Device *device_;
    uint32_t capacity_, queryCapacity_, waitCapacity_;
    uint64_t origin_{}, cpuFrequency_{};
    bool copySupported_{};
    std::map<D3D12_COMMAND_LIST_TYPE, Storage> storage_;
    std::vector<Entry> entries_;
    std::map<uint64_t, nlohmann::json> submissions_;
    std::map<uint64_t, WaitBracket> waits_;
    nlohmann::json synchronization_ = nlohmann::json::array();
    nlohmann::json calibrate(ID3D12CommandQueue *) const;
    bool timestampsSupported(D3D12_COMMAND_LIST_TYPE) const;
    uint32_t reserve(D3D12_COMMAND_LIST_TYPE);
  public:
    NativeTimeline(ID3D12Device *, size_t capacity, size_t waitCapacity = 0);
    size_t begin(ID3D12GraphicsCommandList *, nlohmann::json identity);
    void end(ID3D12GraphicsCommandList *, size_t entry);
    void beforeSubmit(uint64_t submission, ID3D12CommandQueue *);
    void afterComplete(uint64_t submission, ID3D12CommandQueue *);
    void beforeWait(uint64_t record, uint64_t queue, ID3D12CommandQueue *);
    void afterWait(uint64_t record, ID3D12CommandQueue *);
    void afterQueueComplete(uint64_t queue, ID3D12CommandQueue *);
    void synchronization(uint64_t record, uint64_t queue, uint64_t fence, uint64_t value, bool wait);
    nlohmann::json result() const;
};
}
