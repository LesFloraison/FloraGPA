#include "application/Geometry.h"
#include <QtTest>
using namespace flora;
class GeometryTests final : public QObject {
    Q_OBJECT
  private slots:
    void formats() {
        const std::vector<uint8_t> half{0, 0x3c, 0, 0xbc, 0, 0x7c, 0, 0x7e};
        auto values = vertexValue(half, 0, 10);
        QCOMPARE(values[0].get<double>(), 1.);
        QCOMPARE(values[1].get<double>(), -1.);
        QCOMPARE(values[2].get<std::string>(), std::string("inf"));
        QCOMPARE(values[3].get<std::string>(), std::string("nan"));
        const std::vector<uint8_t> snorm{0x80, 0x7f, 0, 0xff};
        auto signedValues = vertexValue(snorm, 0, 31);
        QCOMPARE(signedValues[0].get<double>(), -1.);
        QCOMPARE(signedValues[1].get<double>(), 1.);
        const std::vector<uint8_t> packed{0xff, 0xff, 0xff, 0xff};
        QCOMPARE(vertexValue(packed, 0, 42)[0].get<uint64_t>(), uint64_t(UINT32_MAX));
        QCOMPARE(vertexValue(packed, 0, 24), nlohmann::json({1., 1., 1., 1.}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vertexValue(packed, UINT64_MAX, 42));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vertexValue(packed, 1, 42));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vertexValue(packed, 0, 0));
    }
    void stripAndAdjacency() {
        auto result = meshPrimitives({1, 2, 3, 4, 0, 5, 6, 7}, 5);
        QCOMPARE(result["faces"], nlohmann::json({{1, 2, 3}, {3, 2, 4}, {5, 6, 7}}));
        QCOMPARE(meshPrimitives({1, 2, 3, 4, 5, 6}, 12)["faces"], nlohmann::json({{1, 3, 5}}));
        QCOMPARE(meshPrimitives({1, 2, 3, 4}, 10)["lines"], nlohmann::json({{2, 3}}));
        QVERIFY(meshPrimitives({1, 1, 2}, 5)["faces"].empty());
    }
};
QTEST_GUILESS_MAIN(GeometryTests)
#include "GeometryTests.moc"
