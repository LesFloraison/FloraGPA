#include "MetricIterations.h"
#include <algorithm>
#include <map>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }
bool natural(const Json &v, uint64_t maximum = UINT64_MAX) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0) &&
           v.get<uint64_t>() <= maximum;
}
uint32_t u32(const Json &v, const char *message) {
    if (!natural(v, UINT32_MAX))
        fail(message);
    return v.get<uint32_t>();
}
double numericValue(const Json &v) {
    // Numeric payloads follow Python's float conversion, including bool scalars.
    return v.is_boolean() ? (v.get<bool>() ? 1. : 0.) : v.get<double>();
}
Json zero() {
    return {{"values", Json::array()}, {"kind", 0},     {"weight", 0.}, {"median", 0.},
            {"minimum", 0.},           {"maximum", 0.}, {"mean", 0.},   {"variation_percent", 0.}};
}
Json matrix(size_t rows, size_t columns) {
    Json result = Json::array();
    for (size_t r = 0; r < rows; ++r) {
        Json row = Json::array();
        for (size_t c = 0; c < columns; ++c)
            row.push_back(zero());
        result.push_back(std::move(row));
    }
    return result;
}
Json iterated(const IteratedMetric &v) {
    const auto &s = v.statistics;
    return {{"values", v.values},   {"kind", v.kind},
            {"weight", v.weight},   {"median", s.median},
            {"minimum", s.minimum}, {"maximum", s.maximum},
            {"mean", s.mean},       {"variation_percent", s.variationPercent}};
}
void extend(Json &destination, const Json &source) {
    for (const auto &item : source)
        destination.push_back(item);
}
size_t position(const Json &values, const Json &value) {
    for (size_t i = 0; i < values.size(); ++i)
        if (metricIdentityEqual(values[i], value))
            return i;
    return values.size();
}
} // namespace
MetricPassSelection selectMetricPasses(const Json &groupCount, const Json &requestedPass,
                                       const Json &prepareFlag) {
    const auto groups = u32(groupCount, "Invalid group count"),
               requested = u32(requestedPass, "Invalid pass selector");
    if (!prepareFlag.is_boolean())
        fail("Invalid preparation flag");
    const auto count = std::max(1u, groups);
    const auto flag = prepareFlag.get<bool>();
    if (requested != metricAllPasses && requested >= count)
        fail("Pass outside prepared plan");
    if (requested == metricAllPasses)
        return {0, flag ? 1u : count};
    return flag ? MetricPassSelection{0, count} : MetricPassSelection{requested, 1};
}
Json selectMetricRangeTable(const Json &categorized, const Json &fallback) {
    return categorized.value("2", fallback);
}
Json mapMetricRanges(const Json &requested, const Json &captured, const Json &internalIds) {
    for (const auto *rows : {&requested, &captured})
        for (const auto &r : *rows)
            if (r.size() != 3 ||
                std::any_of(r.begin(), r.end(), [](const Json &v) { return !natural(v, UINT32_MAX); }))
                fail("Range records require three uint32 fields");
    for (const auto &v : internalIds)
        if (!natural(v, UINT32_MAX))
            fail("Internal IDs require uint32 values");
    const auto overlap = [](const Json &a, const Json &b) {
        return a[1].get<uint32_t>() <= b[2].get<uint32_t>() && b[1].get<uint32_t>() <= a[2].get<uint32_t>();
    };
    const auto lookup = [&](uint32_t index) {
        return index < internalIds.size() ? internalIds[index] : Json(UINT32_MAX);
    };
    Json result = Json::array();
    for (const auto &request : requested) {
        const Json *first = nullptr, *last = nullptr;
        for (const auto &r : captured)
            if (overlap(r, request)) {
                if (!first)
                    first = &r;
                if (r[2].get<uint32_t>() <= request[2].get<uint32_t>())
                    last = &r;
            }
        result.push_back(!first || !last ? Json::array({0, 0})
                                         : Json::array({lookup((*first)[2].get<uint32_t>()),
                                                        lookup((*last)[2].get<uint32_t>())}));
    }
    return result;
}
Json parseMetricQueryFlags(const Json &iterations, const Json &descriptions, const Json &initial) {
    std::map<uint64_t, std::map<uint64_t, Json>> output;
    for (const auto &row : initial) {
        const auto &r = row.at("range");
        if (!natural(r) || output.contains(r.get<uint64_t>()))
            fail("Invalid flag range index");
        auto &samples = output[r.get<uint64_t>()];
        for (auto sample : row.at("samples")) {
            const auto &i = sample.at("iteration");
            if (!natural(i) || samples.contains(i.get<uint64_t>()))
                fail("Invalid flag iteration index");
            if (!natural(sample.at("index"), UINT32_MAX))
                fail("Invalid flag sample index");
            std::set<std::string> flags;
            for (const auto &s : sample.at("flags")) {
                if (!s.is_string())
                    fail("Invalid flag label");
                flags.insert(s.get<std::string>());
            }
            samples[i.get<uint64_t>()] = {{"iteration", i}, {"index", sample.at("index")}, {"flags", flags}};
        }
    }
    const auto result = [&] {
        Json rows = Json::array();
        for (const auto &[range, samples] : output) {
            Json items = Json::array();
            for (const auto &[i, sample] : samples)
                items.push_back(sample);
            rows.push_back({{"range", range}, {"samples", items}});
        }
        return rows;
    };
    if (iterations.empty() || iterations.at(0).at("metrics").empty())
        return result();
    std::vector<std::string> labels;
    for (const auto &d : descriptions) {
        if (!natural(d.at("kind"), UINT32_MAX) || !d.at("label").is_string())
            fail("Invalid query flag descriptor");
        if (d.at("kind").get<uint32_t>() == 0)
            labels.push_back(d.at("label").get<std::string>());
    }
    const auto metrics = iterations[0]["metrics"].size(),
               ranges = iterations[0]["metrics"][0].at("values").size();
    for (const auto &it : iterations) {
        if (it.at("metrics").size() != metrics)
            fail("Query flag metric shape differs");
        for (const auto &row : it["metrics"]) {
            if (row.at("values").size() != ranges || row.at("aux").size() < ranges)
                fail("Query flag range shape differs");
            for (size_t r = 0; r < ranges; ++r)
                if (!natural(row["aux"][r]))
                    fail("Expected uint64 query flags");
        }
    }
    for (size_t i = 0; i < iterations.size(); ++i)
        for (const auto &row : iterations[i]["metrics"])
            for (size_t r = 0; r < ranges; ++r) {
                if (!output.contains(r))
                    for (size_t j = 0; j < iterations.size(); ++j)
                        output[r][j] = {{"iteration", j}, {"index", uint32_t(j)}, {"flags", Json::array()}};
                const auto mask = row["aux"][r].get<uint64_t>();
                if (mask) {
                    auto &samples = output[r];
                    if (!samples.contains(i))
                        samples[i] = {{"iteration", i}, {"index", 0}, {"flags", Json::array()}};
                    std::set<std::string> flags = samples[i]["flags"].get<std::set<std::string>>();
                    for (size_t bit = 0; bit < std::min(size_t(64), labels.size()); ++bit)
                        if (mask & (uint64_t(1) << bit))
                            flags.insert(labels[bit]);
                    samples[i]["flags"] = flags;
                }
            }
    return result();
}
Json updateMetricSampleTimes(const Json &previous, const Json &parallelRanges) {
    const auto source = parallelRanges.empty() ? previous : parallelRanges[0];
    for (const auto &pair : source)
        if (pair.size() != 2 || !natural(pair[0]) || !natural(pair[1]))
            fail("Expected uint64 timestamp begin/duration pairs");
    return source;
}
Json chooseWeightMetric(const Json &descriptions) {
    for (const auto name : {"intel.gpu_core_clocks", "intel.duration", "intel.total_time", "intel.time",
                            "intel.debug.incrementing", "intel.frame.duration", "amd.gpu_duration",
                            "gfx.elapsed_time", "fake_uri_0"})
        for (const auto &d : descriptions)
            if (d.at("name") == name)
                return d.at("id");
    fail("No duration or core-clock weight metric");
}
uint32_t metricIterationPass(const Json &requested, const Json &mapping) {
    auto selected = u32(requested, "Invalid pass selector");
    if (selected != metricAllPasses && !mapping.empty()) {
        if (selected >= mapping.size())
            fail("Pass mapping index outside table");
        selected = u32(mapping[selected], "Invalid mapped pass");
    }
    return selected;
}
Json prepareMetricIterationValues(const Json &iterations, const Json &descriptions, const Json &requested,
                                  const Json &weights, const Json &groups, const Json &initial) {
    if (iterations.empty() || iterations[0].at("metrics").empty())
        fail("No metric iteration data");
    const auto count = iterations[0]["metrics"].size(), ranges = weights.size();
    for (const auto &it : iterations)
        if (it.at("metrics").size() != count)
            fail("Iteration metric shape differs");
    Json uniqueGroups = Json::array();
    for (const auto &group : groups)
        if (position(uniqueGroups, group) == uniqueGroups.size())
            uniqueGroups.push_back(group);
    const auto rowCount = groups.empty() ? ranges : uniqueGroups.size();
    auto result = initial.is_null() ? matrix(rowCount, requested.size()) : initial;
    if (result.size() != rowCount || std::any_of(result.begin(), result.end(), [&](const Json &row) {
            return row.size() != requested.size();
        }))
        fail("Initial matrix shape differs");
    for (size_t m = 0; m < count; ++m) {
        const auto &mid = iterations.back()["metrics"][m].at("metric");
        for (const auto &it : iterations)
            if (!metricIdentityEqual(it["metrics"][m].at("metric"), mid) ||
                it["metrics"][m].at("values").size() != ranges)
                fail("Metric identity or range shape differs between iterations");
        const auto desc = std::find_if(descriptions.begin(), descriptions.end(),
                                       [&](const Json &d) { return metricIdentityEqual(d.at("id"), mid); });
        const auto destination = position(requested, mid);
        if (desc == descriptions.end() || destination == requested.size())
            fail("Result metric outside requested catalog");
        if (!groups.empty() && groups.size() != ranges)
            fail("Range, weight and group counts differ");
        if (!ranges)
            fail("No metric ranges");
        std::vector<int64_t> ids;
        for (const auto &g : groups) {
            if (!natural(g, INT32_MAX))
                fail("Range groups must be nonnegative int32 IDs");
            ids.push_back(g.get<int64_t>());
        }
        const auto kind = u32(desc->at("kind"), "Metric kind must be a byte");
        if (kind > 255)
            fail("Metric kind must be a byte");
        std::vector<std::vector<double>> columns(ranges);
        for (size_t r = 0; r < ranges; ++r)
            for (const auto &it : iterations)
                columns[r].push_back(numericValue(it["metrics"][m]["values"][r]));
        std::vector<double> numericWeights;
        for (const auto &weight : weights)
            numericWeights.push_back(numericValue(weight));
        const auto aggregated = aggregateMetricRanges(columns, int(kind), numericWeights, ids);
        for (size_t r = 0; r < aggregated.size(); ++r)
            result[r][destination] = iterated(aggregated[r]);
    }
    return result;
}
Json MetricOuterPassRunner::run(const Json &metricIds, const Json &ranges, const Json &requestedPass,
                                const Json &requestFlag, const std::function<bool()> &cancel, Json result) {
    for (const auto &id : metricIds)
        if (!natural(id, UINT32_MAX))
            fail("Invalid metric ID");
    if (!requestFlag.is_boolean())
        fail("Invalid request flag");
    if (result.is_null())
        result = {{"metrics", Json::array()}, {"parallel_ranges", Json::array()}, {"flag", false}};
    const auto plan = transport_.prepare(metricIds);
    MetricPassSelection passes;
    try {
        passes = selectMetricPasses(plan.at("groups").size(), requestedPass, plan.at("flag"));
    } catch (const std::invalid_argument &) {
        return {{"status", "invalid_pass"}, {"result", result}};
    }
    Json accumulated = Json::array(), parallel = Json::array();
    for (uint64_t n = 0; n < passes.count; ++n) {
        if (cancel && cancel())
            return {{"status", "cancelled"}, {"result", result}};
        const auto incoming = transport_.replay(uint32_t(passes.first + n), ranges, requestFlag.get<bool>());
        extend(result.at("metrics"), incoming.at("metrics"));
        extend(result.at("parallel_ranges"), incoming.at("parallel_ranges"));
        result["flag"] = incoming.at("flag");
        if (result["metrics"].empty())
            return {{"status", "empty_metrics"}, {"result", result}};
        if (result["metrics"][0].at("values").size() != ranges.size())
            return {{"status", "range_mismatch"}, {"result", result}};
        extend(accumulated, result["metrics"]);
        extend(parallel, result["parallel_ranges"]);
        for (auto &row : result["parallel_ranges"])
            row = Json::array();
    }
    transport_.prepare(Json::array());
    result.update({{"metrics", accumulated}, {"parallel_ranges", parallel}, {"flag", false}});
    return {{"status", "success"}, {"result", result}};
}
MetricIterationError::MetricIterationError(Json result)
    : std::runtime_error("Metric acquisition incomplete: " + result.at("reason").get<std::string>()),
      result_(std::move(result)) {}
Json MetricIterationRunner::execute(const Json &ids, const Json &ranges, const Json &options,
                                    const std::function<bool()> &cancel) {
    const auto samples = u32(options.value("samples", Json(1)), "Expected uint32 iteration count");
    const auto requestFlag = options.value("request_flag", Json(false));
    if (!requestFlag.is_boolean())
        fail("Invalid request flag");
    auto weights = options.value("weights", Json::array());
    const auto initial = options.value("initial", Json());
    auto sampleTimes = updateMetricSampleTimes(options.value("initial_sample_times", Json()).is_null()
                                                   ? Json::array()
                                                   : options.at("initial_sample_times"),
                                               Json::array());
    auto queryFlags = parseMetricQueryFlags(Json::array(), Json::array(),
                                            options.value("initial_query_flags", Json()).is_null()
                                                ? Json::array()
                                                : options.at("initial_query_flags"));
    bool includeTimes = !options.value("initial_sample_times", Json()).is_null(),
         includeFlags = !options.value("initial_query_flags", Json()).is_null();
    if (ids.empty()) {
        transport_.replay(0, ranges, requestFlag.get<bool>());
        Json values = initial;
        if (initial.is_null()) {
            values = Json::array();
            for (size_t i = 0; i < ranges.size(); ++i)
                values.push_back(Json::array());
        }
        Json direct = {{"status", "success"},
                       {"complete", true},
                       {"reason", "Direct non-metric replay"},
                       {"direct_replay", true},
                       {"weight_metric", nullptr},
                       {"weights", weights},
                       {"weight_attempts", Json::array()},
                       {"selected_pass", 0},
                       {"iteration_count", 1},
                       {"iteration_statuses", Json::array()},
                       {"iterations", Json::array()},
                       {"values", values}};
        if (includeTimes)
            direct["sample_times"] = sampleTimes;
        if (includeFlags)
            direct["query_flags"] = queryFlags;
        return direct;
    }
    const auto descriptions = transport_.descriptions();
    MetricOuterPassRunner runner(transport_);
    const auto flagDescriptions = transport_.queryFlagDescriptions();
    includeFlags = includeFlags || !flagDescriptions.empty();
    auto values = initial.is_null() ? matrix(ranges.size(), ids.size()) : initial;
    if (values.size() != ranges.size() ||
        std::any_of(values.begin(), values.end(), [&](const Json &row) { return row.size() != ids.size(); }))
        fail("Initial matrix shape differs");
    Json weightMetric, selected, attempts = Json::array(), iterations = Json::array(),
                                 statuses = Json::array();
    uint32_t count = 0;
    const auto result = [&](const char *status, bool complete, const char *reason) {
        Json out = {{"status", status},
                    {"complete", complete},
                    {"reason", reason},
                    {"weight_metric", weightMetric},
                    {"weights", weights},
                    {"weight_attempts", attempts},
                    {"selected_pass", selected},
                    {"iteration_count", count},
                    {"iteration_statuses", statuses},
                    {"iterations", iterations},
                    {"values", values}};
        if (includeTimes)
            out["sample_times"] = sampleTimes;
        if (includeFlags)
            out["query_flags"] = queryFlags;
        return out;
    };
    if (weights.empty()) {
        try {
            weightMetric = chooseWeightMetric(descriptions);
        } catch (const std::invalid_argument &) {
            return result("missing_weight_metric", false, "No weight metric");
        }
        auto measured = runner.run(Json::array({weightMetric}), ranges, 0, requestFlag);
        attempts.push_back(measured.at("status"));
        if (measured["status"] != "success" && measured["status"] != "cancelled") {
            // The recovered assertion evaluates twice more, retaining its earlier result.
            for (int i = 0; i < 2; ++i) {
                measured = runner.run(Json::array({weightMetric}), ranges, 0, requestFlag, {},
                                      measured.at("result"));
                attempts.push_back(measured.at("status"));
            }
            return result(measured["status"].get<std::string>().c_str(), false,
                          "Weight failure early return");
        }
        const auto &rows = measured["result"]["metrics"];
        if (rows.size() != 1 || !metricIdentityEqual(rows[0].at("metric"), weightMetric) ||
            rows[0].at("values").size() != ranges.size())
            return result("invalid_weight_result", false, "Weight report identity or range count mismatch");
        weights = rows[0]["values"];
    }
    selected = metricIterationPass(options.value("requested_pass", Json(0)),
                                   options.value("pass_mapping", Json::array()));
    count = selected == 0 ? samples : 1;
    for (uint32_t i = 0; i < count; ++i) {
        const auto measured = runner.run(ids, ranges, selected, requestFlag, cancel);
        statuses.push_back(measured.at("status"));
        iterations.push_back(measured.at("result"));
        if (!measured["result"]["parallel_ranges"].empty()) {
            includeTimes = true;
            sampleTimes = updateMetricSampleTimes(sampleTimes, measured["result"]["parallel_ranges"]);
        }
    }
    if (iterations.empty())
        return result("success", false, "No iterations");
    if (iterations[0]["metrics"].empty())
        return result("success", false, "First iteration has no metrics");
    if (iterations[0]["metrics"][0]["values"].size() != weights.size())
        return result("success", false, "Weight and first result range counts differ");
    values = prepareMetricIterationValues(iterations, descriptions, ids, weights, Json::array(), values);
    if (!flagDescriptions.empty())
        queryFlags = parseMetricQueryFlags(iterations, flagDescriptions, queryFlags);
    const auto complete =
        std::all_of(statuses.begin(), statuses.end(), [](const Json &status) { return status == "success"; });
    return result("success", complete, complete ? "Complete" : "Inner acquisition failed or cancelled");
}
Json MetricIterationRunner::collect(const Json &ids, const Json &ranges, const Json &options,
                                    const std::function<bool()> &cancel) {
    auto result = execute(ids, ranges, options, cancel);
    if (!result.at("complete").get<bool>())
        throw MetricIterationError(result);
    return result;
}
Json MetricIterationRunner::executeRanges(const Json &ids, const Json &requested, const Json &captured,
                                          const Json &internalIds, const Json &options,
                                          const std::function<bool()> &cancel) {
    const auto ranges = mapMetricRanges(requested, captured, internalIds);
    auto result = execute(ids, ranges, options, cancel);
    result["replay_ranges"] = ranges;
    return result;
}
Json MetricIterationRunner::executeFrame(const Frame &frame, const Json &ids, const Json &requested,
                                         const Json &options, const std::function<bool()> &cancel) {
    const auto index = buildFrameMetricIndex(frame), captured = index.at("ranges").at("2");
    auto result = executeRanges(ids, requested.is_null() ? captured : requested, captured, index.at("ergs"),
                                options, cancel);
    result["frame_index"] = {{"internal_api_count", index["ergs"].size()},
                             {"metric_range_count", captured.size()},
                             {"excluded_api_count", index["excluded"].size()},
                             {"independent", true}};
    return result;
}
} // namespace flora
