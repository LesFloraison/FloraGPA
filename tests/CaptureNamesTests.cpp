#include "application/CaptureNames.h"
#include <QtTest>
class CaptureNamesTests final : public QObject {
    Q_OBJECT
  private slots:
    void byteNames() {
        std::vector<uint8_t> bytes{'a', 0, 0xff, 0xe4, 0xb8, 0xad, 127, 0};
        QCOMPARE(flora::debugDisplayName(bytes), QString::fromUtf8("a\\x00\\xff中\\x7f"));
        std::vector<uint8_t> invalid{0xc0, 0x80, 0xed, 0xa0, 0x80, 0xf4, 0x90, 0x80, 0x80};
        QCOMPARE(flora::debugDisplayName(invalid), QString("\\xc0\\x80\\xed\\xa0\\x80\\xf4\\x90\\x80\\x80"));
    }
    void capturedCatalog() {
        auto directory = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (directory.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::Frame frame(std::filesystem::path(
            (directory + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString()));
        auto names = flora::capturedNames(frame);
        QVERIFY(names["issues"].empty());
        bool found = false;
        for (auto &record : names["records"])
            if (record["resource_id"] == "104" && record["name"] == "ConstantBuffer-208-176")
                found = true;
        QVERIFY(found);
    }
};
QTEST_GUILESS_MAIN(CaptureNamesTests)
#include "CaptureNamesTests.moc"
