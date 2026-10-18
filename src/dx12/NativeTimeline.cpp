#include "NativeTimeline.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
namespace flora::dx12 {
namespace {
using Json = nlohmann::json;
void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) { std::ostringstream s; s << what << ": 0x" << std::hex << uint32_t(hr); throw std::runtime_error(s.str()); }
}
uint64_t qpc() { LARGE_INTEGER v{}; if (!QueryPerformanceCounter(&v)) throw std::runtime_error("Read replay QPC"); return uint64_t(v.QuadPart); }
uint64_t integer(const Json &j, const char *key) { return std::stoull(j.at(key).get<std::string>()); }
// Subtract integer clocks before conversion: absolute GPU ticks can exceed 2^53.
double delta(uint64_t a, uint64_t b, uint64_t frequency) {
    return a >= b ? double(a - b) / double(frequency) : -double(b - a) / double(frequency);
}
}
NativeTimeline::NativeTimeline(ID3D12Device *device, size_t capacity) : device_(device) {
    if (!capacity || capacity > 65536) throw std::runtime_error("Replay timeline requires 1 to 65536 submitted lists");
    capacity_ = uint32_t(capacity);
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) throw std::runtime_error("Read QPC frequency");
    cpuFrequency_ = uint64_t(frequency.QuadPart); origin_ = qpc();
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    copySupported_ = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) && options.CopyQueueTimestampQueriesSupported;
}
size_t NativeTimeline::begin(ID3D12GraphicsCommandList *list, Json identity) {
    if (entries_.size() >= capacity_) throw std::runtime_error("Replay timeline capacity exceeded");
    const auto type = list->GetType();
    const bool supported = type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
        (type == D3D12_COMMAND_LIST_TYPE_COPY && copySupported_);
    uint32_t slot{};
    if (supported) {
        auto &s = storage_[type];
        if (!s.heap) {
            D3D12_QUERY_HEAP_DESC query{}; query.Count = capacity_ * 2;
            query.Type = type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            check(device_->CreateQueryHeap(&query, IID_PPV_ARGS(&s.heap)), "Create timeline timestamps");
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = uint64_t(capacity_) * 16;
            desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&s.readback)), "Create timeline readback");
        }
        slot = s.used++;
        list->EndQuery(s.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    }
    entries_.push_back({std::move(identity), type, slot, supported, false}); return entries_.size() - 1;
}
void NativeTimeline::end(ID3D12GraphicsCommandList *list, size_t entry) {
    auto &e = entries_.at(entry);
    if (e.ended || e.type != list->GetType()) throw std::runtime_error("Invalid timeline recording boundary");
    if (e.supported) {
        auto &s = storage_.at(e.type);
        list->EndQuery(s.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, e.slot * 2 + 1);
        list->ResolveQueryData(s.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, e.slot * 2, 2, s.readback.Get(), uint64_t(e.slot) * 16);
    }
    e.ended = true;
}
Json NativeTimeline::calibrate(ID3D12CommandQueue *queue) const {
    uint64_t gpu{}, cpu{}, frequency{}; const auto before = qpc();
    const auto hr = queue->GetClockCalibration(&gpu, &cpu); const auto after = qpc();
    const auto frequencyHr = queue->GetTimestampFrequency(&frequency);
    return {{"status", SUCCEEDED(hr) && SUCCEEDED(frequencyHr) && frequency ? "calibrated" : "unavailable"},
        {"hresult", std::to_string(uint32_t(hr))}, {"frequency_hresult", std::to_string(uint32_t(frequencyHr))},
        {"gpu", std::to_string(gpu)}, {"cpu", std::to_string(cpu)}, {"frequency", std::to_string(frequency)},
        {"call_begin_qpc", std::to_string(before)}, {"call_end_qpc", std::to_string(after)}};
}
void NativeTimeline::beforeSubmit(uint64_t submission, ID3D12CommandQueue *queue) {
    if (submissions_.contains(submission)) throw std::runtime_error("Duplicate timeline submission");
    if (submissions_.empty()) origin_ = qpc();
    submissions_[submission] = {{"before", calibrate(queue)}};
}
void NativeTimeline::afterComplete(uint64_t submission, ID3D12CommandQueue *queue) {
    submissions_.at(submission)["after"] = calibrate(queue);
}
void NativeTimeline::synchronization(uint64_t record, uint64_t queue, uint64_t fence, uint64_t value, bool wait) {
    synchronization_.push_back({{"record", std::to_string(record)}, {"queue", std::to_string(queue)},
        {"fence", std::to_string(fence)}, {"value", std::to_string(value)}, {"operation", wait ? "queue_wait" : "queue_signal"},
        {"cpu_observed_qpc", std::to_string(qpc())}, {"gpu_wait_duration", nullptr}});
}
Json NativeTimeline::result() const {
    std::map<D3D12_COMMAND_LIST_TYPE, std::vector<uint64_t>> ticks;
    for (const auto &[type, s] : storage_) {
        auto &values = ticks[type]; values.resize(size_t(s.used) * 2);
        D3D12_RANGE range{0, SIZE_T(s.used) * 16}; void *mapped{};
        check(s.readback->Map(0, &range, &mapped), "Read timeline timestamps");
        memcpy(values.data(), mapped, range.End); D3D12_RANGE noWrite{}; s.readback->Unmap(0, &noWrite);
    }
    Json rows = Json::array(), calibrations = Json::array();
    for (const auto &[id, value] : submissions_) { auto row = value; row["submission"] = std::to_string(id); calibrations.push_back(std::move(row)); }
    for (const auto &e : entries_) {
        if (!e.ended) throw std::runtime_error("Incomplete timeline recording");
        auto row = e.identity; row["queue_type"] = uint32_t(e.type);
        row["status"] = e.supported ? "measured" : "unsupported";
        row["clock_scope"] = "queue_only"; row["begin_seconds"] = nullptr; row["end_seconds"] = nullptr;
        if (!e.supported) { row["reason"] = "Copy queue timestamps are unsupported by the replay device"; rows.push_back(std::move(row)); continue; }
        const auto &values = ticks.at(e.type); const auto begin = values[e.slot * 2], end = values[e.slot * 2 + 1];
        if (end < begin) throw std::runtime_error("Timeline timestamps are not monotonic");
        row["timestamp_begin"] = std::to_string(begin); row["timestamp_end"] = std::to_string(end);
        const auto &calibration = submissions_.at(integer(row, "submission"));
        const auto &a = calibration.at("before"), &b = calibration.at("after");
        const auto frequency = integer(a, "frequency");
        row["timestamp_frequency_hz"] = std::to_string(frequency);
        row["duration_seconds"] = frequency ? Json(delta(end, begin, frequency)) : Json(nullptr);
        row["reason"] = "Clock calibration unavailable; compare only within this queue";
        if (a.at("status") == "calibrated" && b.at("status") == "calibrated" && frequency == integer(b, "frequency")) {
            const auto ag = integer(a, "gpu"), bg = integer(b, "gpu"), ac = integer(a, "cpu"), bc = integer(b, "cpu");
            const auto drift = std::abs(delta(bg, ag, frequency) - delta(bc, ac, cpuFrequency_));
            const auto callWindow = delta(integer(a, "call_end_qpc"), integer(a, "call_begin_qpc"), cpuFrequency_) +
                delta(integer(b, "call_end_qpc"), integer(b, "call_begin_qpc"), cpuFrequency_);
            row["calibration_drift_seconds"] = drift; row["calibration_call_window_seconds"] = callWindow;
            if (ac <= bc && ag <= begin && end <= bg && drift <= (std::max)(0.0001, std::abs(delta(bc, ac, cpuFrequency_)) * 0.001) + callWindow) {
                row["clock_scope"] = "calibrated_cpu_qpc";
                row["begin_seconds"] = delta(ac, origin_, cpuFrequency_) + delta(begin, ag, frequency);
                row["end_seconds"] = delta(ac, origin_, cpuFrequency_) + delta(end, ag, frequency);
                row["reason"] = "CPU/GPU calibration brackets this submission; drift is reported, clocks are not stabilized";
            } else row["reason"] = "Calibration drift or range check failed; compare only within this queue";
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const Json &a, const Json &b) {
        return std::pair{integer(a, "submission"), integer(a, "list_ordinal")} <
               std::pair{integer(b, "submission"), integer(b, "list_ordinal")};
    });
    return {{"source", "native_d3d12_queries"}, {"scope", "frame_prefix_submitted_lists"},
        {"qpc_origin", std::to_string(origin_)}, {"qpc_frequency_hz", std::to_string(cpuFrequency_)},
        {"entries", rows}, {"calibrations", calibrations}, {"synchronization", synchronization_},
        {"limitations", {"Replay follows captured GPU Fence dependencies with conservative host mutation/CPU wait drains; not original-application concurrency.",
            "Timestamp intervals are bottom-of-pipe completion brackets and include replay instrumentation.",
            "Initialization and resource experiment helper submissions are excluded. Fence rows are CPU observations, not GPU stall durations.",
            "Calibrated alignment can drift without stable power state; unavailable or inconsistent calibration remains queue-local."}}};
}
}
