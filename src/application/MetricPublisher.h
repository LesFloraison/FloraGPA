#pragma once
#include "MetricReport.h"
#include "MetricSampleTransport.h"
#include <QString>
namespace flora {
MetricTypedValue encodePublisherValue(const nlohmann::json &value);
std::string metricCsv(const std::vector<std::string> &fields, const nlohmann::json &rows);
nlohmann::json loadMetricPublisherResult(const QString &folder, const nlohmann::json &profile);
class MetricPublisherValues final : public MetricPublisherObserver {
  public:
    explicit MetricPublisherValues(MetricClockTransport &metrics, bool recorded = false);
    ~MetricPublisherValues();
    MetricPublisherValues(const MetricPublisherValues &) = delete;
    MetricPublisherValues &operator=(const MetricPublisherValues &) = delete;
    void update(bool force = false) override;
    void append(const nlohmann::json &metadata, const nlohmann::json &row);
    nlohmann::json report() const;
    std::string csv() const;
    void writeCsv(const QString &path) const;
    size_t recordCount() const override;
    const nlohmann::json &lastRecord() const;
    size_t refreshCount() const override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace flora
