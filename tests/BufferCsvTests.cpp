#include "cli/BufferCsv.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <Windows.h>
#include <limits>
#include <stdexcept>
using namespace flora;
namespace {
const QByteArray header = "byte_offset,hex_bytes,uint32,int32,float32\n";
QByteArray read(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read CSV fixture");
    return f.readAll();
}
void previous(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write("Previous data") != 13)
        throw std::runtime_error("Cannot save CSV fixture");
}
class Sink final : public QIODevice {
  public:
    int calls = 0, failAt = 0;
    bool shortWrite = false;
    qint64 largest = 0, total = 0;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    Sink() { open(QIODevice::WriteOnly); }
    bool isSequential() const override { return true; }
  protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override {
        ++calls; largest = std::max(largest, size);
        if (calls == failAt) return shortWrite ? size - 1 : -1;
        total += size; hash.addData(QByteArrayView(data, size)); return size;
    }
};
}
class BufferCsvTests final : public QObject {
    Q_OBJECT
  private slots:
    void format_data() {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<quint64>("offset"); QTest::addColumn<QByteArray>("rows");
        QTest::newRow("empty") << QByteArray() << quint64(0) << QByteArray();
        QTest::newRow("zero") << QByteArray::fromHex("00000000") << quint64(0) << QByteArray("0,00000000,0,0,0\n");
        QTest::newRow("negative-zero") << QByteArray::fromHex("00000080") << quint64(0) << QByteArray("0,00000080,2147483648,-2147483648,0\n");
        QTest::newRow("one") << QByteArray::fromHex("0000803f") << quint64(0) << QByteArray("0,0000803f,1065353216,1065353216,1\n");
        QTest::newRow("negative-one") << QByteArray::fromHex("000080bf") << quint64(0) << QByteArray("0,000080bf,3212836864,-1082130432,-1\n");
        QTest::newRow("inf") << QByteArray::fromHex("0000807f") << quint64(0) << QByteArray("0,0000807f,2139095040,2139095040,inf\n");
        QTest::newRow("negative-inf") << QByteArray::fromHex("000080ff") << quint64(0) << QByteArray("0,000080ff,4286578688,-8388608,-inf\n");
        QTest::newRow("nan") << QByteArray::fromHex("0000c07f") << quint64(0) << QByteArray("0,0000c07f,2143289344,2143289344,nan\n");
        QTest::newRow("subnormal") << QByteArray::fromHex("01000000") << quint64(0) << QByteArray("0,01000000,1,1,1.4012984643248171e-45\n");
        QTest::newRow("maximum-finite") << QByteArray::fromHex("ffff7f7f") << quint64(0) << QByteArray("0,ffff7f7f,2139095039,2139095039,3.4028234663852886e+38\n");
        QTest::newRow("one-byte") << QByteArray::fromHex("ff") << quint64(3) << QByteArray("3,ff,,,\n");
        QTest::newRow("two-bytes") << QByteArray::fromHex("0123") << quint64(4294967296) << QByteArray("4294967296,0123,,,\n");
        QTest::newRow("three-bytes") << QByteArray::fromHex("456789") << quint64(7) << QByteArray("7,456789,,,\n");
        QTest::newRow("maximum-offset") << QByteArray::fromHex("ff") << std::numeric_limits<quint64>::max() << QByteArray("18446744073709551615,ff,,,\n");
        QTest::newRow("multiple-tail") << QByteArray::fromHex("0000803f1122") << quint64(3) << QByteArray("3,0000803f,1065353216,1065353216,1\n7,1122,,,\n");
    }
    void format() {
        QFETCH(QByteArray, bytes); QFETCH(quint64, offset); QFETCH(QByteArray, rows);
        QBuffer out; QVERIFY(out.open(QIODevice::WriteOnly));
        writeBufferCsv(out, bytes, offset); QCOMPARE(out.data(), header + rows);
    }
    void chunks() {
        Sink sink; writeBufferCsv(sink, QByteArray(262144, 'C'), 0);
        QVERIFY(sink.calls > 2); QVERIFY(sink.largest <= 1024 * 1024);
        // Golden CSV from the preceding accepted native CLI, not this encoder.
        QCOMPARE(sink.hash.result().toHex(), QByteArray("661adc4f4975c052278cbdf9ac864a6b14735dfb51a05039731b504b79eb53bc"));
    }
    void failedWrite_data() {
        QTest::addColumn<int>("at"); QTest::addColumn<bool>("shortWrite");
        for (int at : {1, 2, 3}) for (bool shortWrite : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(at).arg(shortWrite))) << at << shortWrite;
    }
    void failedWrite() {
        QFETCH(int, at); QFETCH(bool, shortWrite);
        Sink sink; sink.failAt = at; sink.shortWrite = shortWrite;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeBufferCsv(sink, QByteArray(262144, 'C'), 0));
        QCOMPARE(sink.calls, at); QVERIFY(sink.largest <= 1024 * 1024);
    }
    void publication() {
        QTemporaryDir root; const auto path = root.filePath("words.csv");
        saveBufferCsv(path, {}, 0); QCOMPARE(read(path), header);
        previous(path); saveBufferCsv(path, QByteArray::fromHex("01"), 5);
        QCOMPARE(read(path), header + "5,01,,,\n");
        previous(path);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
            saveBufferCsv(path, QByteArray(2, 'x'), std::numeric_limits<quint64>::max()));
        QCOMPARE(read(path), QByteArray("Previous data"));
        const auto lock = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        auto release = qScopeGuard([&] { CloseHandle(lock); });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, saveBufferCsv(path, QByteArray(262144, 'C'), 0));
        QCOMPARE(read(path), QByteArray("Previous data"));
        QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"words.csv"});
        release.dismiss(); CloseHandle(lock);
        saveBufferCsv(path, {}, 0); QCOMPARE(read(path), header);
    }
    void stagingFailure() {
        QTemporaryDir root; const auto path = root.filePath("directory");
        QVERIFY(QDir().mkpath(path)); previous(path + "/sentinel");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, saveBufferCsv(path, {}, 0));
        QCOMPARE(read(path + "/sentinel"), QByteArray("Previous data"));
        saveBufferCsv(root.filePath("retry.csv"), {}, 0); QCOMPARE(read(root.filePath("retry.csv")), header);
    }
};
QTEST_GUILESS_MAIN(BufferCsvTests)
#include "BufferCsvTests.moc"
