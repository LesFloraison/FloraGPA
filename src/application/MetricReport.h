#pragma once
#include "MetricClock.h"
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
using MetricTypedValue = std::array<uint8_t, 16>;
using MetricTypedReport = std::vector<MetricTypedValue>;
double metricTypedDouble(Bytes record);
uint64_t metricTimestampInteger(Bytes record);
uint64_t metricGpuTimeMicroseconds(uint64_t nanoseconds);
// Exact direct uint64 -> binary32 rounding, returned as binary64.
double metricUintFloat32(uint64_t value);
class MetricValueSink {
  public:
    virtual ~MetricValueSink() = default;
    virtual std::vector<uint32_t> requestedIds() = 0;
    virtual void writeValue(uint32_t index, double value) = 0;
    virtual void setKey(uint64_t key) = 0;
};
class MetricReportValues {
  public:
    explicit MetricReportValues(std::optional<MetricTypedReport> records, uint64_t key = 0)
        : records_(std::move(records)), key_(key) {}
    std::optional<double> read(uint32_t index) const;
    bool writeMetric(MetricValueSink &metric) const;

  private:
    std::optional<MetricTypedReport> records_;
    uint64_t key_;
};
struct MetricBusyState {
    uint64_t previousKey{};
};
struct MetricPostprocessed {
    std::vector<MetricTypedReport> reports;
    std::vector<uint64_t> keys;
};
MetricPostprocessed postprocessMetricReports(std::vector<MetricTypedReport> reports,
                                             const nlohmann::json &metrics, const nlohmann::json &information,
                                             MetricClock &clock, MetricBusyState &busy,
                                             bool normalize = true);
} // namespace flora
