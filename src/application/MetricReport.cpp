#include "MetricReport.h"
#include <bit>
#include <cmath>
#include <limits>
#include <map>
namespace flora {
namespace {
template <class T> T read(Bytes record, size_t offset) {
    if (record.size() != 16)
        throw std::runtime_error("An MD typed value must contain exactly 16 bytes");
    T value;
    std::memcpy(&value, record.data() + offset, sizeof value);
    return value;
}
template <class T> void write(MetricTypedValue &record, T value) {
    std::memcpy(record.data() + 8, &value, sizeof value);
}
} // namespace
double metricTypedDouble(Bytes record) {
    switch (read<uint32_t>(record, 0)) {
    case 0:
        return double(read<uint32_t>(record, 8));
    case 1:
        return double(read<uint64_t>(record, 8));
    case 2:
        return double(read<float>(record, 8));
    case 3:
        return record[8] != 0 ? 1.0 : 0.0;
    default:
        return 0.;
    }
}
uint64_t metricGpuTimeMicroseconds(uint64_t nanoseconds) { return nanoseconds / 1000; }
double metricUintFloat32(uint64_t value) {
    const auto width = std::bit_width(value);
    if (width <= 24)
        return double(value);
    const auto shift = unsigned(width - 24);
    auto high = value >> shift;
    const auto low = value & ((uint64_t(1) << shift) - 1), half = uint64_t(1) << (shift - 1);
    if (low > half || (low == half && (high & 1)))
        ++high;
    return std::ldexp(double(high), int(shift));
}
uint64_t metricTimestampInteger(Bytes record) {
    switch (read<uint32_t>(record, 0)) {
    case 0:
        return read<uint32_t>(record, 8);
    case 1:
        return read<uint64_t>(record, 8);
    case 3:
        return record[8] != 0;
    case 2: {
        const auto value = float(double(read<float>(record, 8)) + 0.5);
        if (!std::isfinite(value) || value < 0 || double(value) >= 18446744073709551616.0)
            throw std::runtime_error("Timestamp float is outside the verified finite unsigned range");
        return uint64_t(value);
    }
    default:
        return 0;
    }
}
std::optional<double> MetricReportValues::read(uint32_t index) const {
    if (!records_ || index >= records_->size())
        return {};
    return metricTypedDouble((*records_)[index]);
}
bool MetricReportValues::writeMetric(MetricValueSink &metric) const {
    if (!records_)
        return false;
    for (auto index : metric.requestedIds()) {
        const auto value = read(index);
        if (!value)
            return false;
        metric.writeValue(index, *value);
    }
    if (key_)
        metric.setKey(key_);
    return true;
}
MetricPostprocessed postprocessMetricReports(std::vector<MetricTypedReport> reports,
                                             const nlohmann::json &metrics, const nlohmann::json &information,
                                             MetricClock &clock, MetricBusyState &busy, bool normalize) {
    std::map<std::string, size_t> indices;
    for (const auto *definitions : {&metrics, &information})
        for (const auto &definition : *definitions) {
            const auto index = indices.size();
            if (!indices.emplace(definition.at("name").get<std::string>(), index).second)
                throw std::runtime_error("Ambiguous duplicate report field names are not supported");
        }
    for (const auto &row : reports)
        if (row.size() != indices.size())
            throw std::runtime_error("Report fields disagree with definitions");
    std::vector<uint64_t> keys;
    std::optional<uint64_t> previousRaw;
    const auto begin = indices.find("QueryBeginTime");
    for (const auto &row : reports) {
        if (begin == indices.end()) {
            keys.push_back(0);
            continue;
        }
        const auto raw = metricTimestampInteger(row[begin->second]);
        keys.push_back(uint64_t(clock.convert(std::bit_cast<int64_t>(raw))));
        if (previousRaw)
            clock.crossed(std::bit_cast<int64_t>(*previousRaw), std::bit_cast<int64_t>(raw));
        previousRaw = raw;
    }
    if (!normalize)
        return {std::move(reports), std::move(keys)};
    std::vector<size_t> timestamps;
    for (size_t i = 0; i < metrics.size(); ++i)
        if (metrics[i].at("metric_type") == 4)
            timestamps.push_back(i);
    for (size_t i = 0; i < information.size(); ++i)
        if (information[i].at("information_type") == 3)
            timestamps.push_back(metrics.size() + i);
    for (auto &row : reports)
        for (auto index : timestamps)
            write(row[index],
                  uint64_t(clock.convert(std::bit_cast<int64_t>(metricTimestampInteger(row[index])))));
    const auto duration = indices.find("GpuTime"), busyIndex = indices.find("GpuBusy");
    if (duration != indices.end())
        for (size_t i = 0; i < reports.size(); ++i) {
            auto &row = reports[i];
            auto &record = row[duration->second];
            if (read<uint32_t>(record, 0) != 1)
                throw std::runtime_error("GpuTime must use the verified uint64 layout");
            const auto nanoseconds = read<uint64_t>(record, 8);
            write(record, metricGpuTimeMicroseconds(nanoseconds));
            if (busyIndex != indices.end()) {
                auto &value = row[busyIndex->second];
                if (read<uint32_t>(value, 0) != 2)
                    throw std::runtime_error("GpuBusy must use the verified float32 layout");
                if (keys[i] && busy.previousKey) {
                    const auto numerator = float(double(nanoseconds));
                    const auto denominator = metricUintFloat32(keys[i] - busy.previousKey);
                    const auto quotient = denominator == 0
                                              ? (numerator != 0 ? std::numeric_limits<float>::infinity()
                                                                : std::bit_cast<float>(uint32_t(0xffc00000)))
                                              : float(double(numerator) / denominator);
                    auto percent = float(double(quotient) * 100.0);
                    // MINSS selects its second operand for unordered input.
                    if (!(percent < 100.f))
                        percent = 100.f;
                    write(value, percent);
                }
                if (keys[i])
                    busy.previousKey = keys[i];
            }
        }
    return {std::move(reports), std::move(keys)};
}
} // namespace flora
