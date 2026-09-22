#include "MdIterationResults.h"
#include "MdIterationTransport.h"
#include "MetricPriority.h"
#include "core/Frame.h"
#include <QDir>
#include <QFile>
#include <cmath>
#include <limits>

namespace flora {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }
bool truth(const Json &value) {
    if (value.is_null())
        return false;
    if (value.is_boolean())
        return value.get<bool>();
    if (value.is_number())
        return value != 0;
    return !value.empty();
}
Json finite(Json value) {
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        return nullptr;
    if (value.is_structured())
        for (auto &item : value)
            item = finite(std::move(item));
    return value;
}
QByteArray bytes(const QDir &folder, const QString &name) {
    QFile file(folder.filePath(name));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read scheduled result: " + file.fileName().toStdString());
    return file.readAll();
}
Json read(const QDir &folder, const QString &name) { return Json::parse(bytes(folder, name).toStdString()); }
std::string digest(const QByteArray &data) {
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size())));
}
bool matches(const Json &object, const char *key, const Json &value) {
    return metricIdentityEqual(object.value(key, Json()), value);
}
size_t arrayIndex(const Json &value) {
    if (value.is_boolean())
        return value.get<bool>(); // Python sequence indices accept bool.
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        fail("Invalid scheduled report index");
    return value.get<size_t>();
}
class RecordedTransport final : public MetricIterationTransport {
  public:
    RecordedTransport(const QDir &folder, const Json &profile, const Json &catalog, const Json &descriptions,
                      const Json &publisher, const Json &mapped)
        : folder_(folder), profile_(profile), catalog_(catalog), descriptions_(descriptions),
          publisher_(publisher), mapped_(mapped) {}
    Json trace = Json::array();
    size_t serial{}, consumed{};
    Json descriptions() override {
        trace.push_back({{"operation", "catalog"}});
        return descriptions_;
    }
    Json prepare(const Json &requested) override {
        Json positions = Json::array(), choices = Json::array();
        for (const auto &id : requested) {
            size_t i = 0;
            while (i < descriptions_.size() && !metricIdentityEqual(descriptions_[i].at("id"), id))
                ++i;
            if (i == descriptions_.size())
                fail("Unknown scheduled metric identity");
            positions.push_back(i);
            choices.push_back(descriptions_[i].at("compatible_sets"));
        }
        groups_ = Json::array();
        for (const auto &group : groupMetricChoices(choices)) {
            Json members = Json::array();
            for (const auto &i : group.at("metrics"))
                members.push_back(positions.at(i.get<size_t>()));
            groups_.push_back(std::move(members));
        }
        trace.push_back({{"operation", "prepare"}, {"ids", requested}, {"passes", groups_}, {"flag", false}});
        return {{"groups", groups_}, {"flag", false}};
    }
    Json replay(uint32_t index, const Json &requestedRanges, bool flag) override {
        const auto &replays = profile_.at("replays"), &records = profile_.at("records"),
                   &ranges = profile_.at("selection").at("ranges");
        if (flag || !metricIdentityEqual(requestedRanges, mapped_) || serial >= replays.size())
            fail("Unexpected scheduled replay");
        const auto &replay = replays.at(serial), &group = groups_.at(index);
        Json handles = Json::array(), choices = Json::array();
        for (const auto &i : group) {
            const auto &d = descriptions_.at(i.get<size_t>());
            handles.push_back(d.at("id"));
            choices.push_back(d.at("compatible_sets"));
        }
        const auto concrete = groupMetricChoices(choices).at(0).at("compatible_groups").at(0).get<size_t>();
        const auto &source = catalog_.at("sets").at(concrete).at("name");
        const auto &span = replay.at("record_span");
        if (!matches(replay, "replay_index", serial) || !matches(replay, "selected_pass", index) ||
            !matches(replay, "set", source) || !matches(replay, "metrics", handles))
            fail("Scheduled pass identity mismatch");
        if (!metricIdentityEqual(span, Json::array({consumed, consumed + ranges.size()})) ||
            consumed + ranges.size() > records.size())
            fail("Scheduled report span mismatch");
        const auto a = arrayIndex(span.at(0)), b = arrayIndex(span.at(1));
        if (replay.at("image_matches") != Json(true) || replay.at("range_mapping_valid") != Json(true) ||
            !metricIdentityEqual(replay.at("rgba_sha256"), profile_.at("baseline_rgba_sha256")))
            fail("Unaccepted scheduled image or range mapping");
        Json wanted = Json::array();
        for (const auto &r : ranges) {
            Json seen = Json::array();
            for (const auto &c : r.at("commands"))
                seen.push_back(c.at("event"));
            wanted.push_back({{"range_index", r.at("range_index")},
                              {"start_event", r.at("start_event")},
                              {"end_event", r.at("end_event")},
                              {"commands_seen", seen}});
        }
        if (!metricIdentityEqual(replay.at("boundaries"), wanted))
            fail("Scheduled command boundary mismatch");
        const auto name = QString("replay-%1").arg(serial, 3, 10, QLatin1Char('0'));
        const Json selected(records.begin() + ptrdiff_t(a), records.begin() + ptrdiff_t(b));
        if (!metricIdentityEqual(read(folder_, name + "/raw-results.json"),
                                 {{"validation", replay}, {"records", selected}}))
            fail("Per-replay source mismatch");
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto &raw = selected.at(i), &info = ranges.at(i);
            for (const auto key : {"range_index", "start_event", "end_event", "pass_index", "selected_pass"})
                if (!raw.at(key).is_number_integer())
                    fail("Scheduled raw report identity mismatch");
            if (!matches(raw, "range_index", info.at("range_index")) ||
                !matches(raw, "start_event", info.at("start_event")) ||
                !matches(raw, "end_event", info.at("end_event")) || !matches(raw, "set", source) ||
                !matches(raw, "pass_index", serial) || !matches(raw, "selected_pass", index))
                fail("Scheduled raw report identity mismatch");
            const auto path =
                name + "/range-" + QString::fromStdString(info.at("range_index").dump()) + ".bin";
            if (!matches(raw, "raw_report", path.toStdString()) ||
                !matches(raw, "raw_sha256", digest(bytes(folder_, path))))
                fail("Scheduled raw bytes mismatch");
        }
        Json incoming = Json::array();
        for (const auto &i : group) {
            const auto &d = descriptions_.at(i.get<size_t>());
            Json values = Json::array(), aux = Json::array();
            for (size_t j = a; j < b; ++j) {
                double number = std::numeric_limits<double>::quiet_NaN();
                if (truth(records.at(j).at("available"))) {
                    bool found = false;
                    for (const auto &v : publisher_.at("records").at(j).at("values"))
                        if (metricIdentityEqual(v.at("name"), d.at("symbol"))) {
                            number = v.at("value").get<double>();
                            found = true;
                            break;
                        }
                    if (!found)
                        fail("Missing scheduled publisher metric");
                }
                values.push_back(number);
                if (profile_.value("auxiliary_semantics", Json("legacy_empty")) == "dx11_zero_query_flags")
                    aux.push_back(0);
            }
            incoming.push_back({{"metric", d.at("id")}, {"values", values}, {"aux", aux}});
        }
        ++serial;
        consumed = b;
        trace.push_back({{"operation", "replay"}, {"pass_index", index}, {"ranges", requestedRanges}});
        return {{"metrics", incoming}, {"parallel_ranges", Json::array()}, {"flag", false}};
    }

  private:
    QDir folder_;
    const Json &profile_, &catalog_, &descriptions_, &publisher_, &mapped_;
    Json groups_ = Json::array();
};
} // namespace

Json loadScheduledMetricResult(const QString &directory, const Json &profile) {
    const QDir folder(QDir(directory).absolutePath());
    if (!matches(profile, "mode", "recovered_metric_iterations") || !matches(profile, "schema_version", 1))
        fail("Unknown scheduled profile schema");
    if (!matches(profile, "value_semantics", "publisher_binary64"))
        fail("Unexpected scheduled value semantics");
    const auto auxiliary = profile.value("auxiliary_semantics", Json("legacy_empty"));
    if (auxiliary != "legacy_empty" && auxiliary != "dx11_zero_query_flags")
        fail("Unknown scheduled auxiliary semantics");
    for (const auto key :
         {"schema_version", "requested_samples", "warmup_count", "requested_pass", "actual_iteration_count"})
        if (!profile.value(key, Json()).is_number_integer())
            fail("Invalid scheduled integer fields");
    if (profile.value("production_gpa_dependency", Json()) != Json(false))
        fail("Unexpected GPA runtime dependency");
    for (const auto &m : profile.at("loaded_modules"))
        if (QString::fromStdString(m.is_string() ? m.get<std::string>() : m.dump())
                .toLower()
                .contains("intelswtools"))
            fail("Unexpected GPA runtime dependency");
    const auto &protocol = profile.at("protocol");
    if (!truth(protocol.at("complete")))
        fail("Incomplete scheduled acquisition");
    if (auxiliary == "dx11_zero_query_flags")
        for (const auto &iteration : protocol.at("iterations"))
            for (const auto &row : iteration.at("metrics")) {
                if (row.at("aux").size() != row.at("values").size())
                    fail("Invalid DX11 zero query flags");
                for (const auto &v : row.at("aux"))
                    if (!v.is_number_integer() || v != 0)
                        fail("Invalid DX11 zero query flags");
            }
    const auto catalog = read(folder, "catalog.json");
    const auto described = mdIterationDescriptors(catalog, profile.at("requested_metrics"));
    const auto &descriptions = described.at("catalog");
    if (!metricIdentityEqual(profile.at("descriptors"), descriptions) ||
        !metricIdentityEqual(profile.at("plan"), described.at("plan")))
        fail("Scheduled descriptor or plan identity mismatch");
    if (!metricIdentityEqual(read(folder, "frame-ranges.json"), profile.at("selection")))
        fail("Frame range sidecar mismatch");
    if (!metricIdentityEqual(read(folder, "raw-records.json"), profile.at("records")))
        fail("Raw record sidecar mismatch");
    const auto publisher = loadMetricPublisherResult(folder.absolutePath(), profile);
    const auto &ranges = profile.at("selection").at("ranges");
    Json ids = Json::array(), mapped = Json::array(), roster = Json::array();
    for (size_t i = 0; i < profile.at("requested_metrics").size(); ++i)
        ids.push_back(descriptions.at(i).at("id"));
    if (ranges.empty())
        fail("Invalid selected range roster");
    for (const auto &r : ranges) {
        for (const auto &previous : roster)
            if (metricIdentityEqual(previous, r.at("range_index")))
                fail("Invalid selected range roster");
        roster.push_back(r.at("range_index"));
    }
    for (const auto &r : ranges) {
        for (const auto key : {"range_index", "start_event", "end_event"})
            if (!r.at(key).is_number_integer())
                fail("Invalid selected range identities");
        mapped.push_back({r.at("start_event"), r.at("end_event")});
    }
    RecordedTransport transport(folder, profile, catalog, descriptions, publisher, mapped);
    const auto result =
        MetricIterationRunner(transport).execute(ids, mapped,
                                                 {{"samples", profile.at("requested_samples")},
                                                  {"requested_pass", profile.at("requested_pass")},
                                                  {"pass_mapping", profile.at("pass_mapping")},
                                                  {"weights", profile.at("supplied_weights")}});
    const auto actual = finite(result);
    for (auto it = actual.begin(); it != actual.end(); ++it)
        if (!metricIdentityEqual(protocol.value(it.key(), Json()), it.value()))
            fail("Scheduled numeric protocol differs from source samples");
    if (!matches(protocol, "replay_ranges", mapped))
        fail("Scheduled numeric protocol differs from source samples");
    if (transport.serial != profile.at("replays").size() ||
        transport.consumed != profile.at("records").size())
        fail("Unused scheduled reports");
    if (!metricIdentityEqual(profile.at("actual_iteration_count"), result.at("iteration_count")))
        fail("Scheduled iteration count mismatch");
    Json cells = Json::array();
    if (actual.at("values").size() != ranges.size())
        fail("Scheduled display matrix differs from source samples");
    for (size_t r = 0; r < ranges.size(); ++r) {
        const auto &info = ranges[r], &row = actual.at("values").at(r);
        if (row.size() != ids.size())
            fail("Scheduled display matrix differs from source samples");
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto &d = descriptions.at(i), &v = row.at(i), &definition = d.at("definition");
            const bool measured = !v.at("values").empty();
            auto cell = metricSampleSummary(v.at("values"));
            cell.update({{"range_index", info.at("range_index")},
                         {"start_event", info.at("start_event")},
                         {"end_event", info.at("end_event")},
                         {"metric", d.at("symbol")},
                         {"metric_id", ids[i]},
                         {"label", definition.at("label")},
                         {"unit", d.at("symbol") == "GpuTime" ? Json("us") : definition.at("unit")},
                         {"measured", measured},
                         {"values", v.at("values")},
                         {"kind", measured ? v.at("kind") : d.at("kind")},
                         {"weight", measured ? v.at("weight") : Json()}});
            cells.push_back(std::move(cell));
        }
    }
    if (!metricIdentityEqual(profile.at("metrics"), cells))
        fail("Scheduled display matrix differs from source samples");
    const auto audit = read(folder, "scheduler-audit.json");
    const auto &adapter = audit.at("adapter");
    if (!metricIdentityEqual(audit.at("protocol"), protocol) || !audit.at("failure").is_null() ||
        !metricIdentityEqual(adapter.at("trace"), transport.trace))
        fail("Scheduled audit protocol mismatch");
    if (!profile.value("arbitration", Json()).is_null()) {
        if (profile.at("arbitration") != "gpa_priority_v2")
            fail("Unknown scheduled arbitration");
        const auto lock = adapter.value("priority_mutex", Json::object());
        const auto &low = catalog.at("luid").at(0), &high = catalog.at("luid").at(1);
        if (!low.is_number_integer() || (!low.is_number_unsigned() && low.get<int64_t>() < 0) ||
            low.get<uint64_t>() > UINT32_MAX)
            fail("Expected uint32");
        const auto bits = high.is_number_unsigned() ? high.get<uint64_t>() : uint64_t(high.get<int64_t>());
        Json priorities = Json::array();
        for (size_t i = 0; i < profile.at("replays").size(); ++i) {
            priorities.push_back(7);
            priorities.push_back(priorityEmpty);
        }
        const auto attempts = lock.value("attempts", Json());
        if (adapter.value("cross_process_priority_mutex", Json()) != Json(true) ||
            lock.value("cross_process", Json()) != Json(true) ||
            !matches(lock, "resource", metricResourceName(1, low.get<uint32_t>(), uint32_t(bits), "OA")) ||
            lock.value("closed", Json()) != Json(true) || !matches(lock, "depth", 0) ||
            !matches(lock, "priority", priorityEmpty) || !matches(lock, "timeouts", 0) ||
            !matches(lock, "priorities", priorities) || !attempts.is_number_integer() ||
            (!attempts.is_number_unsigned() && attempts.get<int64_t>() < 0) ||
            attempts.get<uint64_t>() < profile.at("replays").size() ||
            lock.value("production_gpa_dependency", Json()) != Json(false))
            fail("Scheduled priority lock audit mismatch");
    }
    if (truth(adapter.at("owned")) || truth(adapter.at("native_pool").at("owned")) ||
        truth(adapter.at("native_pool").at("cached")) || truth(adapter.at("subscriptions")) ||
        truth(adapter.at("local_lock_depth")))
        fail("Scheduled resources still owned");
    if (!profile.at("experiment").is_null() &&
        !matches(profile.at("experiment"), "sha256", digest(bytes(folder, "experiment.json"))))
        fail("Frozen experiment identity mismatch");
    return publisher;
}
} // namespace flora
