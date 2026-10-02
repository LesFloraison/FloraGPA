#include "SyntheticCapture.h"
#include "app/CompatibilityButton.h"
#include <QDialog>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class CompatibilityUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void failureThenNewCapture() {
        CompatibilityButton button;
        button.show();
        QTemporaryDir dir;
        button.setCapture(dir.filePath("missing.gpa_frame"));
        QSignalSpy ready(&button, &CompatibilityButton::resultReady);
        button.click();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QCOMPARE(button.report()["status"], nlohmann::json("blocked"));
        QVERIFY(button.findChild<QTreeWidget *>("compatibilityFindings")->topLevelItemCount() > 0);
        Capture c;
        c.buffer(1, 2, 0, 0, {1, 2, 3, 4});
        c.save(dir.filePath("ok.gpa_frame"));
        button.setCapture(dir.filePath("ok.gpa_frame"));
        button.click();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 10000);
        QCOMPARE(button.report()["status"], nlohmann::json("checked"));
        QVERIFY(button.findChild<QDialog *>("compatibilityDialog"));
    }
    void staleResultAndCancellation() {
        CompatibilityButton button;
        QTemporaryDir dir;
        Capture c;
        for (unsigned i = 1; i <= 5000; ++i)
            c.add(i, 9, 1, word(0));
        c.save(dir.filePath("large.gpa_frame"));
        button.setCapture(dir.filePath("large.gpa_frame"));
        button.click();
        button.setCapture(dir.filePath("absent.gpa_frame"));
        QSignalSpy ready(&button, &CompatibilityButton::resultReady);
        button.click();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QCOMPARE(button.report()["status"], nlohmann::json("blocked"));
        button.setCapture(dir.filePath("large.gpa_frame"));
        button.click();
        button.findChild<QPushButton *>("compatibilityCancel")->click();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 10000);
        QCOMPARE(button.report()["status"], nlohmann::json("cancelled"));
        button.findChild<QPushButton *>("compatibilityRetry")->click();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 3, 10000);
        QCOMPARE(button.report()["status"], nlohmann::json("checked"));
    }
};
QTEST_MAIN(CompatibilityUiTests)
#include "CompatibilityUiTests.moc"
