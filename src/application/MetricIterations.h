#pragma once
#include "MetricAnalysis.h"
#include "core/Frame.h"
#include <functional>
#include <stdexcept>
namespace flora {
inline constexpr uint32_t metricAllPasses = UINT32_MAX;
struct MetricPassSelection {
    uint32_t first{};
    uint64_t count{};
};
MetricPassSelection selectMetricPasses(const nlohmann::json &groupCount,
                                       const nlohmann::json &requestedPass = metricAllPasses,
                                       const nlohmann::json &prepareFlag = false);
nlohmann::json selectMetricRangeTable(const nlohmann::json &categorized,
                                      const nlohmann::json &fallback = nlohmann::json::array());
nlohmann::json mapMetricRanges(const nlohmann::json &requested, const nlohmann::json &captured,
                               const nlohmann::json &internalIds);
uint32_t metricApiKind(uint16_t type);
nlohmann::json buildFrameMetricIndex(const Frame &frame);
nlohmann::json parseMetricQueryFlags(const nlohmann::json &iterations, const nlohmann::json &descriptions,
                                     const nlohmann::json &initial = nlohmann::json::array());
nlohmann::json updateMetricSampleTimes(const nlohmann::json &previous, const nlohmann::json &parallelRanges);
nlohmann::json chooseWeightMetric(const nlohmann::json &descriptions);
uint32_t metricIterationPass(const nlohmann::json &requested,
                             const nlohmann::json &mapping = nlohmann::json::array());
nlohmann::json prepareMetricIterationValues(const nlohmann::json &iterations,
                                            const nlohmann::json &descriptions,
                                            const nlohmann::json &requested, const nlohmann::json &weights,
                                            const nlohmann::json &groups = nlohmann::json::array(),
                                            const nlohmann::json &initial = nullptr);
class MetricIterationTransport {
  public:
    virtual ~MetricIterationTransport() = default;
    virtual nlohmann::json descriptions() = 0;
    virtual nlohmann::json queryFlagDescriptions() { return nlohmann::json::array(); }
    virtual nlohmann::json prepare(const nlohmann::json &metricIds) = 0; // {groups, flag}
    virtual nlohmann::json replay(uint32_t pass, const nlohmann::json &ranges, bool requestFlag) = 0;
};
class MetricOuterPassRunner {
  public:
    explicit MetricOuterPassRunner(MetricIterationTransport &transport) : transport_(transport) {}
    nlohmann::json run(const nlohmann::json &metricIds, const nlohmann::json &ranges,
                       const nlohmann::json &requestedPass = metricAllPasses,
                       const nlohmann::json &requestFlag = false, const std::function<bool()> &cancel = {},
                       nlohmann::json result = nullptr);

  private:
    MetricIterationTransport &transport_;
};
class MetricIterationError : public std::runtime_error {
  public:
    explicit MetricIterationError(nlohmann::json result);
    const nlohmann::json &result() const { return result_; }

  private:
    nlohmann::json result_;
};
class MetricIterationRunner {
  public:
    explicit MetricIterationRunner(MetricIterationTransport &transport) : transport_(transport) {}
    nlohmann::json execute(const nlohmann::json &metricIds, const nlohmann::json &ranges,
                           const nlohmann::json &options = nlohmann::json::object(),
                           const std::function<bool()> &cancel = {});
    nlohmann::json collect(const nlohmann::json &metricIds, const nlohmann::json &ranges,
                           const nlohmann::json &options = nlohmann::json::object(),
                           const std::function<bool()> &cancel = {});
    nlohmann::json executeRanges(const nlohmann::json &metricIds, const nlohmann::json &requested,
                                 const nlohmann::json &captured, const nlohmann::json &internalIds,
                                 const nlohmann::json &options = nlohmann::json::object(),
                                 const std::function<bool()> &cancel = {});
    nlohmann::json executeFrame(const Frame &frame, const nlohmann::json &metricIds,
                                const nlohmann::json &requested = nullptr,
                                const nlohmann::json &options = nlohmann::json::object(),
                                const std::function<bool()> &cancel = {});

  private:
    MetricIterationTransport &transport_;
};
} // namespace flora
