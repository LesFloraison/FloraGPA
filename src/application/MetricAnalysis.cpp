#include "MetricAnalysis.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace flora {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const std::string &message) { throw std::invalid_argument(message); }
bool sumKind(int kind) { return kind == 0 || kind == 1 || kind == 2 || kind == 5; }
Json get(const Json &object, const char *key) { return object.value(key, Json()); }
bool integer(const Json &value) { return value.is_number_integer(); }
bool nonnegative(const Json &value) {
    return integer(value) && (value.is_number_unsigned() || value.get<int64_t>() >= 0);
}
bool indexIn(const Json &value, size_t size) { return nonnegative(value) && value.get<uint64_t>() < size; }
bool finiteNumber(const Json &value) { return value.is_number() && std::isfinite(value.get<double>()); }
int compareIntegers(const Json &a, const Json &b) {
    const bool an = !nonnegative(a), bn = !nonnegative(b);
    if (an != bn)
        return an ? -1 : 1;
    if (an) {
        const auto x = a.get<int64_t>(), y = b.get<int64_t>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    const auto x = a.get<uint64_t>(), y = b.get<uint64_t>();
    return x < y ? -1 : x > y ? 1 : 0;
}
int compareIntegerFloat(const Json &a, double b) {
    // Converting uint64 to double first would equate distinct IDs above 2^53.
    if (b >= 0x1p64)
        return -1;
    if (b < -0x1p63)
        return 1;
    const Json truncated = b < 0 ? Json(int64_t(b)) : Json(uint64_t(b));
    const auto result = compareIntegers(a, truncated);
    if (result)
        return result;
    const auto rounded = truncated.get<double>();
    return rounded < b ? -1 : rounded > b ? 1 : 0;
}
// Python's untyped tuple comparisons treat bool and numeric values alike. Explicit
// identity type checks above remain strict (in particular bool is not an int).
bool equal(const Json &a, const Json &b) {
    if (a.is_boolean() && b.is_number())
        return equal(Json(a.get<bool>() ? 1 : 0), b);
    if (b.is_boolean() && a.is_number())
        return equal(b, a);
    if (integer(a) && integer(b))
        return compareIntegers(a, b) == 0;
    if (integer(a) && b.is_number_float())
        return !std::isnan(b.get<double>()) && compareIntegerFloat(a, b.get<double>()) == 0;
    if (integer(b) && a.is_number_float())
        return equal(b, a);
    if (a.is_object() && b.is_object()) {
        if (a.size() != b.size())
            return false;
        for (auto it = a.begin(); it != a.end(); ++it)
            if (!b.contains(it.key()) || !equal(it.value(), b.at(it.key())))
                return false;
        return true;
    }
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!equal(a[i], b[i]))
                return false;
        return true;
    }
    return a == b;
}
bool lessNumber(Json a, Json b) {
    if (a.is_boolean())
        a = a.get<bool>() ? 1 : 0;
    if (b.is_boolean())
        b = b.get<bool>() ? 1 : 0;
    if (integer(a) && integer(b))
        return compareIntegers(a, b) < 0;
    if (integer(a))
        return compareIntegerFloat(a, b.get<double>()) < 0;
    if (integer(b))
        return compareIntegerFloat(b, a.get<double>()) > 0;
    return a.get<double>() < b.get<double>();
}
size_t find(const Json &values, const Json &value) {
    for (size_t i = 0; i < values.size(); ++i)
        if (equal(values[i], value))
            return i;
    return values.size();
}
bool unique(const Json &values) {
    for (size_t i = 0; i < values.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (equal(values[i], values[j]))
                return false;
    return true;
}
Json names(const Json &objects, const char *key) {
    Json result = Json::array();
    for (const auto &object : objects)
        result.push_back(object.at(key));
    return result;
}
Json published(double value) { return std::isfinite(value) ? Json(value) : Json(); }
Json publishedStatistics(const MetricStatistics &s) {
    return {{"median", published(s.median)},
            {"minimum", published(s.minimum)},
            {"maximum", published(s.maximum)},
            {"mean", published(s.mean)},
            {"variation_percent", published(s.variationPercent)}};
}
} // namespace

bool metricIdentityEqual(const Json &a, const Json &b) { return equal(a, b); }
double metricSequentialSum(std::span<const double> values) {
    double total = 0;
    for (const auto value : values)
        total += value;
    return total;
}
MetricStatistics metricStatistics(std::span<const double> input) {
    if (input.empty())
        return {};
    std::vector<double> values;
    for (const auto value : input)
        if (!std::isnan(value))
            values.push_back(value);
    if (values.empty()) {
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan, nan, nan};
    }
    // Preserve signed-zero ordering and the reference's sequential arithmetic.
    std::stable_sort(values.begin(), values.end());
    const auto n = values.size(), middle = n / 2;
    MetricStatistics result;
    result.median = n % 2 ? values[middle] : (values[middle] + values[middle - 1]) * .5;
    result.minimum = values.front();
    result.maximum = values.back();
    result.mean = metricSequentialSum(values) / double(n);
    if (result.mean != 0) {
        double variance = 0;
        for (const auto value : values) {
            const double delta = value - result.mean;
            variance += delta * delta;
        }
        result.variationPercent = std::sqrt(variance / double(n)) / result.mean * 100;
    }
    return result;
}
IteratedMetric iteratedMetric(std::vector<double> values, int kind, double weight) {
    if (kind < 0 || kind > 255)
        fail("Metric kind must be a byte");
    if (weight == 0)
        for (auto &value : values)
            value *= 0.;
    const auto stats = metricStatistics(values);
    return {std::move(values), kind, sumKind(kind) ? 1. : weight, stats};
}
IteratedMetric combineMetrics(const IteratedMetric &left, const IteratedMetric &right) {
    if (right.values.empty())
        return left;
    if (left.values.empty())
        return right;
    if (left.values.size() != right.values.size())
        fail("Different metric iteration counts");
    if (left.kind != right.kind)
        fail("Different metric kinds");
    const auto weight = left.weight + right.weight;
    std::vector<double> values;
    for (size_t i = 0; i < left.values.size(); ++i) {
        double value = left.weight * left.values[i] + right.weight * right.values[i];
        if (!sumKind(left.kind) && weight > 0)
            value /= weight;
        values.push_back(value);
    }
    return iteratedMetric(std::move(values), left.kind, weight);
}
std::vector<IteratedMetric> aggregateMetricRanges(const std::vector<std::vector<double>> &values, int kind,
                                                  std::span<const double> weights,
                                                  std::span<const int64_t> groups) {
    if (values.size() != weights.size() || (!groups.empty() && groups.size() != values.size()))
        fail("Range, weight and group counts differ");
    if (values.empty())
        fail("No metric ranges");
    for (const auto group : groups)
        if (group < 0 || group >= 0x80000000LL)
            fail("Range groups must be nonnegative int32 IDs");
    for (const auto &v : values)
        if (v.size() != values.front().size())
            fail("Different metric iteration counts");
    std::map<int64_t, IteratedMetric> combined;
    for (size_t i = 0; i < values.size(); ++i) {
        const auto group = groups.empty() ? int64_t(i) : groups[i];
        combined[group] = combineMetrics(combined[group], iteratedMetric(values[i], kind, weights[i]));
    }
    std::vector<IteratedMetric> result;
    for (auto &[group, value] : combined)
        result.push_back(std::move(value));
    return result;
}
Json assembleMetricIterations(const Json &iterations) {
    if (iterations.empty() || iterations.at(0).empty())
        fail("No metric iterations");
    const auto ids = names(iterations[0], "metric");
    if (!unique(ids) || std::any_of(ids.begin(), ids.end(), [](const auto &id) { return !nonnegative(id); }))
        fail("Invalid metric identities");
    const auto count = iterations[0][0].at("values").size();
    for (const auto &iteration : iterations) {
        for (const auto &m : iteration)
            if (!integer(m.at("metric")))
                fail("Invalid iteration metric identity");
        if (!equal(names(iteration, "metric"), ids))
            fail("Metric identities differ between iterations");
        for (const auto &m : iteration)
            if (m.at("values").size() != count)
                fail("Metric range counts differ");
        for (const auto &m : iteration)
            for (const auto &v : m.at("values"))
                if (!v.is_number())
                    fail("Non-numeric metric value");
    }
    Json result = Json::array();
    for (size_t m = 0; m < ids.size(); ++m) {
        Json ranges = Json::array();
        for (size_t r = 0; r < count; ++r) {
            Json samples = Json::array();
            for (const auto &iteration : iterations)
                samples.push_back(iteration[m]["values"][r].get<double>());
            ranges.push_back(std::move(samples));
        }
        result.push_back({{"metric", ids[m]}, {"ranges", ranges}});
    }
    return result;
}
Json metricProfileMatrix(const Json &profile) {
    const auto sampleCount = get(profile, "sample_count");
    if (!indexIn(sampleCount, 101) || sampleCount.get<uint64_t>() == 0)
        fail("Invalid sample count");
    const auto n = sampleCount.get<size_t>();
    const auto &sets = profile.at("sets");
    const auto setNames = names(sets, "name");
    if (setNames.empty() || !unique(setNames))
        fail("Invalid metric set roster");
    const auto &events = profile.at("selected_events");
    if (!unique(events) ||
        std::any_of(events.begin(), events.end(), [](const auto &e) { return !integer(e); }))
        fail("Invalid event roster");
    const auto mode = profile.at("selection_mode");
    Json ranges = Json::array(), roster = Json::array();
    if (mode == "events") {
        if (events.empty())
            fail("No event ranges");
        for (const auto &event : events)
            ranges.push_back(Json::array({event, nullptr, nullptr}));
    } else if (mode == "interval") {
        const auto &interval = profile.at("interval");
        const auto &start = interval.at("start_event"), &end = interval.at("end_event");
        if (!integer(start) || !integer(end) || compareIntegers(start, end) > 0)
            fail("Invalid interval identity");
        ranges.push_back(Json::array({nullptr, start, end}));
    } else if (mode == "frame_ranges") {
        roster = profile.at("frame_ranges").at("ranges");
        const auto indices = names(roster, "range_index");
        if (roster.empty() || !unique(indices) ||
            std::any_of(indices.begin(), indices.end(), [](const auto &i) { return !nonnegative(i); }))
            fail("Invalid FrameFile range roster");
        for (const auto &r : roster) {
            const auto &start = r.at("start_event"), &end = r.at("end_event");
            if (!integer(start) || !integer(end) || compareIntegers(start, end) > 0)
                fail("Invalid FrameFile range boundary");
            ranges.push_back(Json::array({nullptr, start, end}));
        }
        for (size_t i = 1; i < ranges.size(); ++i)
            if (compareIntegers(ranges[i - 1][2], ranges[i][1]) >= 0)
                fail("Overlapping FrameFile ranges");
    } else
        fail("Unknown metric range mode");
    if (get(profile, "sample_schedule") != "metric_set_then_iteration")
        fail("Unknown metric pass schedule");
    const auto scheduleSize = sets.size() * n;
    const auto &passes = profile.at("validation").at("passes");
    if (passes.size() != scheduleSize)
        fail("Missing metric pass validation");
    std::set<size_t> seen;
    for (const auto &pass : passes) {
        const auto idx = get(pass, "pass_index"), iteration = get(pass, "sample_index");
        if (!indexIn(idx, scheduleSize) || !integer(iteration) || seen.contains(idx.get<size_t>()))
            fail("Invalid metric pass identity");
        const auto i = idx.get<size_t>();
        if (!equal(pass.at("set"), setNames[i / n]) || !equal(iteration, Json(i % n)))
            fail("Metric pass schedule mismatch");
        if (get(pass, "image_matches") != Json(true) || get(pass, "event_mapping_valid") != Json(true))
            fail("Unaccepted metric pass");
        seen.insert(i);
    }
    for (const auto &s : sets) {
        const auto symbols = names(s.at("metrics"), "name");
        if (symbols.empty() || !unique(symbols))
            fail("Invalid metric descriptor roster");
    }
    // Index only accepted roster positions: serialization order never determines identity.
    std::map<std::array<size_t, 3>, const Json *> rows;
    for (const auto &row : profile.at("records")) {
        const auto idx = get(row, "pass_index"), iteration = get(row, "sample_index"),
                   event = get(row, "event");
        if (!indexIn(idx, scheduleSize) || !integer(iteration))
            fail("Invalid metric record pass identity");
        const auto start = get(row, "start_event"), end = get(row, "end_event");
        if (mode == "events" && !integer(event))
            fail("Invalid record event");
        if (mode == "interval" && (!integer(start) || !integer(end)))
            fail("Invalid record interval");
        const auto r = Json::array({event, start, end});
        if (mode == "frame_ranges") {
            const auto rangeIndex = get(row, "range_index");
            if (!integer(rangeIndex))
                fail("Invalid FrameFile record index");
            const auto match = std::find_if(roster.begin(), roster.end(), [&](const auto &info) {
                return equal(info.at("range_index"), rangeIndex);
            });
            if (match == roster.end() ||
                !equal(r, Json::array({nullptr, match->at("start_event"), match->at("end_event")})))
                fail("FrameFile record index and boundary disagree");
        }
        const auto setIndex = find(setNames, row.at("set")), rangeIndex = find(ranges, r),
                   passIndex = idx.get<size_t>();
        if (setIndex == sets.size() || rangeIndex == ranges.size() || !indexIn(iteration, n) ||
            setIndex != passIndex / n || iteration != passIndex % n)
            fail("Missing, duplicate or mismatched metric record");
        const std::array key{setIndex, rangeIndex, iteration.get<size_t>()};
        if (rows.contains(key))
            fail("Missing, duplicate or mismatched metric record");
        if (row.at("values").size() != sets[setIndex].at("metrics").size() ||
            !row.at("available").is_boolean())
            fail("Malformed metric record");
        rows.emplace(key, &row);
    }
    if (rows.size() != sets.size() * ranges.size() * n)
        fail("Missing complete metric ranges or iterations");
    Json result = Json::array();
    for (size_t s = 0; s < sets.size(); ++s) {
        Json metrics = Json::array();
        const auto &definitions = sets[s].at("metrics");
        for (size_t m = 0; m < definitions.size(); ++m) {
            Json metricRanges = Json::array();
            for (size_t r = 0; r < ranges.size(); ++r) {
                Json values = Json::array();
                for (size_t i = 0; i < n; ++i) {
                    const auto &row = *rows.at({s, r, i});
                    const auto &v = row.at("values")[m].at("value");
                    values.push_back(
                        row.at("available").get<bool>() && finiteNumber(v) ? Json(v.get<double>()) : Json());
                }
                metricRanges.push_back({{"event", ranges[r][0]},
                                        {"start_event", ranges[r][1]},
                                        {"end_event", ranges[r][2]},
                                        {"values", values}});
            }
            const auto &d = definitions[m];
            metrics.push_back({{"metric", d.at("name")},
                               {"label", d.at("label")},
                               {"unit", d.at("unit")},
                               {"ranges", metricRanges}});
        }
        result.push_back({{"set", setNames[s]}, {"metrics", metrics}});
    }
    return result;
}
Json metricSampleSummary(const Json &values) {
    std::vector<double> valid;
    for (const auto &v : values)
        if (finiteNumber(v))
            valid.push_back(v.get<double>());
    auto result = publishedStatistics(metricStatistics(valid));
    if (valid.empty())
        for (auto &v : result)
            v = nullptr;
    result.update({{"total_samples", values.size()},
                   {"valid_samples", valid.size()},
                   {"invalid_samples", values.size() - valid.size()}});
    return result;
}
Json summarizeMetricMatrix(const Json &matrix) {
    Json result = Json::array();
    for (const auto &group : matrix) {
        const auto &metrics = group.at("metrics");
        for (size_t i = 0; i < metrics.at(0).at("ranges").size(); ++i)
            for (const auto &metric : metrics) {
                const auto &r = metric.at("ranges").at(i);
                auto row = metricSampleSummary(r.at("values"));
                row.update({{"set", group.at("set")},
                            {"event", r.at("event")},
                            {"start_event", r.at("start_event")},
                            {"end_event", r.at("end_event")},
                            {"metric", metric.at("metric")},
                            {"label", metric.at("label")},
                            {"unit", metric.at("unit")}});
                result.push_back(std::move(row));
            }
    }
    return result;
}
Json summarizeMetricRecords(const Json &profile) {
    return summarizeMetricMatrix(metricProfileMatrix(profile));
}
Json groupMetricChoices(const Json &choices) {
    std::vector<std::set<int32_t>> compatible;
    Json passes = Json::array();
    for (size_t metric = 0; metric < choices.size(); ++metric) {
        const auto &groups = choices[metric];
        std::set<int32_t> allowed;
        for (const auto &g : groups) {
            if (!integer(g) || compareIntegers(g, Json(INT32_MIN)) < 0 ||
                compareIntegers(g, Json(INT32_MAX)) > 0)
                fail("Compatibility groups must be int32");
            allowed.insert(g.get<int32_t>());
        }
        auto selected = passes.size();
        for (const auto &g : groups) {
            for (size_t i = 0; i < compatible.size(); ++i)
                if (compatible[i].empty() || compatible[i].contains(g.get<int32_t>())) {
                    selected = i;
                    break;
                }
            if (selected != passes.size())
                break;
        }
        if (groups.empty())
            selected = 0;
        if (selected == passes.size()) {
            passes.push_back(Json::array());
            compatible.push_back(allowed);
        } else if (compatible[selected].empty())
            compatible[selected] = allowed;
        else {
            std::set<int32_t> intersection;
            std::set_intersection(compatible[selected].begin(), compatible[selected].end(), allowed.begin(),
                                  allowed.end(), std::inserter(intersection, intersection.end()));
            compatible[selected] = std::move(intersection);
        }
        passes[selected].push_back(metric);
    }
    Json result = Json::array();
    for (size_t i = 0; i < passes.size(); ++i)
        result.push_back({{"metrics", passes[i]}, {"compatible_groups", compatible[i]}});
    return result;
}
Json planMetrics(const Json &catalog, const Json &requested) {
    if (requested.empty() || !unique(requested) ||
        std::any_of(requested.begin(), requested.end(),
                    [](const auto &n) { return !n.is_string() || n.template get<std::string>().empty(); }))
        fail("Choose distinct nonempty metric symbols");
    const auto &sets = catalog.at("sets");
    const auto setNames = names(sets, "name");
    if (!unique(setNames))
        fail("Ambiguous metric set names");
    Json choices = Json::array();
    for (const auto &symbol : requested) {
        Json candidates = Json::array(), signatures = Json::array();
        for (size_t i = 0; i < sets.size(); ++i) {
            const Json *definition = nullptr;
            for (const auto &d : sets[i].at("metrics"))
                if (equal(d.at("name"), symbol)) {
                    if (definition)
                        fail("Ambiguous metric symbol in set: " + symbol.get<std::string>());
                    definition = &d;
                }
            if (definition) {
                candidates.push_back(i);
                Json signature = Json::array();
                for (const auto key : {"label", "description", "unit", "result_type", "metric_type"})
                    signature.push_back(get(*definition, key));
                if (find(signatures, signature) == signatures.size())
                    signatures.push_back(signature);
            }
        }
        if (candidates.empty())
            fail("Unknown metric symbol: " + symbol.get<std::string>());
        if (signatures.size() != 1)
            fail("Inconsistent metric definitions; select an explicit metric set: " +
                 symbol.get<std::string>());
        choices.push_back(candidates);
    }
    Json passes = Json::array();
    for (const auto &g : groupMetricChoices(choices)) {
        const auto &candidates = g.at("compatible_groups");
        if (candidates.empty())
            fail("No concrete MD set for planned pass");
        Json metrics = Json::array(), compatibleSets = Json::array();
        for (const auto &m : g.at("metrics"))
            metrics.push_back(requested.at(m.get<size_t>()));
        for (const auto &c : candidates)
            compatibleSets.push_back(setNames.at(c.get<size_t>()));
        passes.push_back({{"pass_index", passes.size()},
                          {"set", compatibleSets[0]},
                          {"metrics", metrics},
                          {"compatible_sets", compatibleSets}});
    }
    return {{"schema_version", 1},
            {"algorithm", "gpa_ordered_compatibility"},
            {"requested_metrics", requested},
            {"passes", passes},
            {"semantics", "Request order then candidate order; choose first catalog set from final "
                          "compatible choices; not a global minimum-pass optimizer"},
            {"limits",
             {"Compatibility inferred from actual MD set membership; not a recovered GPA global descriptor "
              "catalog.",
              "Matching public metadata does not prove identical hidden driver formulas across sets.",
              "GPA plugin planning flags, special pass-zero behavior and complete hotspot scheduling remain "
              "unverified."}}};
}
Json requestedMetricResults(const Json &profile, const Json &plan) {
    const auto matrix = metricProfileMatrix(profile);
    const auto &sets = profile.at("sets");
    if (!equal(names(plan.at("passes"), "set"), names(sets, "name")))
        fail("Collected sets differ from metric plan");
    std::map<std::string, Json> assignments;
    size_t count = 0;
    for (const auto &p : plan.at("passes"))
        for (const auto &m : p.at("metrics")) {
            assignments[m.get<std::string>()] = p.at("set");
            ++count;
        }
    std::set<std::string> requested;
    for (const auto &m : plan.at("requested_metrics"))
        requested.insert(m.get<std::string>());
    if (assignments.size() != count || assignments.size() != requested.size() ||
        std::any_of(requested.begin(), requested.end(),
                    [&](const auto &s) { return !assignments.contains(s); }))
        fail("Metric request assignments are incomplete or duplicated");
    const auto summaries = summarizeMetricMatrix(matrix);
    Json samples = Json::array(), statistics = Json::array();
    for (const auto &symbol : plan.at("requested_metrics")) {
        const auto name = assignments.at(symbol.get<std::string>());
        const auto &definition =
            *std::find_if(sets.begin(), sets.end(), [&](const auto &s) { return equal(s.at("name"), name); });
        const auto &metrics = definition.at("metrics");
        const auto index = find(names(metrics, "name"), symbol);
        if (index == metrics.size())
            fail("Planned metric absent from source set: " + symbol.get<std::string>());
        const auto &d = metrics[index];
        std::vector<const Json *> records;
        for (const auto &r : profile.at("records"))
            if (equal(r.at("set"), name))
                records.push_back(&r);
        std::stable_sort(records.begin(), records.end(), [](const Json *a, const Json *b) {
            if (a->at("sample_index") != b->at("sample_index"))
                return compareIntegers(a->at("sample_index"), b->at("sample_index")) < 0;
            const auto &ae = a->at("event"), &be = b->at("event");
            const auto &av = ae.is_null() ? a->at("start_event") : ae;
            const auto &bv = be.is_null() ? b->at("start_event") : be;
            // Frame-range rows may carry integral floats accepted by the roster.
            return lessNumber(av, bv);
        });
        for (const auto *ptr : records) {
            const auto &row = *ptr, &value = row.at("values")[index], &v = value.at("value");
            const bool available = row.at("available").get<bool>() && (v.is_boolean() || finiteNumber(v));
            samples.push_back({{"metric", symbol},
                               {"label", d.at("label")},
                               {"unit", d.at("unit")},
                               {"set", name},
                               {"event", row.at("event")},
                               {"start_event", get(row, "start_event")},
                               {"end_event", get(row, "end_event")},
                               {"pass_index", row.at("pass_index")},
                               {"sample_index", row.at("sample_index")},
                               {"value", available ? v : Json()},
                               {"available", available},
                               {"value_type", value.at("type")}});
        }
        for (const auto &s : summaries)
            if (equal(s.at("set"), name) && equal(s.at("metric"), symbol))
                statistics.push_back(s);
    }
    return {{"schema_version", 1},
            {"plan", plan},
            {"frame_sha256", profile.at("frame_sha256")},
            {"experiment", profile.at("experiment")},
            {"baseline_rgba_sha256", profile.at("baseline_rgba_sha256")},
            {"source_profile", "profile.json"},
            {"sample_count", profile.at("sample_count")},
            {"samples", samples},
            {"statistics", statistics},
            {"semantics", "Each requested symbol uses its assigned source set exactly once per range and "
                          "iteration; different passes are not simultaneous"}};
}
Json publisherMetricProfile(const Json &profile, const Json &publisher) {
    auto result = profile;
    for (auto &group : result.at("sets"))
        for (auto &definition : group.at("metrics"))
            if (definition.at("name") == "GpuTime")
                definition["unit"] = "us";
    auto &rows = result.at("records");
    const auto &convertedRows = publisher.at("records");
    for (size_t i = 0; i < std::min(rows.size(), convertedRows.size()); ++i) {
        auto &raw = rows[i];
        const auto &converted = convertedRows[i];
        for (const auto key :
             {"set", "event", "start_event", "end_event", "pass_index", "sample_index", "raw_sha256"})
            if (!equal(get(raw, key), get(converted, key)))
                fail("Converted statistics source identity mismatch");
        Json values = Json::array();
        for (const auto &v : converted.at("values"))
            values.push_back({{"type", 2}, {"value", v.at("value")}});
        raw["values"] = std::move(values);
    }
    if (convertedRows.size() < rows.size())
        fail("zip() argument 2 is shorter than argument 1");
    if (convertedRows.size() > rows.size())
        fail("zip() argument 2 is longer than argument 1");
    return result;
}
Json publisherMetricAnalysis(const Json &profile, const Json &publisher) {
    const auto projected = publisherMetricProfile(profile, publisher);
    const auto matrix = metricProfileMatrix(projected);
    Json result = {{"iterations", matrix},
                   {"statistics", summarizeMetricMatrix(matrix)},
                   {"value_semantics", "publisher_binary64"}};
    if (profile.contains("metric_request")) {
        auto requested = requestedMetricResults(projected, profile.at("metric_request"));
        requested.update(
            {{"source_profile", "publisher-values.json"}, {"value_semantics", "publisher_binary64"}});
        for (auto &sample : requested.at("samples"))
            sample["value_type"] = "binary64";
        result["requested"] = std::move(requested);
    }
    return result;
}
} // namespace flora
