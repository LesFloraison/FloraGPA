#include "GpuProfile.h"
#include "ExperimentReport.h"
#include "GpuStatistics.h"
#include <QDir>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>
#include <thread>
namespace flora {
using Json = nlohmann::json;
namespace {
const std::set<uint16_t> writes{0x31, 0x32, 0x33, 0x34, 0x3e, 0x3f, 0x40, 0x42, 0x245, 0x246, 0x247};
void save(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save GPU profile");
}
void wait(ID3D11DeviceContext *context, ID3D11Query *query, void *data, UINT size,
          std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto hr = context->GetData(query, data, size, 0);
        check(hr, "Read profile query");
        if (hr == S_OK)
            return;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("GPU profile query did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
Com<ID3D11Query> query(ID3D11Device *device, D3D11_QUERY kind) {
    Com<ID3D11Query> result;
    const D3D11_QUERY_DESC desc{kind, 0};
    check(device->CreateQuery(&desc, &result), "Create profile query");
    return result;
}
void complete(ID3D11Device *device, ID3D11DeviceContext *context) {
    auto done = query(device, D3D11_QUERY_EVENT);
    context->End(done.Get());
    context->Flush();
    BOOL value{};
    wait(context, done.Get(), &value, sizeof value,
         std::chrono::steady_clock::now() + std::chrono::seconds(10));
}
class Batch {
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11Query> disjoint_, first_, last_;
    std::map<Id, std::pair<Com<ID3D11Query>, Com<ID3D11Query>>> pairs_;
    std::map<Id, unsigned> seen_;
    bool active_{};

  public:
    Batch(ID3D11DeviceContext *context, const std::vector<Id> &events) : context_(context) {
        context_->GetDevice(&device_);
        disjoint_ = query(device_.Get(), D3D11_QUERY_TIMESTAMP_DISJOINT);
        first_ = query(device_.Get(), D3D11_QUERY_TIMESTAMP);
        last_ = query(device_.Get(), D3D11_QUERY_TIMESTAMP);
        for (auto id : events)
            pairs_.emplace(id, std::pair{query(device_.Get(), D3D11_QUERY_TIMESTAMP),
                                         query(device_.Get(), D3D11_QUERY_TIMESTAMP)});
    }
    ~Batch() { end(); }
    void begin() {
        if (active_)
            throw std::runtime_error("Profile interval already active");
        seen_.clear();
        context_->Begin(disjoint_.Get());
        active_ = true;
        context_->End(first_.Get());
    }
    void mark(Id id, bool after) {
        if (!active_ || seen_[id] != unsigned(after))
            throw std::runtime_error("Profile boundary duplicated or reversed");
        const auto &pair = pairs_.at(id);
        context_->End(after ? pair.second.Get() : pair.first.Get());
        ++seen_[id];
    }
    void end() {
        if (!active_)
            return;
        context_->End(last_.Get());
        context_->End(disjoint_.Get());
        active_ = false;
    }
    Json result() {
        if (active_ || seen_.size() != pairs_.size())
            throw std::runtime_error("Profile is missing work boundaries");
        for (const auto &[id, pair] : pairs_)
            if (seen_[id] != 2)
                throw std::runtime_error("Profile is missing work boundaries");
        complete(device_.Get(), context_.Get());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        wait(context_.Get(), disjoint_.Get(), &frequency, sizeof frequency, deadline);
        auto tick = [&](ID3D11Query *q) {
            uint64_t v{};
            wait(context_.Get(), q, &v, sizeof v, deadline);
            return v;
        };
        const auto first = tick(first_.Get()), last = tick(last_.Get());
        Json rows = Json::object();
        for (const auto &[id, pair] : pairs_)
            rows[std::to_string(id)] =
                profileTiming(frequency.Frequency, frequency.Disjoint != FALSE, tick(pair.first.Get()),
                              tick(pair.second.Get()), first, last);
        return {{"frequency_hz", frequency.Frequency},
                {"disjoint", frequency.Disjoint != FALSE},
                {"envelope",
                 profileTiming(frequency.Frequency, frequency.Disjoint != FALSE, first, last, first, last)},
                {"events", rows}};
    }
};
uint64_t integer(const Json &v, uint64_t low, uint64_t high, const char *label) {
    if ((!v.is_number_unsigned() && !v.is_number_integer()) ||
        (v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0))
        throw std::runtime_error(std::string(label) + " is out of range");
    auto n = v.get<uint64_t>();
    if (n < low || n > high)
        throw std::runtime_error(std::string(label) + " is out of range");
    return n;
}
std::string csvValue(const Json &value) {
    if (value.is_null())
        return {};
    auto text = value.is_boolean()  ? (value.get<bool>() ? std::string("True") : std::string("False"))
                : value.is_string() ? value.get<std::string>()
                                    : value.dump();
    if (text.find_first_of(",\"\r\n") == std::string::npos)
        return text;
    std::string escaped = "\"";
    for (char c : text) {
        if (c == '"')
            escaped += '"';
        escaped += c;
    }
    return escaped + '"';
}
} // namespace
GpuProfileRequest gpuProfileRequest(const Json &value) {
    if (!value.is_object())
        throw std::runtime_error("Invalid GPU profile request");
    GpuProfileRequest r;
    if (value.contains("start") && !value["start"].is_null())
        r.start = integer(value["start"], 0, UINT64_MAX, "Start event");
    if (value.contains("end") && !value["end"].is_null())
        r.end = integer(value["end"], 0, UINT64_MAX, "End event");
    r.samples = unsigned(integer(value.value("samples", Json(5)), 1, 1000, "Sample count"));
    r.warmup = unsigned(integer(value.value("warmup", Json(1)), 0, 1000, "Warmup count"));
    auto includeWrites = value.value("include_writes", Json(false));
    if (!includeWrites.is_boolean())
        throw std::runtime_error("include_writes must be boolean");
    r.includeWrites = includeWrites.get<bool>();
    return r;
}
GpuProfileSelection gpuProfileSelection(const Frame &frame, const GpuProfileRequest &request) {
    if (!request.samples || request.samples > 1000 || request.warmup > 1000)
        throw std::runtime_error("Profile sample/warmup count out of range");
    std::vector<Id> commands;
    for (const auto &[id, e] : frame.entries())
        if (e.category == 7)
            commands.push_back(id);
    if (commands.empty())
        throw std::runtime_error("Capture has no API commands");
    GpuProfileSelection result{request.start.value_or(commands.front()),
                               request.end.value_or(commands.back())};
    for (auto id : {result.start, result.end})
        if (!frame.entries().contains(id) || frame.entry(id).category != 7)
            throw std::runtime_error("Profile endpoints must be API command IDs");
    if (result.start > result.end)
        throw std::runtime_error("Profile start must not exceed end");
    for (auto id : commands)
        if (id >= result.start && id <= result.end &&
            (isDraw(frame.entry(id).type) ||
             (request.includeWrites && writes.contains(frame.entry(id).type))))
            result.events.push_back(id);
    if (result.events.empty())
        throw std::runtime_error(
            "Range has no selected GPU work; include resource-writing commands for a no-Draw range");
    return result;
}
Json timingDistribution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    auto quantile = [&](double q) -> Json {
        if (!n)
            return nullptr;
        double i = double(n - 1) * q;
        auto lo = size_t(std::floor(i)), hi = size_t(std::ceil(i));
        return values[lo] + (values[hi] - values[lo]) * (i - double(lo));
    };
    double mean = 0, correction = 0;
    for (double v : values) {
        const auto sum = mean + v;
        correction += (std::abs(mean) >= std::abs(v)) ? (mean - sum) + v : (v - sum) + mean;
        mean = sum;
    }
    if (n)
        mean = (mean + correction) / double(n);
    double squares = 0, offset = 0;
    for (double v : values) {
        auto d = v - mean;
        squares += d * d;
        offset += d;
    }
    return {{"valid_samples", n},
            {"min_ms", n ? Json(values.front()) : Json(nullptr)},
            {"max_ms", n ? Json(values.back()) : Json(nullptr)},
            {"mean_ms", n ? Json(mean) : Json(nullptr)},
            {"median_ms", quantile(.5)},
            {"p05_ms", quantile(.05)},
            {"p95_ms", quantile(.95)},
            {"sample_stddev_ms",
             n > 1 ? Json(std::sqrt(std::max(0.0, (squares - offset * offset / double(n)) / double(n - 1))))
                   : Json(nullptr)}};
}
Json profileTiming(uint64_t frequency, bool disjoint, uint64_t start, uint64_t end, uint64_t origin,
                   uint64_t limit) {
    const char *reason = disjoint                                             ? "disjoint"
                         : !frequency                                         ? "invalid_frequency"
                         : !(origin <= start && start <= end && end <= limit) ? "nonmonotonic_ticks"
                                                                              : nullptr;
    return {
        {"start_tick", start},
        {"end_tick", end},
        {"available", !reason},
        {"unavailable_reason", reason ? Json(reason) : Json(nullptr)},
        {"elapsed_ms", reason ? Json(nullptr) : Json(double(end - start) * 1000.0 / double(frequency))},
        {"offset_ms", reason ? Json(nullptr) : Json(double(start - origin) * 1000.0 / double(frequency))}};
}
void exportGpuProfile(const Json &report, const QString &directory) {
    QDir dir(directory);
    if (!dir.mkpath("."))
        throw std::runtime_error("Cannot create GPU profile directory");
    save(dir.filePath("profile.json"), QByteArray::fromStdString(report.dump(2) + '\n'));
    const std::vector<std::string> columns{
        "event",  "api",       "enabled", "boundary", "rank",   "valid_samples", "invalid_samples",
        "min_ms", "median_ms", "mean_ms", "p05_ms",   "p95_ms", "max_ms",        "sample_stddev_ms"};
    std::string csv = "\xef\xbb\xbf";
    for (size_t n = 0; n < columns.size(); ++n)
        csv += (n ? "," : "") + columns[n];
    csv += "\r\n";
    for (const auto &row : report.at("events")) {
        for (size_t n = 0; n < columns.size(); ++n)
            csv += (n ? "," : "") + csvValue(row.at(columns[n]));
        csv += "\r\n";
    }
    save(dir.filePath("profile.csv"), QByteArray::fromStdString(csv));
    csv =
        "\xef\xbb\xbf"
        "sample,event,frequency_hz,disjoint,start_tick,end_tick,elapsed_ms,offset_ms,unavailable_reason\r\n";
    for (const auto &pass : report.at("passes"))
        for (const auto &row : report.at("events")) {
            const auto id = row.at("event").get<Id>();
            const auto &v = pass.at("events").at(std::to_string(id));
            std::vector<Json> values{pass.at("index"),          id,
                                     pass.at("frequency_hz"),   pass.at("disjoint"),
                                     v.at("start_tick"),        v.at("end_tick"),
                                     v.at("elapsed_ms"),        v.at("offset_ms"),
                                     v.at("unavailable_reason")};
            for (size_t n = 0; n < values.size(); ++n)
                csv += (n ? "," : "") + csvValue(values[n]);
            csv += "\r\n";
        }
    save(dir.filePath("profile-samples.csv"), QByteArray::fromStdString(csv));
}
Json profileGpu(Replay &replay, const Frame &capture, const GpuProfileRequest &request,
                const QString &directory) {
    const auto selected = gpuProfileSelection(replay.frame(), request);
    if (replay.options().until != selected.end || replay.options().before || replay.options().timings ||
        replay.options().measurement)
        throw std::runtime_error("GPU profile requires its own complete replay boundary");
    QDir dir(directory);
    if (!dir.mkpath("."))
        throw std::runtime_error("Cannot create GPU profile directory");
    const auto began = std::chrono::steady_clock::now();
    auto progress = [&](const char *phase, unsigned done) {
        save(dir.filePath("profile-progress.json"), QByteArray::fromStdString(Json{
                                                        {"phase", phase},
                                                        {"completed", done},
                                                        {"samples", request.samples},
                                                        {"warmup", request.warmup},
                                                        {"events",
                                                         selected.events.size()}}.dump(2)));
    };
    Com<ID3D11DeviceContext> context;
    Com<ID3D11Device> device;
    progress("warmup", 0);
    for (unsigned i = 0; i < request.warmup; ++i) {
        replay.run({}, {}, [&](Id, bool, auto *ctx, const auto &) {
            if (!context) {
                context = ctx;
                ctx->GetDevice(&device);
            }
        });
        complete(device.Get(), context.Get());
        progress("warmup", i + 1);
    }
    std::unique_ptr<Batch> batch;
    std::set<Id> ids(selected.events.begin(), selected.events.end());
    Json passes = Json::array();
    for (unsigned i = 0; i < request.samples; ++i) {
        replay.run(
            {},
            [&](Id id, bool after, auto *, const auto &) {
                if (ids.contains(id))
                    batch->mark(id, after);
            },
            [&](Id id, bool after, auto *ctx, const auto &) {
                if (!batch)
                    batch = std::make_unique<Batch>(ctx, selected.events);
                if (!after && id == selected.start)
                    batch->begin();
                if (ids.contains(id) && !isDraw(replay.frame().entry(id).type))
                    batch->mark(id, after);
                if (after && id == selected.end)
                    batch->end();
            });
        auto pass = batch->result();
        pass.update({{"index", i},
                     {"replay_generation", replay.generation()},
                     {"replay_counts", compatibleReplayCounts(replay, 0, selected.end, replay.counts)}});
        passes.push_back(std::move(pass));
        progress("sampling", i + 1);
    }
    Json rows = Json::array();
    std::vector<double> envelopes;
    for (auto id : selected.events) {
        std::vector<double> values;
        for (const auto &p : passes) {
            const auto &v = p.at("events").at(std::to_string(id));
            if (v.at("available").get<bool>())
                values.push_back(v.at("elapsed_ms").get<double>());
        }
        auto row = timingDistribution(values);
        row.update({{"event", id},
                    {"api", commandName(replay.frame().entry(id).type)},
                    {"enabled", !replay.options().disabled.contains(id)},
                    {"boundary", isDraw(replay.frame().entry(id).type) ? "submission_after_binding"
                                                                       : "complete_replay_command"},
                    {"invalid_samples", request.samples - values.size()},
                    {"rank", nullptr}});
        rows.push_back(std::move(row));
    }
    std::vector<size_t> ranked;
    for (size_t n = 0; n < rows.size(); ++n)
        if (!rows[n]["median_ms"].is_null())
            ranked.push_back(n);
    std::stable_sort(ranked.begin(), ranked.end(), [&](auto a, auto b) {
        return rows[a]["median_ms"].get<double>() > rows[b]["median_ms"].get<double>();
    });
    for (size_t n = 0; n < ranked.size(); ++n)
        rows[ranked[n]]["rank"] = n + 1;
    for (const auto &p : passes)
        if (p["envelope"]["available"].get<bool>())
            envelopes.push_back(p["envelope"]["elapsed_ms"].get<double>());
    const auto desc = replay.adapterDescription();
    std::ostringstream level;
    level << "0x" << std::hex << unsigned(replay.featureLevel());
    const bool warp = replay.options().warp;
    Json report{
        {"source", "native_dx11_queries"},
        {"single_replay_sample", false},
        {"statistics_shader_instrumentation", false},
        {"replay_generation", replay.generation()},
        {"query_completion", "native_event_query"},
        {"execution_device",
         {{"selected", warp ? "warp" : "hardware"},
          {"software", warp},
          {"measurement_scope", warp ? "software_device" : "hardware_device"},
          {"feature_level", level.str()},
          {"adapter",
           {{"description", replay.adapter()},
            {"vendor_id", desc.VendorId},
            {"device_id", desc.DeviceId},
            {"revision", desc.Revision},
            {"dedicated_video_memory", desc.DedicatedVideoMemory},
            {"shared_system_memory", desc.SharedSystemMemory}}}}},
        {"so_count_reconstruction", replay.reconstructsSoCounts()},
        {"experiment", experimentReport(replay)},
        {"scope", "repeated_work_profile"},
        {"frame_sha256", capture.sha256()},
        {"range", {{"start", selected.start}, {"end", selected.end}, {"inclusive", true}}},
        {"requested_samples", request.samples},
        {"warmup_replays", request.warmup},
        {"include_writes", request.includeWrites},
        {"events", rows},
        {"passes", passes},
        {"envelope", timingDistribution(envelopes)},
        {"wall_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count()},
        {"quantile_method", "linear_interpolation_(n-1)*q"}};
    report["notes"] = {
        "Timing describes the selected replay device; WARP is software execution. No original application "
        "performance is inferred.",
        "Every sample restores captured storage and replays the prefix in original order. Warmup does not "
        "preserve warmed resource contents.",
        "Draw/Dispatch timestamps bracket submission after binding; SO reconstruction and predication may "
        "affect measurements. Disabled work still measures boundary overhead, not an invented zero.",
        "Enabled identifies the experiment toggle; it does not prove that predication, rasterization or "
        "shader tests allowed GPU output writes.",
        "Resource-writing rows and the enclosing interval include replay preparation and CPU submission "
        "gaps. Rows and medians must not be summed as original application GPU time.",
        "Timing-only timestamp instrumentation does not collect pipeline or vendor performance counters. "
        "Quantiles describe observed samples, not confidence intervals or a stable benchmark."};
    exportGpuProfile(report, directory);
    progress("complete", request.samples);
    return report;
}
} // namespace flora
