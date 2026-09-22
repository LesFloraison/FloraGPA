#pragma once
#include "MetricSampleTransport.h"
#include "replay/Replay.h"
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
unsigned metricDescriptorKind(const nlohmann::json &type, const nlohmann::json &unit,
                              const nlohmann::json &symbol);
nlohmann::json annotateMetricCatalog(nlohmann::json catalog);
nlohmann::json validateMetricResult(const nlohmann::json &metadata, nlohmann::json result, Bytes raw);
QString metricsDriverLibrary();
class MetricsDiscovery final : public MetricSampleTransport,
                               public MetricCounterTransport,
                               public MetricRecordedTransport {
  public:
    explicit MetricsDiscovery(ID3D11Device *device, const QString &bridge = {});
    ~MetricsDiscovery() override;
    MetricsDiscovery(const MetricsDiscovery &) = delete;
    MetricsDiscovery &operator=(const MetricsDiscovery &) = delete;
    const nlohmann::json &catalog() const;
    const nlohmann::json &selected() const override;
    void select(const std::string &name);
    void begin() override;
    MetricResult end(unsigned timeoutMs = 10000);
    MetricResult result();
    MetricResult decode(Bytes raw);
    nlohmann::json clockPair() override;
    bool supportsDrain() const override;
    void submit() override;
    std::optional<MetricResult> poll(bool flush = false) override;
    void discard() override;
    bool supportsSamples() const override;
    uint64_t sampleBegin() override;
    void sampleSubmit(uint64_t token) override;
    std::optional<MetricResult> samplePoll(uint64_t token, bool flush = false) override;
    MetricResult sampleResult(uint64_t token);
    void sampleRelease(uint64_t token) override;
    unsigned sampleCount();
    bool supportsReuse() const override;
    void sampleReserve(unsigned count) override;
    void sampleRecycle(uint64_t token) override;
    void sampleClearCache() override;
    uint64_t sampleInfo(uint64_t token) override;
    nlohmann::json sampleStats() override;
    bool supportsRecorded() const override;
    uint64_t recordedBegin(ID3D11DeviceContext *context) override;
    void recordedEnd(uint64_t token) override;
    uint64_t recordedExecute(ID3D11CommandList *command, std::span<const uint64_t> tokens,
                             bool restore = false) override;
    std::optional<MetricResult> recordedPoll(uint64_t token, uint64_t execution, bool flush = false) override;
    MetricResult recordedResult(uint64_t token, uint64_t execution);
    void recordedRelease(uint64_t token) override;
    unsigned recordedCount();
    nlohmann::json provenance() const override;
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace flora
