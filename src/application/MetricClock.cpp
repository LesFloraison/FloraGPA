#include "MetricClock.h"
#include <bit>
#include <limits>
#include <stdexcept>
namespace flora {
namespace {
int64_t checkedPeriod(int64_t period) {
    if (period <= 0 || period > std::numeric_limits<int64_t>::max() / 95)
        throw std::runtime_error("Clock period exceeds the verified positive range");
    return period;
}
int64_t wrappedAbsolute(int64_t value) {
    return value < 0 ? std::bit_cast<int64_t>(uint64_t(0) - uint64_t(value)) : value;
}
int64_t wrappedAdd(int64_t a, int64_t b) { return std::bit_cast<int64_t>(uint64_t(a) + uint64_t(b)); }
} // namespace
uint64_t metricTicksToNanoseconds(uint64_t ticks, uint64_t frequency) {
    if (!frequency)
        throw std::runtime_error("Frequency must be a positive unsigned 64-bit integer");
    // Preserve the publisher's uint64 intermediate overflow before division.
    return (ticks % frequency * uint64_t(1000000000)) / frequency + ticks / frequency * uint64_t(1000000000);
}
MetricsDiscoveryClockSource::MetricsDiscoveryClockSource(int64_t maximum,
                                                         std::function<MetricClockPair()> readPair)
    : period_(checkedPeriod(maximum)), readPair_(std::move(readPair)) {}
std::pair<int64_t, int64_t> MetricsDiscoveryClockSource::sample(bool) {
    lastSuccess_ = false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto pair = readPair_();
        if (!pair.status) {
            lastSuccess_ = true;
            return {std::bit_cast<int64_t>(pair.cpu - pair.gpu), std::bit_cast<int64_t>(pair.gpu)};
        }
    }
    return {0, 0};
}
MetricClock::MetricClock(MetricClockProvider &provider) : provider_(provider) { update(); }
bool MetricClock::crossed(int64_t earlier, int64_t later) {
    if (later >= earlier)
        return false;
    auto period = checkedPeriod(provider_.maximum());
    if (earlier <= period * 95 / 100)
        return false;
    period = checkedPeriod(provider_.maximum());
    return later < period * 5 / 100;
}
void MetricClock::update(bool force) {
    const auto [offset, reference] = provider_.sample(force);
    if (crossed(reference_, reference))
        previousOffset_ = offset_;
    if (provider_.replaceOffset() || wrappedAbsolute(offset) > wrappedAbsolute(offset_))
        offset_ = offset;
    reference_ = reference;
}
int64_t MetricClock::convert(int64_t timestamp) {
    if (timestamp < reference_) {
        if (crossed(reference_, timestamp))
            update(true);
        return wrappedAdd(offset_, timestamp);
    }
    if (crossed(timestamp, reference_))
        return previousOffset_ ? wrappedAdd(previousOffset_, timestamp) : 0;
    return wrappedAdd(offset_, timestamp);
}
} // namespace flora
