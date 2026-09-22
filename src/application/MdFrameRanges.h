#pragma once
#include "MetricSampleTransport.h"
#include "replay/Replay.h"
namespace flora {
class Experiment;
nlohmann::json selectMetricInterval(const Frame &frame, const Experiment *experiment = nullptr,
                                    const nlohmann::json &start = nullptr,
                                    const nlohmann::json &end = nullptr);
nlohmann::json selectFrameMetricRanges(const Frame &frame, const nlohmann::json &indices = nullptr,
                                       const Experiment *experiment = nullptr);
// Borrowed counter operations. The acquisition owner closes counters on failure.
struct MetricCommandCounterClient {
    using Consumer = std::function<void(MetricResult &)>;
    std::function<void(Consumer)> begin;
    std::function<void()> submit;
    std::function<MetricResult()> end;
};
class MetricIntervalCounter {
  public:
    MetricIntervalCounter(MetricCommandCounterClient metrics, nlohmann::json interval,
                          MetricCommandCounterClient::Consumer consume, bool callbackReports = false);
    void scope(Id event, const std::function<void()> &command);
    nlohmann::json verify() const;
    nlohmann::json snapshot() const;

  private:
    MetricCommandCounterClient metrics_;
    nlohmann::json interval_;
    MetricCommandCounterClient::Consumer consume_;
    bool callbackReports_{}, completed_{}, active_{}, failed_{};
    std::vector<Id> expected_, seen_;
};
class FrameRangeCounter {
  public:
    using Consumer = std::function<void(const nlohmann::json &, MetricResult &)>;
    FrameRangeCounter(MetricCommandCounterClient metrics, nlohmann::json ranges, Consumer consume,
                      bool callbackReports = false);
    void scope(Id event, const std::function<void()> &command);
    nlohmann::json verify() const;
    nlohmann::json snapshot() const;

  private:
    MetricCommandCounterClient metrics_;
    nlohmann::json ranges_;
    Consumer consume_;
    bool callbackReports_{}, active_{}, failed_{};
    size_t position_{};
    std::vector<Id> seen_;
    std::optional<Id> lastEvent_;
    nlohmann::json audit_ = nlohmann::json::array();
};
} // namespace flora
