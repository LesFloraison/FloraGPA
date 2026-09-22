#include "application/MdSamplePool.h"
#include "application/MdScheduledPool.h"
#include "application/MetricClock.h"
#include "application/MetricQueries.h"
#include "application/MetricReport.h"
#include "application/MetricsDiscovery.h"
#include "replay/Device.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QtTest>
#include <d3dcompiler.h>
#include <functional>
#include <map>
using namespace flora;
using Json = nlohmann::json;
namespace {
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
        QVERIFY(md.provenance()["driver"]["sha256"].get<std::string>().size() == 64);
        auto warp = createDx11Device(true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, MetricsDiscovery(warp.device.Get()));
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
