#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <vector>
struct ID3D11DeviceContext;
struct ID3D11CommandList;
namespace flora {
struct MetricResult {
    nlohmann::json values;
    std::vector<uint8_t> raw;
};
class MetricClockTransport {
  public:
    virtual ~MetricClockTransport() = default;
    virtual nlohmann::json clockPair() = 0;
};
class MetricRecordedTransport : public MetricClockTransport {
  public:
    virtual bool supportsRecorded() const = 0;
    virtual const nlohmann::json &selected() const = 0;
    virtual uint64_t recordedBegin(ID3D11DeviceContext *context) = 0;
    virtual void recordedEnd(uint64_t token) = 0;
    virtual uint64_t recordedExecute(ID3D11CommandList *command, std::span<const uint64_t> tokens,
                                     bool restore = false) = 0;
    virtual std::optional<MetricResult> recordedPoll(uint64_t token, uint64_t execution,
                                                     bool flush = false) = 0;
    virtual void recordedRelease(uint64_t token) = 0;
    virtual nlohmann::json provenance() const = 0;
};
class MetricCounterTransport {
  public:
    virtual ~MetricCounterTransport() = default;
    virtual bool supportsDrain() const = 0;
    virtual void begin() = 0;
    virtual void submit() = 0;
    virtual std::optional<MetricResult> poll(bool flush = false) = 0;
    virtual void discard() = 0;
};
class MetricSampleTransport {
  public:
    virtual ~MetricSampleTransport() = default;
    virtual bool supportsSamples() const = 0;
    virtual bool supportsReuse() const = 0;
    virtual uint64_t sampleBegin() = 0;
    virtual void sampleSubmit(uint64_t token) = 0;
    virtual std::optional<MetricResult> samplePoll(uint64_t token, bool flush = false) = 0;
    virtual void sampleRelease(uint64_t token) = 0;
    virtual void sampleReserve(unsigned count) = 0;
    virtual void sampleRecycle(uint64_t token) = 0;
    virtual void sampleClearCache() = 0;
    virtual uint64_t sampleInfo(uint64_t token) = 0;
    virtual nlohmann::json sampleStats() = 0;
};
class MetricPublisherObserver {
  public:
    virtual ~MetricPublisherObserver() = default;
    virtual void update(bool force) = 0;
    virtual size_t recordCount() const = 0;
    virtual size_t refreshCount() const = 0;
};
} // namespace flora
