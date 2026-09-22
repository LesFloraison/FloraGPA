#include "MetricPublisher.h"
#include <QByteArray>
#include <QFile>
#include <algorithm>
#include <charconv>
#include <cmath>
namespace flora {
using Json = nlohmann::json;
namespace {
std::string floatText(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    char buffer[64];
    const auto converted =
        std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    if (converted.ec != std::errc{})
        throw std::runtime_error("Cannot format metric value");
    std::string repr(buffer, converted.ptr);
    const auto e = repr.find('e');
    const int exponent = std::stoi(repr.substr(e + 1));
    if (exponent < -4 || exponent >= 16)
        return repr;
    const bool negative = repr[0] == '-';
    auto digits = repr.substr(negative ? 1 : 0, e - (negative ? 1 : 0));
    std::erase(digits, '.');
    const int point = exponent + 1;
    std::string result = negative ? "-" : "";
    if (point <= 0)
        return result + "0." + std::string(size_t(-point), '0') + digits;
    if (size_t(point) >= digits.size())
        return result + digits + std::string(size_t(point) - digits.size(), '0') + ".0";
    return result + digits.substr(0, point) + "." + digits.substr(point);
}
std::string cell(const Json &value) {
    auto text = value.is_null()           ? std::string{}
                : value.is_string()       ? value.get<std::string>()
                : value.is_boolean()      ? std::string(value.get<bool>() ? "True" : "False")
                : value.is_number_float() ? floatText(value.get<double>())
                                          : value.dump();
    if (text.find_first_of(",\"\r\n") == std::string::npos)
        return text;
    std::string out = "\"";
    for (char c : text) {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + '"';
}
std::string hex(Bytes bytes) {
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
        .toHex()
        .toStdString();
}
std::vector<uint8_t> rawHex(const std::string &text) {
    std::vector<uint8_t> bytes;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    const auto invalid = [](size_t position) {
        throw std::runtime_error("non-hexadecimal number found in fromhex() arg at position " +
                                 std::to_string(position));
    };
    for (size_t i = 0; i < text.size();) {
        if (std::string_view(" \t\n\r\v\f").find(text[i]) != std::string_view::npos) {
            ++i;
            continue;
        }
        const auto high = digit(text[i]);
        if (high < 0)
            invalid(i);
        ++i;
        if (i == text.size())
            invalid(i);
        const auto low = digit(text[i]);
        if (low < 0)
            invalid(i);
        ++i;
        bytes.push_back(uint8_t(high * 16 + low));
    }
    return bytes;
}
} // namespace
std::string metricCsv(const std::vector<std::string> &fields, const Json &rows) {
    std::string output = "\xef\xbb\xbf";
    const auto line = [&](const auto &values) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i)
                output += ',';
            output += cell(values[i]);
        }
        output += "\r\n";
    };
    line(fields);
    for (const auto &row : rows) {
        Json values = Json::array();
        for (const auto &name : fields)
            values.push_back(row.at(name));
        line(values);
    }
    return output;
}
MetricTypedValue encodePublisherValue(const Json &value) {
    const auto kind = value.at("type").get<uint32_t>();
    const auto &number = value.at("value");
    if (number.is_null())
        throw std::runtime_error("Publisher conversion needs a finite calculated value");
    MetricTypedValue record{};
    std::memcpy(record.data(), &kind, 4);
    if (kind == 2) {
        const auto input = number.get<double>();
        const auto scalar = float(input);
        if (std::isfinite(input) && !std::isfinite(scalar))
            throw std::runtime_error("float too large to pack with f format");
        std::memcpy(record.data() + 8, &scalar, 4);
    } else {
        if (!number.is_number_integer() && !number.is_number_unsigned() && !number.is_boolean() &&
            !(kind == 3 && number.is_number_float()))
            throw std::runtime_error("required argument is not an integer");
        if ((number.is_number_integer() && !number.is_number_unsigned() && number.get<int64_t>() < 0) ||
            (number.is_number_float() &&
             (!std::isfinite(number.get<double>()) || std::trunc(number.get<double>()) < 0 ||
              number.get<double>() >= 18446744073709551616.0)))
            throw std::runtime_error("'Q' format requires 0 <= number <= 18446744073709551615");
        const auto scalar = number.is_boolean() ? uint64_t(number.get<bool>()) : number.get<uint64_t>();
        std::memcpy(record.data() + 8, &scalar, 8);
    }
    return record;
}
struct MetricPublisherValues::State {
    MetricClockTransport &metrics;
    bool recorded;
    Json calibrations = Json::array(), records = Json::array(), refreshes = Json::array();
    Json maximum, frequency;
    MetricBusyState busy;
    std::optional<Json> first;
    std::unique_ptr<MetricsDiscoveryClockSource> source;
    std::unique_ptr<MetricClock> clock;
    Json read() {
        auto result = metrics.clockPair();
        calibrations.push_back(result);
        return result;
    }
    Json clockState() const {
        return Json::array({clock->reference(), clock->offset(), clock->previousOffset()});
    }
    State(MetricClockTransport &transport, bool mode) : metrics(transport), recorded(mode) {
        first = read();
        maximum = first->at("maximum_ns");
        frequency = first->at("frequency_hz");
        source = std::make_unique<MetricsDiscoveryClockSource>(maximum.get<int64_t>(), [this] {
            auto value = first ? *first : read();
            first.reset();
            if (value.at("maximum_ns") != maximum || value.at("frequency_hz") != frequency)
                throw std::runtime_error("Clock metadata changed during collection");
            return MetricClockPair{value.at("status").get<uint64_t>(), value.at("gpu_ns").get<uint64_t>(),
                                   value.at("cpu_ns").get<uint64_t>()};
        });
        clock = std::make_unique<MetricClock>(*source);
        if (!source->lastSuccess())
            throw std::runtime_error("Driver clock calibration failed three times");
    }
    Json convert(const Json &metadata, const Json &values) {
        MetricTypedReport typed;
        for (const auto &value : values)
            typed.push_back(encodePublisherValue(value));
        const auto before = clockState();
        const auto previous = busy.previousKey, calibrationStart = uint64_t(calibrations.size());
        auto information = metadata.at("information");
        for (auto &definition : information)
            definition["information_type"] = definition.at("info_type");
        const auto processed =
            postprocessMetricReports({std::move(typed)}, metadata.at("metrics"), information, *clock, busy);
        if (!source->lastSuccess())
            throw std::runtime_error("Clock recalibration failed; publisher output refused");
        Json fields = Json::array();
        auto definitions = metadata.at("metrics");
        for (const auto &item : metadata.at("information"))
            definitions.push_back(item);
        for (size_t i = 0; i < definitions.size(); ++i) {
            const auto scalar = metricTypedDouble(processed.reports[0][i]);
            if (!std::isfinite(scalar))
                throw std::runtime_error("Publisher transformation produced a nonfinite value");
            const auto &definition = definitions[i];
            fields.push_back(
                {{"name", definition.at("name")},
                 {"unit", definition.at("name") == "GpuTime" ? Json("us") : definition.at("unit")},
                 {"value", scalar},
                 {"typed_hex", hex(processed.reports[0][i])}});
        }
        const auto split = metadata.at("metrics").size();
        return {{"key", processed.keys[0]},
                {"clock_before", before},
                {"clock_after", clockState()},
                {"busy_before", previous},
                {"busy_after", busy.previousKey},
                {"calibration_range", {calibrationStart, calibrations.size()}},
                {"values", Json(fields.begin(), fields.begin() + split)},
                {"information", Json(fields.begin() + split, fields.end())}};
    }
};
MetricPublisherValues::MetricPublisherValues(MetricClockTransport &metrics, bool recorded)
    : state_(std::make_unique<State>(metrics, recorded)) {}
MetricPublisherValues::~MetricPublisherValues() = default;
void MetricPublisherValues::update(bool force) {
    auto &s = *state_;
    const auto before = s.clockState();
    const auto start = s.calibrations.size(), busy = s.busy.previousKey;
    s.clock->update(force);
    if (!s.source->lastSuccess())
        throw std::runtime_error("Driver clock refresh failed three times; report refused");
    s.refreshes.push_back({{"report_index", s.records.size()},
                           {"force", force},
                           {"calibration_range", {start, s.calibrations.size()}},
                           {"clock_before", before},
                           {"clock_after", s.clockState()},
                           {"busy_before", busy},
                           {"busy_after", s.busy.previousKey}});
}
void MetricPublisherValues::append(const Json &metadata, const Json &row) {
    auto &s = *state_;
    Json converted;
    if (s.recorded) {
        const auto raw = rawHex(row.at("raw_hex").get<std::string>());
        if (size_t(raw.size()) != metadata.at("report_size").get<size_t>())
            throw std::runtime_error("Recorded raw report size differs from metadata");
        converted = s.convert(metadata, row.at("result").at("values"));
        for (auto name : {"set", "key0", "key1", "tag", "token", "execution", "list_id", "context_slot"})
            converted[name] = row.at(name);
        converted["raw_sha256"] = sha256(raw);
        converted["available"] = row.at("result").at("available");
        converted["unavailable_reasons"] = row.at("result").at("unavailable_reasons");
    } else {
        auto values = row.at("values");
        for (const auto &item : row.at("information"))
            values.push_back(item);
        converted = s.convert(metadata, values);
        for (auto name : {"set", "event", "pass_index", "sample_index", "raw_report", "raw_sha256",
                          "available", "unavailable_reasons"})
            converted[name] = row.at(name);
        converted["start_event"] = row.value("start_event", Json());
        converted["end_event"] = row.value("end_event", Json());
    }
    s.records.push_back(std::move(converted));
}
size_t MetricPublisherValues::recordCount() const { return state_->records.size(); }
size_t MetricPublisherValues::refreshCount() const { return state_->refreshes.size(); }
Json MetricPublisherValues::report() const {
    const auto &s = *state_;
    const bool refreshed = !s.refreshes.empty();
    Json result{
        {"schema_version", 1},
        {"mode", "Recovered publisher transforms on independent DX11 reports"},
        {"maximum_ns", s.maximum},
        {"frequency_hz", s.frequency},
        {"calibrations", s.calibrations},
        {"records", s.records},
        {"refreshes", s.refreshes},
        {"calibration_policy",
         refreshed ? "initial, before query drain, and on rollover" : "initial and on rollover"},
        {"busy_state_scope", "one collection, shared across its passes and repeats, initial key zero"},
        {"production_gpa_dependency", false},
        {"complete_original_scheduling", false},
        {"limits",
         {"Collection uses the independent replay schedule; values are not a claim of complete GPA sampling "
          "equivalence.",
          std::string("Calibration occurs initially, on detected rollover") +
              (refreshed ? ", and before query drains" : "") +
              "; complete original calibration scheduling remains unverified.",
          "Raw MD values and units remain in profile.json. These values use publisher binary64 conversion.",
          "Busy uses gaps between queries and can include polling/replay overhead. First busy value retains "
          "the driver formula.",
          "Report availability is inherited from raw counter diagnostics; unavailable results are never "
          "presented as valid."}}};
    if (s.recorded)
        result.update(
            {{"mode", "Recovered publisher transforms on independently recorded DX11 command lists"},
             {"busy_state_scope", "One recorded session; shared across its lists, executions and metric-set "
                                  "changes, initial key zero"},
             {"limits",
              {"Deferred reports follow actual collector delivery order, which can differ from GPU "
               "submission order.",
               "Raw MD units and bytes are retained in the recorded profile; publisher values are a separate "
               "view.",
               "Calibration is initial, before each collector drain, and on detected rollover; complete "
               "original scheduling remains unverified.",
               "Busy uses gaps in report keys and can include polling/submission overhead. Unavailable raw "
               "reports remain unavailable.",
               "This does not parse traditional frame command lists or recover all pass/device "
               "scheduling."}}});
    return result;
}
std::string MetricPublisherValues::csv() const {
    const auto &s = *state_;
    std::vector<std::string> identity =
        s.recorded ? std::vector<std::string>{"set",  "list_id", "context_slot", "key0",
                                              "key1", "tag",     "token",        "execution"}
                   : std::vector<std::string>{"set",        "event",        "start_event", "end_event",
                                              "pass_index", "sample_index", "key"};
    auto columns = identity;
    columns.insert(columns.end(), {"metric", "unit", "value", "available"});
    Json rows = Json::array();
    for (const auto &record : s.records)
        for (const auto &value : record.at("values")) {
            Json row;
            for (const auto &name : identity)
                row[name] = record.at(name);
            row.update({{"metric", value.at("name")},
                        {"unit", value.at("unit")},
                        {"value", record.at("available").get<bool>() ? value.at("value") : Json()},
                        {"available", record.at("available")}});
            rows.push_back(std::move(row));
        }
    return metricCsv(columns, rows);
}
void MetricPublisherValues::writeCsv(const QString &path) const {
    QFile file(path);
    const auto bytes = csv();
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()))
        throw std::runtime_error("Cannot write publisher CSV");
}
} // namespace flora
