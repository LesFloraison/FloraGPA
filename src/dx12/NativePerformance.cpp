#include "NativePerformance.h"
#include <cstddef>
#include <cstring>
#include <sstream>
namespace flora::dx12 {
namespace {
void check(HRESULT status, const char *operation) {
    if (FAILED(status)) {
        std::ostringstream error;
        error << operation << " failed: 0x" << std::hex << uint32_t(status);
        throw std::runtime_error(error.str());
    }
}
} // namespace
NativePerformance::NativePerformance(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) : type_(type) {
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        throw std::runtime_error("Performance queries currently require a direct or compute list");
    D3D12_QUERY_HEAP_DESC query{};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = 2;
    check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps_)), "Create timestamp queries");
    meshStatistics_ = {{"status", "not_sampled_on_compute_list"}};
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS9 options{};
        const auto support = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options, sizeof(options));
        const bool mesh = SUCCEEDED(support) && options.MeshShaderPipelineStatsSupported;
        meshStatistics_ = {{"status", mesh ? "measured" : FAILED(support) ? "capability_unavailable" : "unsupported_by_device"},
            {"feature_query_hresult", std::to_string(uint32_t(support))},
            {"supported", SUCCEEDED(support) ? nlohmann::json(bool(options.MeshShaderPipelineStatsSupported)) : nlohmann::json()},
            {"includes_culled_primitives", nullptr},
            {"scope", "Hardware pipeline statistics for the sampled commands; MSPrimitives is not a visible-primitive count. View instancing may affect counts."}};
        if (mesh) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
            const auto culling = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12));
            meshStatistics_["culling_query_hresult"] = std::to_string(uint32_t(culling));
            if (SUCCEEDED(culling) && options12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives != D3D12_TRI_STATE_UNKNOWN)
                meshStatistics_["includes_culled_primitives"] =
                    options12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives == D3D12_TRI_STATE_TRUE;
            statisticsType_ = D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
        }
        query.Type = mesh ? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 : D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        query.Count = 1;
        check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&statistics_)),
              "Create pipeline statistics query");
        readbackSize_ += mesh ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1) : sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = readbackSize_;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&readback_)),
          "Create performance readback");
}
void NativePerformance::begin(ID3D12GraphicsCommandList *list) {
    if (begun_)
        throw std::runtime_error("Performance query was already recorded");
    if (statistics_)
        list->BeginQuery(statistics_.Get(), statisticsType_, 0);
    list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    begun_ = true;
}
void NativePerformance::end(ID3D12GraphicsCommandList *list) {
    if (!begun_ || ended_)
        throw std::runtime_error("Invalid performance query recording boundary");
    list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    if (statistics_)
        list->EndQuery(statistics_.Get(), statisticsType_, 0);
    list->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback_.Get(), 0);
    if (statistics_)
        list->ResolveQueryData(statistics_.Get(), statisticsType_, 0, 1, readback_.Get(),
                               16);
    ended_ = true;
}
nlohmann::json NativePerformance::result(ID3D12CommandQueue *queue) const {
    if (!ended_ || queue->GetDesc().Type != type_)
        throw std::runtime_error("Performance query does not match completed queue");
    uint64_t frequency{};
    check(queue->GetTimestampFrequency(&frequency), "Get selected queue timestamp frequency");
    if (!frequency)
        throw std::runtime_error("Selected queue reported zero timestamp frequency");
    D3D12_RANGE range{0, SIZE_T(readbackSize_)};
    void *mapped{};
    check(readback_->Map(0, &range, &mapped), "Map performance query results");
    uint64_t ticks[2]{};
    D3D12_QUERY_DATA_PIPELINE_STATISTICS1 stats{};
    static_assert(offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1, ASInvocations) ==
                  sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    memcpy(ticks, mapped, sizeof(ticks));
    if (statistics_)
        memcpy(&stats, static_cast<const uint8_t *>(mapped) + 16, size_t(readbackSize_ - 16));
    D3D12_RANGE noWrite{};
    readback_->Unmap(0, &noWrite);
    if (ticks[1] < ticks[0])
        throw std::runtime_error("Selected GPU timestamps are not monotonic");
    nlohmann::json result{
        {"source", "native_d3d12_queries"},
        {"sample_count", 1},
        {"warmup_count", 0},
        {"stable_power_state", false},
        {"queue_type", uint32_t(type_)},
        {"timestamp_begin", std::to_string(ticks[0])},
        {"timestamp_end", std::to_string(ticks[1])},
        {"timestamp_frequency_hz", std::to_string(frequency)},
        {"duration_seconds", double(ticks[1] - ticks[0]) / double(frequency)},
        {"pipeline_statistics_status", statistics_ ? "measured" : "not_sampled_on_compute_list"},
        {"pipeline_statistics_query_type", !statistics_ ? nlohmann::json() : nlohmann::json(
            statisticsType_ == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1 ? "D3D12_QUERY_TYPE_PIPELINE_STATISTICS1" : "D3D12_QUERY_TYPE_PIPELINE_STATISTICS")},
        {"mesh_pipeline_statistics", meshStatistics_},
        {"clock_scope", "selected_queue_only"},
        {"pipeline_statistics", nullptr},
        {"limitations",
         {"Single instrumented replay sample; GPU clocks are not stabilized.",
          "Selected-event duration is queue-local; prefix timeline calibration is reported separately. No vendor hardware counters.",
          "Pipeline statistics are currently sampled only on direct command lists."}}};
    if (statistics_)
        result["pipeline_statistics"] = {{"IAVertices", std::to_string(stats.IAVertices)},
                                         {"IAPrimitives", std::to_string(stats.IAPrimitives)},
                                         {"VSInvocations", std::to_string(stats.VSInvocations)},
                                         {"GSInvocations", std::to_string(stats.GSInvocations)},
                                         {"GSPrimitives", std::to_string(stats.GSPrimitives)},
                                         {"CInvocations", std::to_string(stats.CInvocations)},
                                         {"CPrimitives", std::to_string(stats.CPrimitives)},
                                         {"PSInvocations", std::to_string(stats.PSInvocations)},
                                         {"HSInvocations", std::to_string(stats.HSInvocations)},
                                         {"DSInvocations", std::to_string(stats.DSInvocations)},
                                         {"CSInvocations", std::to_string(stats.CSInvocations)}};
    if (statistics_ && statisticsType_ == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1)
        result["pipeline_statistics"].update({{"ASInvocations", std::to_string(stats.ASInvocations)},
            {"MSInvocations", std::to_string(stats.MSInvocations)}, {"MSPrimitives", std::to_string(stats.MSPrimitives)}});
    return result;
}
} // namespace flora::dx12
