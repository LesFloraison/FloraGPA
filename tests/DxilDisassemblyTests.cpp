#include "application/DxilDisassembly.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace flora;
class DxilDisassemblyTests : public QObject {
    Q_OBJECT
  private slots:
    void actualShadersAndFailures() {
        const auto runtime = qEnvironmentVariable("FLORA_DXC_LIBRARY");
        const auto reports = qEnvironmentVariable("FLORA_DXIL_EXPORTS");
        if (runtime.isEmpty() || reports.isEmpty())
            QSKIP("Set pinned DXC and exported Hellblade2 shader directory");
        const auto path = std::filesystem::path(runtime.toStdWString());
        for (const auto &name : {QString("restored"), QString("modified")}) {
            QFile file(reports + '/' + name + "/shader.dxil");
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            const auto result = disassembleDxil(
                {reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())}, path);
            QVERIFY(result.text.find("dx.op.textureStore.f32") != std::string::npos);
            QVERIFY(result.text.find(name == "restored" ? "define void @MainCS" : "define void @main") !=
                    std::string::npos);
            if (name == "restored")
                QVERIFY(result.text.find("dx.op.waveReadLaneAt") != std::string::npos);
            QCOMPARE(result.backend.at("release"), nlohmann::json("v1.8.2505.1"));
            QCOMPARE(result.backend.at("gpu_commands_executed"), nlohmann::json(false));
        }
        const std::vector<uint8_t> malformed{1, 2, 3, 4};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, disassembleDxil(malformed, path));
        QTemporaryDir temp;
        const auto missing = std::filesystem::path(temp.filePath("dxcompiler.dll").toStdWString());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, disassembleDxil(malformed, missing));
        QFile wrong(QString::fromStdWString(missing.wstring()));
        QVERIFY(wrong.open(QIODevice::WriteOnly));
        QCOMPARE(wrong.write("wrong runtime"), qint64(13));
        wrong.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, disassembleDxil(malformed, missing));
    }
};
QTEST_GUILESS_MAIN(DxilDisassemblyTests)
#include "DxilDisassemblyTests.moc"
