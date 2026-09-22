#pragma once
#include <cstdint>
#include <functional>
#include <utility>
namespace flora {
uint64_t metricTicksToNanoseconds(uint64_t ticks, uint64_t frequency);
class MetricClockProvider {
  public:
    virtual ~MetricClockProvider() = default;
    virtual int64_t maximum() = 0;
    virtual std::pair<int64_t, int64_t> sample(bool force) = 0;
    virtual bool replaceOffset() const = 0;
};
struct MetricClockPair {
    uint64_t status{}, gpu{}, cpu{};
};
class MetricsDiscoveryClockSource final : public MetricClockProvider {
  public:
    MetricsDiscoveryClockSource(int64_t maximum, std::function<MetricClockPair()> readPair);
    int64_t maximum() override { return period_; }
    std::pair<int64_t, int64_t> sample(bool force) override;
    bool replaceOffset() const override { return false; }
    bool lastSuccess() const { return lastSuccess_; }

  private:
    int64_t period_;
    std::function<MetricClockPair()> readPair_;
    bool lastSuccess_{};
};
class MetricClock {
  public:
    explicit MetricClock(MetricClockProvider &provider);
    bool crossed(int64_t earlier, int64_t later);
    void update(bool force = false);
    int64_t convert(int64_t timestamp);
    int64_t reference() const { return reference_; }
    int64_t offset() const { return offset_; }
    int64_t previousOffset() const { return previousOffset_; }

  private:
    MetricClockProvider &provider_;
    int64_t reference_{}, offset_{}, previousOffset_{};
};
} // namespace flora
