#include "MdFrameRanges.h"
#include "Experiment.h"
#include "MetricIterations.h"
#include <algorithm>
#include <sstream>
namespace flora {
using Json = nlohmann::json;
namespace {
bool natural(const Json &v) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0);
}
std::string apiName(uint16_t type) {
    auto name = commandName(type);
    if (name.starts_with("Unknown (")) {
        std::ostringstream out;
        out << "Unknown_" << std::hex << type;
        name = out.str();
    }
    return name;
}
} // namespace
Json selectMetricInterval(const Frame &frame, const Experiment *experiment, const Json &start,
                          const Json &end) {
    std::vector<Id> commands;
    for (const auto &[id, entry] : frame.entries())
        if (entry.category == 7)
            commands.push_back(id);
    if (commands.empty())
        throw std::invalid_argument("Capture has no API commands");
    const Json first = start.is_null() ? Json(commands.front()) : start;
    const Json last = end.is_null() ? Json(commands.back()) : end;
    for (const auto &id : {first, last})
        if (!natural(id) || !frame.entries().contains(id.get<Id>()) ||
            frame.entry(id.get<Id>()).category != 7)
            throw std::invalid_argument("Hardware interval endpoints must be API command IDs");
    if (first.get<Id>() > last.get<Id>())
        throw std::invalid_argument("Hardware interval start must not exceed end");
    Json rows = Json::array(), enabled = Json::array();
    for (auto id : commands) {
        if (id < first.get<Id>() || id > last.get<Id>())
            continue;
        const auto &e = frame.entry(id);
        const bool on = !experiment || experiment->enabled(id);
        rows.push_back(
            {{"event", id}, {"api", apiName(e.type)}, {"enabled", on}, {"draw_dispatch", isDraw(e.type)}});
        if (on && isDraw(e.type))
            enabled.push_back(id);
    }
    return {{"start_event", first},
            {"end_event", last},
            {"commands", rows},
            {"enabled_draw_events", enabled},
            {"boundary", "before_first_complete_command_to_after_last_complete_command"},
            {"includes", "Replay bindings, experiment resource preparation and intervening command "
                         "submission gaps; excludes restart and final readback."}};
}
Json selectFrameMetricRanges(const Frame &frame, const Json &indices, const Experiment *experiment) {
    if (!indices.is_null() && !indices.is_array())
        throw std::invalid_argument("Frame range indices must be an array");
    const auto index = buildFrameMetricIndex(frame);
    const auto &captured = index.at("ranges").at("2");
    auto selected = indices;
    if (selected.is_null()) {
        selected = Json::array();
        for (size_t i = 0; i < captured.size(); ++i)
            selected.push_back(i);
    }
    if (selected.empty() || std::any_of(selected.begin(), selected.end(), [&](const Json &v) {
            return !natural(v) || v.get<uint64_t>() >= captured.size();
        }))
        throw std::invalid_argument("Choose existing category-2 frame range indices");
    std::vector<size_t> ordered;
    for (const auto &i : selected)
        ordered.push_back(i.get<size_t>());
    std::sort(ordered.begin(), ordered.end());
    if (std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end())
        throw std::invalid_argument("Duplicate frame range indices");
    Json requested = Json::array(), ranges = Json::array();
    for (auto i : ordered)
        requested.push_back(captured[i]);
    const auto mapped = mapMetricRanges(requested, captured, index.at("ergs"));
    for (size_t i = 0; i < ordered.size(); ++i) {
        auto row = selectMetricInterval(frame, experiment, mapped[i][0], mapped[i][1]);
        row["range_index"] = ordered[i];
        row["frame_range"] = requested[i];
        if (!ranges.empty() && ranges.back()["end_event"].get<Id>() >= row["start_event"].get<Id>())
            throw std::invalid_argument("Frame ranges overlap or are out of replay order");
        ranges.push_back(std::move(row));
    }
    return {{"category", 2},
            {"independent", true},
            {"internal_api_count", index["ergs"].size()},
            {"metric_range_count", captured.size()},
            {"ranges", ranges},
            {"boundary", "recovered_endpoint_mapping_on_complete_replay_commands"},
            {"complete_original_scheduling", false}};
}
MetricIntervalCounter::MetricIntervalCounter(MetricCommandCounterClient metrics, Json interval,
                                             MetricCommandCounterClient::Consumer consume, bool callbacks)
    : metrics_(std::move(metrics)), interval_(std::move(interval)), consume_(std::move(consume)),
      callbackReports_(callbacks) {
    for (const auto &r : interval_.at("commands"))
        expected_.push_back(r.at("event").get<Id>());
}
void MetricIntervalCounter::scope(Id event, const std::function<void()> &command) {
    if (event < interval_.at("start_event").get<Id>() || event > interval_.at("end_event").get<Id>()) {
        command();
        return;
    }
    const auto index = seen_.size();
    if (failed_)
        throw std::invalid_argument("Hardware interval already failed");
    if (index >= expected_.size() || event != expected_[index]) {
        failed_ = true;
        throw std::invalid_argument("Hardware interval command boundary missing, reordered or duplicated");
    }
    if (!index) {
        metrics_.begin(callbackReports_ ? consume_ : MetricCommandCounterClient::Consumer{});
        active_ = true;
    }
    if (!active_)
        throw std::invalid_argument("Hardware interval counter is not active");
    seen_.push_back(event);
    try {
        command();
    } catch (...) {
        failed_ = true;
        throw;
    }
    if (event == interval_.at("end_event").get<Id>()) {
        auto result = metrics_.end();
        active_ = false;
        if (!callbackReports_)
            consume_(result);
        completed_ = true;
    }
}
Json MetricIntervalCounter::verify() const {
    if (failed_ || !completed_ || active_ || seen_ != expected_)
        throw std::invalid_argument("Hardware interval did not complete all command boundaries");
    return {{"start_event", interval_.at("start_event")},
            {"end_event", interval_.at("end_event")},
            {"commands_seen", seen_},
            {"reports", 1}};
}
Json MetricIntervalCounter::snapshot() const {
    return {{"seen", seen_}, {"completed", completed_}, {"active", active_}, {"failed", failed_}};
}
FrameRangeCounter::FrameRangeCounter(MetricCommandCounterClient metrics, Json ranges, Consumer consume,
                                     bool callbacks)
    : metrics_(std::move(metrics)), ranges_(std::move(ranges)), consume_(std::move(consume)),
      callbackReports_(callbacks) {}
void FrameRangeCounter::scope(Id event, const std::function<void()> &command) {
    if (failed_)
        throw std::invalid_argument("Frame range collection already failed");
    if (lastEvent_ && event <= *lastEvent_) {
        failed_ = true;
        throw std::invalid_argument("Frame commands reordered or duplicated");
    }
    lastEvent_ = event;
    if (position_ == ranges_.size()) {
        command();
        return;
    }
    const auto &current = ranges_.at(position_);
    if (event < current.at("start_event").get<Id>()) {
        command();
        return;
    }
    const auto &expected = current.at("commands");
    if (seen_.size() >= expected.size() || event != expected[seen_.size()].at("event").get<Id>()) {
        failed_ = true;
        throw std::invalid_argument("Frame range command missing, reordered or duplicated");
    }
    if (seen_.empty()) {
        MetricCommandCounterClient::Consumer callback;
        if (callbackReports_)
            callback = [consume = consume_, row = current](MetricResult &r) { consume(row, r); };
        metrics_.begin(std::move(callback));
        active_ = true;
    }
    seen_.push_back(event);
    try {
        command();
    } catch (...) {
        failed_ = true;
        throw;
    }
    if (event == current.at("end_event").get<Id>()) {
        if (callbackReports_)
            metrics_.submit();
        else {
            auto result = metrics_.end();
            consume_(current, result);
        }
        active_ = false;
        audit_.push_back({{"range_index", current.at("range_index")},
                          {"start_event", current.at("start_event")},
                          {"end_event", event},
                          {"commands_seen", seen_}});
        seen_.clear();
        ++position_;
    }
}
Json FrameRangeCounter::verify() const {
    if (failed_ || active_ || position_ != ranges_.size())
        throw std::invalid_argument("Frame range collection did not complete all boundaries");
    return audit_;
}
Json FrameRangeCounter::snapshot() const {
    return {{"position", position_}, {"seen", seen_},
            {"audit", audit_},       {"active", active_},
            {"failed", failed_},     {"last_event", lastEvent_ ? Json(*lastEvent_) : Json()}};
}
} // namespace flora
