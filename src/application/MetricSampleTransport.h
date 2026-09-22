#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>
namespace flora {
struct MetricResult {
    nlohmann::json values;
    std::vector<uint8_t> raw;
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
