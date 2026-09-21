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
        QCOMPARE(j.size(), size_t(8));
        for (unsigned i = 0; i < 9; ++i)
            QVERIFY(rdcCounterUnit(CounterUnit(i)).starts_with("CounterUnit."));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcCounterUnit(CounterUnit(99)));
    }
};
QTEST_GUILESS_MAIN(RdcCounterTests)
#include "RdcCounterTests.moc"
