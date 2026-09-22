#pragma once
#include "replay/Replay.h"
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
unsigned metricDescriptorKind(const nlohmann::json &type, const nlohmann::json &unit,
                              const nlohmann::json &symbol);
nlohmann::json annotateMetricCatalog(nlohmann::json catalog);
nlohmann::json validateMetricResult(const nlohmann::json &metadata, nlohmann::json result, Bytes raw);
QString metricsDriverLibrary();
struct MetricResult {
    nlohmann::json values;
    std::vector<uint8_t> raw;
};
class MetricsDiscovery final {
  public:
    explicit MetricsDiscovery(ID3D11Device *device, const QString &bridge = {});
    ~MetricsDiscovery();
    MetricsDiscovery(const MetricsDiscovery &) = delete;
    MetricsDiscovery &operator=(const MetricsDiscovery &) = delete;
    const nlohmann::json &catalog() const;
    const nlohmann::json &selected() const;
    void select(const std::string &name);
    void begin();
    MetricResult end(unsigned timeoutMs = 10000);
    MetricResult result();
    MetricResult decode(Bytes raw);
    nlohmann::json clockPair();
    bool supportsDrain() const;
    void submit();
    std::optional<MetricResult> poll(bool flush = false);
    void discard();
    bool supportsSamples() const;
    uint64_t sampleBegin();
    void sampleSubmit(uint64_t token);
    std::optional<MetricResult> samplePoll(uint64_t token, bool flush = false);
    MetricResult sampleResult(uint64_t token);
    void sampleRelease(uint64_t token);
    unsigned sampleCount();
    bool supportsReuse() const;
    void sampleReserve(unsigned count);
    void sampleRecycle(uint64_t token);
    void sampleClearCache();
    uint64_t sampleInfo(uint64_t token);
    nlohmann::json sampleStats();
    bool supportsRecorded() const;
    uint64_t recordedBegin(ID3D11DeviceContext *context);
    void recordedEnd(uint64_t token);
    uint64_t recordedExecute(ID3D11CommandList *command, std::span<const uint64_t> tokens,
                             bool restore = false);
    std::optional<MetricResult> recordedPoll(uint64_t token, uint64_t execution, bool flush = false);
    MetricResult recordedResult(uint64_t token, uint64_t execution);
    void recordedRelease(uint64_t token);
    unsigned recordedCount();
    nlohmann::json provenance() const;
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace flora
