#pragma once
#include <array>
#include <nlohmann/json.hpp>
#include <span>
#include <vector>

namespace flora {
// Keep binary64 values separate from JSON publication, which uses null for NA.
struct MetricStatistics {
    double median{}, minimum{}, maximum{}, mean{}, variationPercent{};
};
struct IteratedMetric {
    std::vector<double> values;
    int kind{};
    double weight{1};
    MetricStatistics statistics;
};
double metricSequentialSum(std::span<const double> values);
MetricStatistics metricStatistics(std::span<const double> values);
IteratedMetric iteratedMetric(std::vector<double> values, int kind, double weight = 1);
IteratedMetric combineMetrics(const IteratedMetric &left, const IteratedMetric &right);
std::vector<IteratedMetric> aggregateMetricRanges(const std::vector<std::vector<double>> &values, int kind,
                                                  std::span<const double> weights,
                                                  std::span<const int64_t> groups = {});
nlohmann::json assembleMetricIterations(const nlohmann::json &iterations);
nlohmann::json metricProfileMatrix(const nlohmann::json &profile);
nlohmann::json metricSampleSummary(const nlohmann::json &values);
nlohmann::json summarizeMetricRecords(const nlohmann::json &profile);
nlohmann::json summarizeMetricMatrix(const nlohmann::json &matrix);
nlohmann::json groupMetricChoices(const nlohmann::json &choices);
nlohmann::json planMetrics(const nlohmann::json &catalog, const nlohmann::json &requested);
nlohmann::json requestedMetricResults(const nlohmann::json &profile, const nlohmann::json &plan);
nlohmann::json publisherMetricProfile(const nlohmann::json &profile, const nlohmann::json &publisher);
nlohmann::json publisherMetricAnalysis(const nlohmann::json &profile, const nlohmann::json &publisher);
} // namespace flora
