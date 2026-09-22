#include "application/MdRecordedQueries.h"
#include "application/MdIterationTransport.h"
#include "application/MdSamplePool.h"
#include "application/MdScheduledPool.h"
#include "application/MetricAcquisitionPriority.h"
#include "application/MetricClock.h"
#include "application/MetricPassController.h"
#include "application/MetricProbeRegistry.h"
#include "application/MetricQueries.h"
#include "application/MetricReport.h"
#include "application/MetricsDiscovery.h"
#include "replay/Device.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <d3dcompiler.h>
#include <functional>
#include <map>
using namespace flora;
using Json = nlohmann::json;
namespace {
struct FaultSessionSamples final : MetricSampleTransport {
    MetricsDiscovery &md;
    std::string fault;
    explicit FaultSessionSamples(MetricsDiscovery &m) : md(m) {}
    bool supportsSamples() const override { return md.supportsSamples(); }
    bool supportsReuse() const override { return md.supportsReuse(); }
    uint64_t sampleBegin() override { return md.sampleBegin(); }
    void sampleSubmit(uint64_t t) override { md.sampleSubmit(t); }
    std::optional<MetricResult> samplePoll(uint64_t t, bool flush) override {
        return md.samplePoll(t, flush);
    }
    void sampleRelease(uint64_t t) override {
        if (fault == "release" || fault == "stats")
            throw std::runtime_error("Injected release");
        md.sampleRelease(t);
    }
    void sampleReserve(unsigned n) override { md.sampleReserve(n); }
    void sampleRecycle(uint64_t t) override { md.sampleRecycle(t); }
    void sampleClearCache() override {
        if (fault == "clear")
            throw std::runtime_error("Injected cache clear");
        md.sampleClearCache();
    }
    uint64_t sampleInfo(uint64_t t) override { return md.sampleInfo(t); }
    Json sampleStats() override {
        if (fault == "stats")
            throw std::runtime_error("Injected stats");
        return md.sampleStats();
    }
};
struct FaultRecorded final : MetricRecordedTransport {
    MetricsDiscovery &md;
    std::string fault;
    explicit FaultRecorded(MetricsDiscovery &value) : md(value) {}
    void fail(const char *name) const {
        if (fault == name)
            throw std::runtime_error(std::string("Injected ") + name);
    }
    bool supportsRecorded() const override { return md.supportsRecorded(); }
    const Json &selected() const override { return md.selected(); }
    Json provenance() const override { return md.provenance(); }
    Json clockPair() override {
        auto value = md.clockPair();
        if (fault == "clock")
            value["status"] = 1;
        return value;
    }
    uint64_t recordedBegin(ID3D11DeviceContext *context) override {
        fail("begin");
        return md.recordedBegin(context);
    }
    void recordedEnd(uint64_t token) override {
        fail("end");
        md.recordedEnd(token);
    }
    uint64_t recordedExecute(ID3D11CommandList *command, std::span<const uint64_t> tokens,
                             bool restore) override {
        fail("execute");
        return md.recordedExecute(command, tokens, restore);
    }
    std::optional<MetricResult> recordedPoll(uint64_t token, uint64_t execution, bool flush) override {
        fail("poll");
        if (fault == "timeout")
            return {};
        return md.recordedPoll(token, execution, flush);
    }
    void recordedRelease(uint64_t token) override { md.recordedRelease(token); }
};
MetricResult wait(const std::function<std::optional<MetricResult>(bool)> &poll) {
    QElapsedTimer elapsed;
    elapsed.start();
    for (bool flush = true;; flush = false) {
        if (auto value = poll(flush))
            return std::move(*value);
        if (elapsed.elapsed() > 10000)
            throw std::runtime_error("Counter polling timed out");
        QThread::msleep(1);
    }
}
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read metrics fixture");
    return Json::parse(file.readAll().toStdString());
}
Json probe(const Json &jobs) {
    Json result = Json::array();
    std::optional<Dx11Device> device;
    std::unique_ptr<MetricsDiscovery> md;
    for (const auto &job : jobs)
        try {
            const auto op = job.at("op").get<std::string>();
            if (op == "kind")
                result.push_back(metricDescriptorKind(job.at("type"), job.at("unit"), job.at("symbol")));
            else if (op == "validate") {
                std::vector<uint8_t> raw(job.at("size").get<size_t>());
                result.push_back(validateMetricResult(job.at("metadata"), job.at("result"), raw));
            } else if (op == "device") {
                auto value = createDx11Device(
                    job.value("warp", false), false,
                    job.contains("vendor") ? std::optional(job.at("vendor").get<uint32_t>()) : std::nullopt);
                result.push_back(value.information());
            } else {
                if (!device) {
                    device = createDx11Device(false, false, 0x8086);
                    md = std::make_unique<MetricsDiscovery>(device->device.Get());
                }
                if (op == "catalog")
                    result.push_back(md->catalog());
                else if (op == "decode") {
                    md->select(job.at("set").get<std::string>());
                    QFile file(QString::fromStdString(job.at("path").get<std::string>()));
                    if (!file.open(QIODevice::ReadOnly))
                        throw std::runtime_error("Cannot read raw report");
                    const auto data = file.readAll();
                    auto decoded = md->decode(
                        Bytes(reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size())));
                    result.push_back(
                        {{"result", decoded.values},
                         {"raw_hex", QByteArray(reinterpret_cast<const char *>(decoded.raw.data()),
                                                qsizetype(decoded.raw.size()))
                                         .toHex()
                                         .toStdString()}});
                } else
                    throw std::runtime_error("Unknown probe operation");
            }
        } catch (const std::exception &e) {
            result.push_back({{"error", e.what()}});
        }
    return result;
}
} // namespace
class MetricsDiscoveryTests : public QObject {
    Q_OBJECT
  private slots:
    void legacyCapabilities() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
        if (root.isEmpty())
            QSKIP("Set FLORA_TEST_REFERENCE_ROOT for historical bridge compatibility");
        auto device = createDx11Device(false, false, 0x8086);
        {
            MetricsDiscovery old(device.device.Get(),
                                 root + "/output/metrics-discovery-drain-build/Release/flora_metrics.dll");
            QVERIFY(old.supportsDrain());
            QVERIFY(!old.supportsSamples() && !old.supportsReuse() && !old.supportsRecorded());
            old.select("ComputeBasic");
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, old.sampleBegin());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, old.sampleReserve(1));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, old.recordedBegin(device.context.Get()));
            old.begin();
            old.end();
        }
        {
            MetricsDiscovery old(device.device.Get(),
                                 root + "/output/metrics-discovery-reuse-build/Release/flora_metrics.dll");
            QVERIFY(old.supportsDrain() && old.supportsSamples() && old.supportsReuse());
            QVERIFY(!old.supportsRecorded());
            old.select("RenderBasic");
            old.sampleReserve(1);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, old.recordedCount());
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, createDx11Device(true, false, 0x8086));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, createDx11Device(false, false, 0xffffffff));
    }
    void valueTypes() {
        Json meta{
            {"report_size", 4},
            {"metrics",
             Json::array(
                 {{{"result_type", 0}}, {{"result_type", 1}}, {{"result_type", 3}}, {{"result_type", 2}}})},
            {"information_count", 1},
            {"information", Json::array({{{"name", "ReportLost"}}})}};
        Json value{{"reports", 1},
                   {"values", Json::array({{{"type", 0}, {"value", UINT32_MAX}},
                                           {{"type", 1}, {"value", UINT64_MAX}},
                                           {{"type", 2}, {"value", 0.5}},
                                           {{"type", 3}, {"value", true}},
                                           {{"type", 3}, {"value", false}}})}};
        std::vector<uint8_t> raw(4);
        QVERIFY(validateMetricResult(meta, value, raw)["available"] == true);
        auto bad = value;
        bad["reports"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateMetricResult(meta, bad, raw));
        for (const auto &v : {Json(true), Json(-1), Json(1.5), Json(4294967296ull)}) {
            bad = value;
            bad["values"][0]["value"] = v;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateMetricResult(meta, bad, raw));
        }
        bad = value;
        bad["values"][2]["value"] = std::numeric_limits<double>::infinity();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateMetricResult(meta, bad, raw));
        bad = value;
        bad["values"][2]["value"] = nullptr;
        QVERIFY(validateMetricResult(meta, bad, raw)["available"] == true);
        bad = value;
        bad["values"][4]["value"] = true;
        QCOMPARE(validateMetricResult(meta, bad, raw)["unavailable_reasons"], Json::array({"ReportLost"}));
    }
    void hardwareLifecycle() {
        if (!qEnvironmentVariableIsSet("FLORA_TEST_INTEL_METRICS"))
            QSKIP("Set FLORA_TEST_INTEL_METRICS for installed Intel driver checks");
        auto device = createDx11Device(false, false, 0x8086);
        QCOMPARE(device.information()["adapter"]["vendor_id"], Json(0x8086));
        MetricsDiscovery md(device.device.Get());
        QVERIFY(md.supportsDrain() && md.supportsSamples() && md.supportsReuse() && md.supportsRecorded());
        QVERIFY(!md.catalog().at("sets").empty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.begin());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.sampleBegin());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.select("invented"));
        md.select("ComputeBasic");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.end());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.end(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.sampleReserve(257));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.sampleSubmit(0));
        const char shader[] = "RWStructuredBuffer<uint> Output:register(u0); [numthreads(8,1,1)] void "
                              "main(uint3 id:SV_DispatchThreadID) { Output[id.x]=id.x*17+1337; }";
        Com<ID3DBlob> code, error;
        check(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                         &error),
              "Compile metric workload");
        Com<ID3D11ComputeShader> cs;
        check(
            device.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs),
            "Create metric workload");
        D3D11_BUFFER_DESC desc{
            1024, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            4};
        Com<ID3D11Buffer> buffer, staging;
        check(device.device->CreateBuffer(&desc, nullptr, &buffer), "Create metric buffer");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        check(device.device->CreateBuffer(&desc, nullptr, &staging), "Create metric staging");
        D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
        viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        viewDesc.Buffer.NumElements = 256;
        Com<ID3D11UnorderedAccessView> view;
        check(device.device->CreateUnorderedAccessView(buffer.Get(), &viewDesc, &view), "Create metric UAV");
        auto bind = [&](ID3D11DeviceContext *context) {
            context->CSSetShader(cs.Get(), nullptr, 0);
            auto *uav = view.Get();
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        };
        bind(device.context.Get());
        for (const auto *name : {"RenderBasic", "ComputeBasic"}) {
            md.select(name);
            std::vector<std::pair<uint64_t, unsigned>> pending;
            for (unsigned groups : {1u, 4u, 16u, 2u, 8u, 0u, 3u, 12u}) {
                const auto id = md.sampleBegin();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.sampleBegin());
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.clockPair());
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.samplePoll(id));
                device.context->Dispatch(groups, 1, 1);
                md.sampleSubmit(id);
                pending.emplace_back(id, groups);
            }
            QCOMPARE(md.sampleCount(), 8u);
            QCOMPARE(md.clockPair()["status"], Json(0));
            std::vector<MetricResult> results;
            for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
                auto value = wait([&](bool flush) { return md.samplePoll(it->first, flush); });
                QVERIFY(value.values["available"] == true);
                const auto &metrics = md.selected()["metrics"];
                bool found = false;
                for (size_t i = 0; i < metrics.size(); ++i)
                    if (metrics[i]["name"] == "CsThreads") {
                        QCOMPARE(value.values["values"][i]["value"], Json((it->second + 1) / 2));
                        found = true;
                    }
                QVERIFY(found);
                QCOMPARE(md.sampleResult(it->first).values, value.values);
                QCOMPARE(md.sampleResult(it->first).raw, value.raw);
                md.sampleRelease(it->first);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.samplePoll(it->first));
                results.push_back(std::move(value));
            }
            for (const auto &value : results)
                QCOMPARE(md.decode(value.raw).values, value.values);
        }
        device.context->CopyResource(staging.Get(), buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read metric workload");
        std::vector<uint32_t> pixels(256);
        memcpy(pixels.data(), mapped.pData, 1024);
        device.context->Unmap(staging.Get(), 0);
        for (unsigned i = 0; i < 128; ++i)
            QCOMPARE(pixels[i], i * 17 + 1337);
        md.sampleReserve(2);
        const auto id = md.sampleBegin(), object = md.sampleInfo(id);
        device.context->Dispatch(4, 1, 1);
        md.sampleSubmit(id);
        wait([&](bool flush) { return md.samplePoll(id, flush); });
        md.sampleRecycle(id);
        const auto recycled = md.sampleBegin();
        QCOMPARE(md.sampleInfo(recycled), object);
        QVERIFY(recycled != id);
        md.sampleRelease(recycled);
        QVERIFY(md.sampleStats()["reused"].get<unsigned>() >= 1);
        md.sampleClearCache();
        md.begin();
        device.context->Dispatch(2, 1, 1);
        md.submit();
        QVERIFY(wait([&](bool flush) { return md.poll(flush); }).values["available"] == true);
        md.discard();
        md.begin();
        device.context->Dispatch(4, 1, 1);
        QVERIFY(md.end().values["available"] == true);
        // Connect the recovered queue and clock directly to the installed driver.
        auto firstPair = md.clockPair();
        bool firstClock = true;
        MetricsDiscoveryClockSource clockSource(firstPair.at("maximum_ns").get<int64_t>(), [&] {
            const auto pair = firstClock ? firstPair : md.clockPair();
            firstClock = false;
            return MetricClockPair{pair.at("status").get<uint64_t>(), pair.at("gpu_ns").get<uint64_t>(),
                                   pair.at("cpu_ns").get<uint64_t>()};
        });
        MetricClock clock(clockSource);
        struct CounterSink final : MetricQuerySink {
            std::optional<MetricResult> result;
            unsigned completed{}, flushed{};
            uint64_t category() const override { return 0; }
            void setKey(uint64_t) override {}
            void complete(uint64_t, uint32_t) override { ++completed; }
            void flush() override { ++flushed; }
        } sink;
        struct CounterQuery final : MetricQuery {
            MetricsDiscovery &md;
            std::optional<MetricResult> result;
            explicit CounterQuery(MetricsDiscovery &value) : md(value) {}
            uint64_t category() const override { return 0; }
            void begin(void *) override { md.begin(); }
            void end(void *) override { md.submit(); }
            bool ready(bool flush) override {
                result = md.poll(flush);
                return result.has_value();
            }
            bool valid() override { return result.has_value(); }
            bool writeMetric(MetricQuerySink &value) override {
                static_cast<CounterSink &>(value).result = result;
                return true;
            }
        };
        auto query = std::make_shared<CounterQuery>(md);
        auto batch = std::make_shared<MetricQueryBatch>(std::vector<std::shared_ptr<MetricQuery>>{query});
        batch->begin(device.context.Get());
        device.context->Dispatch(4, 1, 1);
        batch->end(device.context.Get());
        PendingMetricPool pool;
        pool.bySlot[0] = {batch};
        MetricBatchSlots recycledBatches;
        MetricQueryDrain collector;
        collector.pending = &pool;
        collector.recycled = &recycledBatches;
        collector.metrics = {&sink};
        collector.updateClock = [&](bool force) { clock.update(force); };
        collector.drain(true);
        QCOMPARE(batch->state, 0u);
        QVERIFY(pool.bySlot[0].empty() && recycledBatches[0].size() == 1);
        QCOMPARE(sink.completed, 1u);
        QCOMPARE(sink.flushed, 1u);
        QVERIFY(sink.result && sink.result->values["available"] == true && clockSource.lastSuccess());
        md.discard();
        MetricTypedReport typed;
        for (const auto &value : sink.result->values["values"]) {
            MetricTypedValue record{};
            const auto type = value["type"].get<uint32_t>();
            std::memcpy(record.data(), &type, 4);
            QVERIFY(!value["value"].is_null());
            if (type == 2) {
                const auto number = value["value"].get<float>();
                std::memcpy(record.data() + 8, &number, 4);
            } else {
                const auto number =
                    type == 3 ? uint64_t(value["value"].get<bool>()) : value["value"].get<uint64_t>();
                std::memcpy(record.data() + 8, &number, 8);
            }
            typed.push_back(record);
        }
        auto information = md.selected()["information"];
        for (auto &definition : information)
            definition["information_type"] = definition["info_type"];
        MetricBusyState busy;
        const auto converted =
            postprocessMetricReports({typed}, md.selected()["metrics"], information, clock, busy);
        QVERIFY(converted.keys[0] != 0);
        bool durationChecked = false;
        for (size_t i = 0; i < md.selected()["metrics"].size(); ++i) {
            if (md.selected()["metrics"][i]["name"] == "GpuTime") {
                QCOMPARE(metricTimestampInteger(converted.reports[0][i]),
                         metricTimestampInteger(typed[i]) / 1000);
                durationChecked = true;
            }
        }
        QVERIFY(durationChecked);
        // Exercise the migrated collector with real reusable counters and varied workloads.
        struct ScheduledObserver final : MetricPublisherObserver {
            MetricClock &clock;
            size_t records{}, refreshes{};
            explicit ScheduledObserver(MetricClock &value) : clock(value) {}
            void update(bool force) override {
                clock.update(force);
                ++refreshes;
            }
            size_t recordCount() const override { return records; }
            size_t refreshCount() const override { return refreshes; }
        } observer(clock);
        MdScheduledPool scheduled(md, observer, 3);
        const std::array<unsigned, 9> workloads{1, 4, 16, 2, 8, 0, 3, 12, 5};
        std::vector<std::pair<unsigned, MetricResult>> delivered;
        for (const auto groups : workloads) {
            scheduled.begin([&, groups](MetricResult &result) {
                delivered.emplace_back(groups, result);
                ++observer.records;
            });
            QVERIFY(scheduled.active());
            device.context->Dispatch(groups, 1, 1);
            scheduled.submit();
            QVERIFY(!scheduled.active());
        }
        scheduled.finish();
        QCOMPARE(scheduled.ownedCount(), size_t(0));
        QCOMPARE(md.sampleCount(), 0u);
        QCOMPARE(delivered.size(), workloads.size());
        QVERIFY(observer.refreshes > 0);
        size_t csIndex = md.selected()["metrics"].size();
        for (size_t i = 0; i < md.selected()["metrics"].size(); ++i)
            if (md.selected()["metrics"][i]["name"] == "CsThreads")
                csIndex = i;
        QVERIFY(csIndex < md.selected()["metrics"].size());
        for (size_t i = 0; i < workloads.size(); ++i) {
            QCOMPARE(delivered[i].first, workloads[i]);
            const auto &result = delivered[i].second;
            QVERIFY(result.values["available"] == true);
            QCOMPARE(result.values["values"][csIndex]["value"], Json((workloads[i] + 1) / 2));
            QCOMPARE(md.decode(result.raw).values, result.values);
        }
        const auto schedule = scheduled.report();
        QCOMPARE(schedule["records"].size(), workloads.size());
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> identities;
        for (size_t i = 0; i < schedule["records"].size(); ++i) {
            const auto &record = schedule["records"][i];
            QCOMPARE(record["report_index"], Json(i));
            QCOMPARE(record["state_at_delivery"], Json(3));
            const auto batchId = record["batch_id"].get<uint64_t>();
            const auto counterId = record["counter_id"].get<uint64_t>();
            const auto use = record["use_index"].get<uint64_t>();
            if (auto found = identities.find(batchId); found != identities.end()) {
                QCOMPARE(counterId, found->second.first);
                QCOMPARE(use, found->second.second + 1);
                found->second.second = use;
            } else {
                QCOMPARE(use, uint64_t(1));
                identities.emplace(batchId, std::pair(counterId, use));
            }
        }
        QVERIFY(identities.size() <= 3);
        scheduled.close();
        QCOMPARE(md.sampleStats()["cached"], Json(0));
        // Explicit close releases native objects but leaves the scheduler reusable.
        scheduled.begin();
        device.context->Dispatch(6, 1, 1);
        const auto retry = scheduled.end();
        QVERIFY(retry.values["available"] == true);
        QCOMPARE(retry.values["values"][csIndex]["value"], Json(3));
        scheduled.close();
        QCOMPARE(md.sampleCount(), 0u);
        QCOMPARE(md.sampleStats()["cached"], Json(0));
        for (bool reuse : {false, true}) {
            ScheduledObserver adapterObserver(clock);
            std::unique_ptr<MdSamplePool> samples;
            if (reuse)
                samples = std::make_unique<MdReusingPool>(md, adapterObserver, 3);
            else
                samples = std::make_unique<MdSamplePool>(md, adapterObserver, 3);
            std::vector<std::pair<unsigned, MetricResult>> records;
            for (auto groups : workloads) {
                samples->begin([&, groups](MetricResult &result) {
                    records.emplace_back(groups, result);
                    ++adapterObserver.records;
                });
                device.context->Dispatch(groups, 1, 1);
                samples->submit();
            }
            samples->finish();
            QVERIFY(!samples->active() && samples->ownedCount() == 0);
            QCOMPARE(md.sampleCount(), 0u);
            QCOMPARE(records.size(), workloads.size());
            for (size_t i = 0; i < workloads.size(); ++i) {
                QCOMPARE(records[i].first, workloads[i]);
                QVERIFY(records[i].second.values["available"] == true);
                QCOMPARE(records[i].second.values["values"][csIndex]["value"], Json((workloads[i] + 1) / 2));
                QCOMPARE(md.decode(records[i].second.raw).values, records[i].second.values);
            }
            const auto history = samples->report();
            QCOMPARE(history["records"].size(), workloads.size());
            QVERIFY(history["high_watermark"].get<unsigned>() <= 3);
            std::map<uint64_t, std::pair<uint64_t, uint64_t>> counterUses;
            for (size_t i = 0; i < history["records"].size(); ++i) {
                const auto &record = history["records"][i];
                QCOMPARE(record["report_index"], Json(i));
                QCOMPARE(record["state"], Json(0));
                if (reuse) {
                    const auto batchId = record["batch_id"].get<uint64_t>();
                    const auto identity = record["counter_id"].get<uint64_t>();
                    const auto use = record["use_index"].get<uint64_t>();
                    if (auto previous = counterUses.find(batchId); previous != counterUses.end()) {
                        QCOMPARE(identity, previous->second.first);
                        QCOMPARE(use, ++previous->second.second);
                    } else {
                        QCOMPARE(use, uint64_t(1));
                        counterUses.emplace(batchId, std::pair(identity, use));
                    }
                }
            }
            if (reuse)
                QVERIFY(counterUses.size() <= 3);
            samples->close();
            QCOMPARE(md.sampleStats()["cached"], Json(0));
            samples->begin();
            device.context->Dispatch(6, 1, 1);
            QCOMPARE(samples->end().values["values"][csIndex]["value"], Json(3));
            samples->close();
            QCOMPARE(md.sampleCount(), 0u);
            QCOMPARE(md.sampleStats()["cached"], Json(0));
        }
        {
            ScheduledObserver counterObserver(clock);
            MdCounter counter(md, counterObserver);
            for (auto groups : workloads) {
                counter.begin();
                device.context->Dispatch(groups, 1, 1);
                const auto result = counter.end();
                QVERIFY(!counter.active() && result.values["available"] == true);
                QCOMPARE(result.values["values"][csIndex]["value"], Json((groups + 1) / 2));
                QCOMPARE(md.decode(result.raw).values, result.values);
                ++counterObserver.records;
            }
            QCOMPARE(counter.audit().size(), workloads.size());
            for (size_t i = 0; i < counter.audit().size(); ++i) {
                const auto &record = counter.audit()[i];
                QCOMPARE(record["report_index"], Json(i));
                QCOMPARE(record["state"], Json(0));
                QCOMPARE(record["recycled"], Json(1));
                QCOMPARE(record["refresh_range"], Json::array({i, i + 1}));
            }
        }
        Com<ID3D11DeviceContext> deferred;
        check(device.device->CreateDeferredContext(0, &deferred), "Create recorded context");
        bind(deferred.Get());
        const auto recorded = md.recordedBegin(deferred.Get());
        deferred->Dispatch(8, 1, 1);
        md.recordedEnd(recorded);
        Com<ID3D11CommandList> command;
        check(deferred->FinishCommandList(FALSE, &command), "Finish recorded workload");
        const std::array<uint64_t, 1> tokens{recorded};
        const auto execution = md.recordedExecute(command.Get(), tokens);
        auto value = wait([&](bool flush) { return md.recordedPoll(recorded, execution, flush); });
        QVERIFY(value.values["available"] == true);
        QCOMPARE(md.recordedResult(recorded, execution).values, value.values);
        const auto next = md.recordedExecute(command.Get(), tokens);
        QVERIFY(next != execution);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.recordedPoll(recorded, execution));
        wait([&](bool flush) { return md.recordedPoll(recorded, next, flush); });
        md.recordedRelease(recorded);
        QCOMPARE(md.recordedCount(), 0u);
        Com<ID3D11DeviceContext> otherDeferred;
        check(device.device->CreateDeferredContext(0, &otherDeferred), "Create second recorded context");
        const std::array<ID3D11DeviceContext *, 2> contexts{deferred.Get(), otherDeferred.Get()};
        const std::array<unsigned, 6> recordedWork{1, 4, 16, 2, 3, 8};
        for (bool publisherValues : {false, true}) {
            MdRecordedQueries session(md, 10000, publisherValues);
            for (const auto *set : {"ComputeBasic", "RenderBasic"}) {
                md.select(set);
                std::vector<uint64_t> lists;
                for (unsigned list = 0; list < 3; ++list) {
                    auto *context = contexts[list % 2];
                    bind(context);
                    for (unsigned range = 0; range < 2; ++range) {
                        const auto key = list * 2 + range;
                        session.begin(uint64_t(reinterpret_cast<uintptr_t>(context)), key, key * 10);
                        context->Dispatch(recordedWork[key], 1, 1);
                        session.end(uint64_t(reinterpret_cast<uintptr_t>(context)));
                    }
                    lists.push_back(
                        session.finish(uint64_t(reinterpret_cast<uintptr_t>(context)), list % 2 != 0));
                }
                for (unsigned repeat = 0; repeat < 3; ++repeat) {
                    for (unsigned index = 0; index < 3; ++index)
                        session.execute(lists[(index + repeat) % 3], (index + repeat) % 2 != 0);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.report());
                    session.drain(false);
                }
                for (auto list : lists)
                    session.release(list);
                session.drain(true);
                QCOMPARE(md.recordedCount(), 0u);
                QCOMPARE(session.ownedCount(), size_t(0));
                QCOMPARE(session.commandCount(), size_t(0));
            }
            const auto report = session.report();
            QCOMPARE(report["sets"].size(), size_t(2));
            QCOMPARE(report["records"].size(), size_t(36));
            QCOMPARE(report["executions"].size(), size_t(18));
            std::map<std::pair<std::string, uint64_t>, unsigned> deliveries;
            for (const auto &row : report["records"]) {
                const auto set = row["set"].get<std::string>();
                const auto key = row["key0"].get<uint64_t>();
                QVERIFY(key < recordedWork.size());
                QCOMPARE(row["key1"], Json(key * 10));
                QVERIFY(row["result"]["available"] == true);
                const auto &sets = report["sets"];
                const auto definitions = std::find_if(
                    sets.begin(), sets.end(), [&](const auto &group) { return group["name"] == set; });
                for (size_t i = 0; i < definitions->at("metrics").size(); ++i)
                    if (definitions->at("metrics")[i]["name"] == "CsThreads")
                        QCOMPARE(row["result"]["values"][i]["value"], Json((recordedWork[key] + 1) / 2));
                ++deliveries[{set, key}];
            }
            QCOMPARE(deliveries.size(), size_t(12));
            for (const auto &[key, count] : deliveries)
                QCOMPARE(count, 3u);
            if (publisherValues) {
                const auto &convertedRecords = report["publisher_values"]["records"];
                QCOMPARE(convertedRecords.size(), report["records"].size());
                QVERIFY(!report["publisher_values"]["refreshes"].empty());
                for (size_t i = 0; i < convertedRecords.size(); ++i) {
                    for (const auto *field :
                         {"set", "key0", "key1", "tag", "token", "execution", "list_id", "context_slot"})
                        QCOMPARE(convertedRecords[i][field], report["records"][i][field]);
                    const auto raw = QByteArray::fromHex(
                        QByteArray::fromStdString(report["records"][i]["raw_hex"].get<std::string>()));
                    QCOMPARE(convertedRecords[i]["raw_sha256"],
                             Json(sha256(
                                 Bytes(reinterpret_cast<const uint8_t *>(raw.data()), size_t(raw.size())))));
                    for (const auto &field : convertedRecords[i]["values"]) {
                        const auto typedBytes = QByteArray::fromHex(
                            QByteArray::fromStdString(field["typed_hex"].get<std::string>()));
                        QCOMPARE(metricTypedDouble(Bytes(reinterpret_cast<const uint8_t *>(typedBytes.data()),
                                                         size_t(typedBytes.size()))),
                                 field["value"].get<double>());
                    }
                }
            } else
                QVERIFY(report["publisher_values"].is_null());
            QTemporaryDir output;
            const auto folder = output.filePath("recorded");
            QCOMPARE(session.exportReport(folder), report);
            QCOMPARE(read(folder + "/recorded-profile.json"), report);
            QVERIFY(QFile::exists(folder + "/raw-values.csv"));
            QCOMPARE(QFile::exists(folder + "/publisher-values.csv"), publisherValues);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.exportReport(folder));
            session.close();
            QCOMPARE(session.report(), report);
        }
        for (const std::string fault : {"begin", "end", "execute", "poll", "timeout", "clock"}) {
            FaultRecorded transport(md);
            MdRecordedQueries session(transport, fault == "timeout" ? 1 : 10000, fault == "clock");
            const auto context = uint64_t(reinterpret_cast<uintptr_t>(deferred.Get()));
            bind(deferred.Get());
            if (fault == "begin") {
                transport.fault = fault;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.begin(context, 77));
            } else {
                session.begin(context, 77);
                deferred->Dispatch(4, 1, 1);
                if (fault == "end") {
                    transport.fault = fault;
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.end(context));
                } else {
                    session.end(context);
                    const auto list = session.finish(context);
                    if (fault == "execute") {
                        transport.fault = fault;
                        QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.execute(list));
                    } else {
                        session.execute(list);
                        transport.fault = fault;
                        QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.drain());
                    }
                }
            }
            QVERIFY(session.closed() && session.failed());
            QVERIFY(session.records().empty());
            QCOMPARE(session.commandCount(), size_t(0));
            QCOMPARE(session.ownedCount(), size_t(0));
            QCOMPARE(md.recordedCount(), 0u);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.report());
            Com<ID3D11CommandList> discarded;
            check(deferred->FinishCommandList(FALSE, &discarded), "Discard failed recording");
        }
        {
            MdRecordedQueries recordedRetry(md, 10000, true);
            const auto context = uint64_t(reinterpret_cast<uintptr_t>(deferred.Get()));
            bind(deferred.Get());
            recordedRetry.begin(context, 88);
            deferred->Dispatch(2, 1, 1);
            recordedRetry.end(context);
            const auto list = recordedRetry.finish(context);
            recordedRetry.execute(list);
            recordedRetry.drain();
            recordedRetry.release(list);
            QCOMPARE(recordedRetry.report()["records"].size(), size_t(1));
            QCOMPARE(md.recordedCount(), 0u);
        }
        {
            QTemporaryDir controllerDir;
            QVERIFY(controllerDir.isValid());
            MetricPriorityOptions lockOptions;
            lockOptions.path = controllerDir.filePath("controller.table");
            lockOptions.mutexName = "FloraGPA_PassController_" + QUuid::createUuid().toString();
            auto lock = metricDeviceMutex(md.catalog(), lockOptions);
            struct Publisher final : MetricPassPublisher, MetricProbeRegistryBackend {
                MetricsDiscovery &md;
                MetricPublisherObserver &observer;
                MetricPassController *manager{};
                std::unique_ptr<MdScheduledPool> pool;
                MetricProbeRegistry registry;
                uint64_t devicePointer;
                std::vector<uint64_t> createdKinds, destroyedHandles;
                std::vector<uint64_t> subscribed;
                bool failSecond{};
                size_t delivered{};
                Publisher(MetricsDiscovery &m, MetricPublisherObserver &o, ID3D11Device *device)
                    : md(m), observer(o), registry(*this),
                      devicePointer(uint64_t(reinterpret_cast<uintptr_t>(device))) {
                    if (!registry.registerType("probe_types/22") || !registry.registerType("probe_types/9"))
                        throw std::runtime_error("Test probe registration failed");
                }
                MetricProbeResult lookup(uint64_t parent, const std::string &path) override {
                    if (!parent)
                        return {0, 555};
                    return {0, path == "probe_types/22" ? 22 : 9};
                }
                MetricProbeResult createProbe(uint64_t kind, Bytes configuration) override {
                    Reader reader(configuration);
                    if (reader.read<uint32_t>() != 3 || reader.read<uint64_t>() != devicePointer ||
                        reader.remaining())
                        throw std::runtime_error("Probe device configuration mismatch");
                    createdKinds.push_back(kind);
                    return {0, kind};
                }
                int32_t destroyProbe(uint64_t handle) override {
                    closePool();
                    destroyedHandles.push_back(handle);
                    return 0;
                }
                void closePool() {
                    if (pool)
                        pool->close();
                    pool.reset();
                }
                void setPool(uint32_t capacity) override {
                    closePool();
                    md.select(manager->currentPass == 0 ? "RenderBasic" : "ComputeBasic");
                    pool = std::make_unique<MdScheduledPool>(md, observer, capacity);
                }
                int32_t subscribe(uint64_t, uint64_t handle) override {
                    subscribed.push_back(handle);
                    return 0;
                }
                int32_t unsubscribe(uint64_t, uint64_t handle) override {
                    std::erase(subscribed, handle);
                    return 0;
                }
                bool configure(uint64_t group, Bytes configuration) override {
                    return registry.configure(group, configuration);
                }
                bool begin(uint64_t a, uint64_t b, uint32_t tag, uint64_t ctx) override {
                    return registry.begin(a, b, tag, ctx);
                }
                bool end(uint64_t key, uint32_t tag, uint64_t ctx) override {
                    return registry.end(key, tag, ctx);
                }
                void flush(uint64_t) override {
                    if (pool)
                        pool->finish();
                }
                int32_t beginProbe(uint64_t handle, uint64_t, uint64_t, uint32_t, uint64_t) override {
                    if (handle == 22)
                        return failSecond ? 9 : 0;
                    pool->begin([&](MetricResult &result) {
                        if (!result.values.at("available").get<bool>())
                            throw std::runtime_error("Unavailable controller sample");
                        size_t index = 0;
                        while (index < md.selected()["metrics"].size() &&
                               md.selected()["metrics"][index]["name"] != "CsThreads")
                            ++index;
                        const auto value = result.values["values"].at(index).at("value").get<double>();
                        std::array<uint8_t, 20> packet{};
                        const uint32_t header = 0x14600000;
                        std::memcpy(packet.data(), &header, 4);
                        std::memcpy(packet.data() + 12, &value, 8);
                        for (auto h : subscribed)
                            manager->consumer().receive(h, 0xa3, Bytes(packet));
                        ++delivered;
                    });
                    return 0;
                }
                int32_t endProbe(uint64_t handle, uint64_t, uint32_t, uint64_t) override {
                    if (handle == 9)
                        pool->submit();
                    return 0;
                }
            } publisher(md, observer, device.device.Get());
            MetricPassController manager({11, 22, 33}, {}, {}, publisher, *lock, nullptr, {{"pool_size", 2}});
            publisher.manager = &manager;
            const Json devices = Json::array({Json::array({6, publisher.devicePointer, Json::array({17})})});
            const auto plan = manager.prepare({0, 1, 2}, {{0}, {1}, {0}}, 6, [&](uint32_t key) {
                return dx11MetricConfigurationForKey(devices, key);
            });
            QVERIFY(plan["passes"] == Json::array({{0, 2}, {1}}));
            QCOMPARE(publisher.createdKinds, std::vector<uint64_t>({22, 9}));
            QVERIFY(manager.configure(6, dx11MetricConfigurationForKey(devices, 17)));
            QCOMPARE(publisher.createdKinds.size(), size_t(2));
            bind(device.context.Get());
            for (uint32_t pass = 0; pass < 2; ++pass) {
                manager.select(pass);
                QVERIFY(lock->audit()["depth"] == 1);
                manager.begin(5);
                QVERIFY(!publisher.pool->active());
                for (unsigned groups : {1u, 4u, 8u}) {
                    manager.begin(6);
                    device.context->Dispatch(groups, 1, 1);
                    manager.end();
                }
                manager.finish(true);
                publisher.closePool();
                QCOMPARE(manager.completedProbes, 3u);
                QVERIFY(lock->audit()["depth"] == 0);
            }
            QCOMPARE(publisher.delivered, size_t(6));
            QCOMPARE(manager.consumer().counts(), std::vector<uint64_t>({3, 3, 3}));
            const auto &rows = manager.consumer().rows();
            QCOMPARE(rows, std::vector<std::vector<double>>({{1., 1., 1.}, {2., 2., 2.}, {4., 4., 4.}}));
            const auto assembled =
                receiveDx11MetricResult({11, 22, 33}, {0, 0, 0}, rows, manager.consumer().timings());
            QCOMPARE(assembled["status"], Json(0));
            for (const auto &metric : assembled["metrics"]) {
                QVERIFY(metric["values"] == Json::array({1., 2., 4.}));
                QVERIFY(metric["aux"] == Json::array({0, 0, 0}));
            }
            manager.select(0);
            publisher.failSecond = true;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, manager.begin(6));
            QVERIFY(publisher.pool->active());
            QVERIFY(lock->audit()["depth"] == 1);
            publisher.closePool();
            manager.finish(true);
            QCOMPARE(md.sampleCount(), 0u);
            QVERIFY(lock->audit()["depth"] == 0);
            publisher.failSecond = false;
            manager.select(0);
            manager.begin(6);
            device.context->Dispatch(4, 1, 1);
            manager.end();
            manager.finish();
            publisher.closePool();
            QCOMPARE(manager.consumer().rows(), std::vector<std::vector<double>>({{2., 0., 2.}}));
            QCOMPARE(md.sampleCount(), 0u);
            QCOMPARE(md.sampleStats()["cached"], Json(0));
            publisher.registry.release();
            QCOMPARE(publisher.destroyedHandles, std::vector<uint64_t>({9, 22}));
            lock->close();
        }
        QVERIFY(md.provenance()["driver"]["sha256"].get<std::string>().size() == 64);
        auto warp = createDx11Device(true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, MetricsDiscovery(warp.device.Get()));
        {
            QTemporaryDir arbitrationDir;
            QVERIFY(arbitrationDir.isValid());
            MetricPriorityOptions options;
            options.path = arbitrationDir.filePath("private-priority.table");
            options.mutexName = "FloraGPA_MetricIntegration_" + QUuid::createUuid().toString();
            const auto factory = [&](const Json &catalog) { return metricDeviceMutex(catalog, options); };
            md.select("ComputeBasic");
            bind(device.context.Get());
            MetricAcquisitionPriority acquisition(md, arbitrationDir.path(), factory);
            unsigned completed{};
            acquisition.run([&](MetricAcquisitionPriority &scope) {
                for (unsigned i = 0; i < 3; ++i)
                    scope.replay(i, "ComputeBasic", i, [&] {
                        md.begin();
                        device.context->Dispatch(4, 1, 1);
                        const auto report = md.end();
                        if (report.values.at("available") != true)
                            throw std::runtime_error("Arbitrated Intel sample unavailable");
                        ++completed;
                    });
            });
            QCOMPARE(completed, 3u);
            const auto audit = acquisition.report();
            Json validation = Json::array();
            for (unsigned i = 0; i < 3; ++i)
                validation.push_back({{"pass_index", i}, {"set", "ComputeBasic"}, {"sample_index", i}});
            const Json profile = {{"arbitration", "gpa_priority_v2"},
                                  {"priority_audit", "priority-audit.json"},
                                  {"adapter_luid", md.catalog().at("luid")},
                                  {"validation", {{"passes", validation}}}};
            QVERIFY(validateMetricPriorityResult(arbitrationDir.path(), profile, true) == audit);
            QVERIFY(!md.closed());
            MetricAcquisitionPriority failed(md, arbitrationDir.path(), factory);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, failed.run([&](MetricAcquisitionPriority &scope) {
                scope.replay(0, "ComputeBasic", 0, [&] {
                    md.begin();
                    device.context->Dispatch(2, 1, 1);
                    throw std::runtime_error("injected begun-counter replay failure");
                });
            }));
            QVERIFY(md.closed());
            QVERIFY(failed.report()["passes"][0]["counters_closed_after_failure"] == true);
            QVERIFY(failed.report()["lock"]["closed"] == true);
            QVERIFY(!QFile::exists(*options.path));
            MetricsDiscovery retried(device.device.Get());
            retried.select("ComputeBasic");
            MetricAcquisitionPriority priorityRetry(retried, arbitrationDir.path(), factory);
            priorityRetry.run([&](MetricAcquisitionPriority &scope) {
                scope.replay(0, "ComputeBasic", 0, [&] {
                    retried.begin();
                    device.context->Dispatch(2, 1, 1);
                    if (retried.end().values.at("available") != true)
                        throw std::runtime_error("Arbitrated retry unavailable");
                });
            });
            QVERIFY(priorityRetry.report()["passes"][0]["complete"] == true);
        }
        {
            MetricsDiscovery live(device.device.Get());
            QTemporaryDir arbitrationDir;
            QVERIFY(arbitrationDir.isValid());
            MetricPriorityOptions options;
            options.path = arbitrationDir.filePath("iteration-priority.table");
            options.mutexName = "FloraGPA_MdIteration_" + QUuid::createUuid().toString();
            const auto factory = [&](const Json &catalog) { return metricDeviceMutex(catalog, options); };
            bind(device.context.Get());
            uint64_t sample{};
            const auto acquire = [&](MdIterationTransport &transport, uint32_t pass, const Json &ranges) {
                const auto current = sample++;
                for (const auto &range : ranges) {
                    const auto event = range.get<unsigned>();
                    transport.begin([&, event, pass, current](MetricResult &result) {
                        const auto &set = live.selected();
                        const auto count = set.at("metrics").size();
                        const auto &all = result.values.at("values");
                        Json values = Json::array(), info = Json::array();
                        for (size_t i = 0; i < all.size(); ++i)
                            (i < count ? values : info).push_back(all[i]);
                        transport.deliver(set,
                                          {{"set", set.at("name")},
                                           {"event", event},
                                           {"pass_index", pass},
                                           {"sample_index", current},
                                           {"raw_report", "hardware.bin"},
                                           {"raw_sha256", sha256(result.raw)},
                                           {"available", result.values.at("available")},
                                           {"unavailable_reasons", result.values.at("unavailable_reasons")},
                                           {"values", values},
                                           {"information", info}});
                    });
                    device.context->Dispatch(event, 1, 1);
                    transport.submit();
                }
            };
            {
                MdIterationTransport session(live, device.device.Get(), Json::array({"CsThreads"}), acquire,
                                             factory);
                const auto result = MetricIterationRunner(session).collect(
                    session.requestedIds(), {1, 4, 8}, {{"weights", {1., 1., 1.}}, {"samples", 2}});
                QVERIFY(result.at("complete") == true);
                QCOMPARE(result.at("iterations").size(), size_t(2));
                for (const auto &iteration : result.at("iterations"))
                    QCOMPARE(iteration.at("metrics")[0].at("values"), Json::array({1., 2., 4.}));
                QCOMPARE(session.publisherValues().recordCount(), size_t(6));
                session.close();
                const auto audit = session.audit();
                QCOMPARE(audit["query_pools"].size(), size_t(2));
                QCOMPARE(audit["probes_created"], Json(1));
                QCOMPARE(audit["local_lock_depth"], Json(0));
                QVERIFY(audit["subscriptions"].empty());
                QCOMPARE(live.sampleCount(), 0u);
                QCOMPARE(live.sampleStats()["cached"], Json(0));
            }
            {
                MdIterationTransport session(
                    live, device.device.Get(),
                    {"GpuTime", "EuActive", "Sampler00InputAvailable", "Sampler00OutputReady"}, acquire,
                    factory);
                const auto prepared = session.prepare(session.requestedIds());
                QCOMPARE(prepared["groups"].size(), size_t(2));
                for (auto pass : {1u, 0u, 1u}) {
                    const auto result = session.replay(pass, {1, 4, 8}, false);
                    QVERIFY(!result["metrics"].empty());
                    for (const auto &metric : result["metrics"]) {
                        QCOMPARE(metric["values"].size(), size_t(3));
                        for (const auto &v : metric["values"])
                            QVERIFY(v.is_number() && std::isfinite(v.get<double>()));
                    }
                }
                QCOMPARE(session.publisherValues().recordCount(), size_t(9));
                session.close();
                QCOMPARE(session.audit()["probes_created"], Json(1));
                QCOMPARE(live.sampleCount(), 0u);
                QCOMPARE(live.sampleStats()["cached"], Json(0));
            }
            QVERIFY(!QFile::exists(*options.path));
        }
        for (const auto fault : {"release", "clear", "stats"}) {
            for (bool retryClose : {false, true}) {
                MetricsDiscovery live(device.device.Get());
                FaultSessionSamples samples(live);
                samples.fault = fault;
                QTemporaryDir arbitrationDir;
                QVERIFY(arbitrationDir.isValid());
                MetricPriorityOptions options;
                options.path = arbitrationDir.filePath("failed-iteration.table");
                options.mutexName = "FloraGPA_FailedIteration_" + QUuid::createUuid().toString();
                SharedMetricPriorityMutex *lock{};
                std::vector<unsigned> closeDepths;
                MdIterationClient client{samples,
                                         live,
                                         live.catalog(),
                                         [&](const std::string &name) { live.select(name); },
                                         [&] { return live.sampleCount(); },
                                         [&] {
                                             closeDepths.push_back(lock->audit().at("depth").get<unsigned>());
                                             if (retryClose && closeDepths.size() == 1)
                                                 throw std::runtime_error("Injected native close");
                                             live.close();
                                         },
                                         [&] { return live.closed(); }};
                MdIterationTransport session(
                    client, uint64_t(reinterpret_cast<uintptr_t>(device.device.Get())), {"CsThreads"},
                    [&](MdIterationTransport &transport, uint32_t, const Json &) {
                        transport.begin({});
                        device.context->Dispatch(4, 1, 1);
                        throw std::runtime_error("Injected replay failure with a begun query");
                    },
                    [&](const Json &catalog) {
                        auto result = metricDeviceMutex(catalog, options);
                        lock = result.get();
                        return result;
                    });
                session.prepare(session.requestedIds());
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.replay(0, {1}, false));
                QCOMPARE(lock->audit()["depth"], Json(1));
                session.close();
                QVERIFY(session.closed() && live.closed());
                QCOMPARE(closeDepths, std::vector<unsigned>(retryClose ? 2 : 1, 1));
                const auto audit = session.audit();
                QCOMPARE(audit["local_lock_depth"], Json(0));
                QCOMPARE(audit["cleanup_failure"]["stage"], Json("query_pool"));
                if (std::string(fault) == "stats")
                    QCOMPARE(audit["cleanup_failure"]["before_device_close"]["native_pool"]["unavailable"],
                             Json("RuntimeError"));
                QVERIFY(!QFile::exists(*options.path));
            }
        }
        md.close();
        md.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, md.begin());
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        auto result = probe(read(args[2]));
        QFile output(args[3]);
        if (!output.open(QIODevice::WriteOnly))
            return 2;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    MetricsDiscoveryTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricsDiscoveryTests.moc"
