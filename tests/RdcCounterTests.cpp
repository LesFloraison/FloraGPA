#include "rdc/Counters.h"
#include <QtTest>
#include <cmath>
#include <cstdlib>
#include <limits>
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return std::malloc(size_t(size)); }
extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *p) { std::free(p); }
using namespace flora;
class RdcCounterTests : public QObject {
    Q_OBJECT
  private slots:
    void exactIntegerStorage() {
        CounterDescription d{};
        d.counter = GPUCounter::InputVerticesRead;
        d.resultType = CompType::UInt;
        d.resultByteWidth = 8;
        CounterResult r(9, d.counter, UINT64_MAX);
        auto value = rdcCounterValue(d, r);
        QCOMPARE(value.get<uint64_t>(), UINT64_MAX);
        QCOMPARE(value.dump(), std::string("18446744073709551615"));
        d.resultByteWidth = 4;
        r.value.u32 = UINT32_MAX;
        QCOMPARE(rdcCounterValue(d, r).get<uint32_t>(), UINT32_MAX);
        d.resultType = CompType::SInt;
        QCOMPARE(rdcCounterValue(d, r).get<uint32_t>(), UINT32_MAX);
        d.resultByteWidth = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcCounterValue(d, r));
        d.resultByteWidth = 4;
        r.counter = GPUCounter::VSInvocations;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcCounterValue(d, r));
    }
    void floatingStorage() {
        CounterDescription d{};
        d.counter = GPUCounter::EventGPUDuration;
        d.resultType = CompType::Float;
        d.resultByteWidth = 8;
        CounterResult r(9, d.counter, 1.2345678901234567);
        QCOMPARE(rdcCounterValue(d, r).get<double>(), r.value.d);
        r.value.d = std::numeric_limits<double>::infinity();
        QCOMPARE(rdcCounterValue(d, r).get<std::string>(), std::string("inf"));
        r.value.d = -std::numeric_limits<double>::infinity();
        QCOMPARE(rdcCounterValue(d, r).get<std::string>(), std::string("-inf"));
        r.value.d = std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(rdcCounterValue(d, r).get<std::string>(), std::string("nan"));
        d.resultByteWidth = 4;
        r.value.f = -0.f;
        QVERIFY(std::signbit(rdcCounterValue(d, r).get<double>()));
        r.value.f = 1.25f;
        QCOMPARE(rdcCounterValue(d, r).get<double>(), 1.25);
    }
    void catalogAndUnits() {
        CounterDescription d{};
        d.counter = GPUCounter::VSInvocations;
        d.name = "Vertices";
        d.category = "D3D11";
        d.description = "Actual invocations";
        d.resultType = CompType::UInt;
        d.resultByteWidth = 8;
        d.unit = CounterUnit::Absolute;
        d.uuid = Uuid(1, 2, 3, UINT32_MAX);
        auto j = rdcCounterDescription(d);
        QCOMPARE(j.at("uuid").at("words"), nlohmann::json({1, 2, 3, UINT32_MAX}));
        QCOMPARE(j.at("resultType"), nlohmann::json(uint32_t(CompType::UInt)));
        QCOMPARE(j.at("measurable"), nlohmann::json(true));
        QCOMPARE(j.at("status"), nlohmann::json("available_not_sampled"));
        QCOMPARE(j.at("source"), nlohmann::json("renderdoc_builtin"));
        QCOMPARE(j.at("unit_name"), nlohmann::json("CounterUnit.Absolute"));
        for (unsigned i = 0; i < 9; ++i)
            QVERIFY(rdcCounterUnit(CounterUnit(i)).starts_with("CounterUnit."));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcCounterUnit(CounterUnit(99)));
    }
    void explicitHardwareSelection() {
        using Json = nlohmann::json;
        CounterDescription builtin{}, hardware{}, diagnostic{};
        builtin.counter = GPUCounter::EventGPUDuration;
        builtin.resultType = CompType::Float; builtin.resultByteWidth = 8;
        hardware = builtin; hardware.counter = GPUCounter(uint32_t(GPUCounter::FirstNvidia) + 1);
        hardware.unit = CounterUnit::Percentage;
        diagnostic.counter = GPUCounter::FirstNvidia;
        diagnostic.name = "ERROR: Could not find Nsight Perf SDK library";
        std::map<GPUCounter, CounterDescription> catalog{{builtin.counter, builtin},
            {hardware.counter, hardware}, {diagnostic.counter, diagnostic}};
        const auto defaults = selectRdcCounters(catalog, nullptr);
        QCOMPARE(defaults.size(), size_t(1)); QCOMPARE(defaults[0], builtin.counter);
        const auto selected = selectRdcCounters(catalog, Json::array({uint32_t(hardware.counter), 1}));
        QCOMPARE(selected.size(), size_t(2)); QCOMPARE(selected[0], hardware.counter);
        QCOMPARE(rdcCounterDescription(hardware).at("source"), Json("renderdoc_nvidia"));
        QCOMPARE(rdcCounterDescription(hardware).at("unit_name"), Json("CounterUnit.Percentage"));
        QCOMPARE(rdcCounterDescription(diagnostic).at("measurable"), Json(false));
        for (const auto &ids : {Json::array(), Json::array({3000000}), Json::array({4000001}),
             Json::array({1, 1}), Json::array({-1}), Json::array({0}), Json::array({1.0}),
             Json::array({uint64_t(UINT32_MAX) + 1}), Json("1")})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectRdcCounters(catalog, ids));
        catalog.erase(builtin.counter);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectRdcCounters(catalog, nullptr));
    }
};
QTEST_GUILESS_MAIN(RdcCounterTests)
#include "RdcCounterTests.moc"
