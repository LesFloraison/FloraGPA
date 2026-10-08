#include "GuiResourceSnapshot.h"
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
using flora::testing::GuiResourceSnapshot;
class GuiResourceTests final : public QObject {
    Q_OBJECT
  private slots:
    void hiddenWidgetIsNotMaterialized() {
        QWidget hidden;
        hidden.setObjectName("snapshot-hidden-control");
        QVERIFY(!hidden.testAttribute(Qt::WA_WState_Created));
        const auto snapshot = GuiResourceSnapshot::capture();
        QVERIFY(!hidden.testAttribute(Qt::WA_WState_Created));
        QVERIFY(snapshot["modules_complete"].get<bool>());
        QVERIFY(snapshot["native_windows_complete"].get<bool>());
        QCOMPARE(snapshot["process_id"].get<DWORD>(), GetCurrentProcessId());
        bool found = false;
        for (const auto &row : snapshot["qt_top_widgets"])
            if (row["object_name"] == "snapshot-hidden-control") { found = true; QVERIFY(!row["native_created"].get<bool>()); }
        QVERIFY(found);
    }
    void visibleWidgetIsRemoved() {
        auto widget = std::make_unique<QWidget>();
        widget->setObjectName("snapshot-visible-control"); widget->show();
        QTRY_VERIFY(widget->windowHandle() && widget->windowHandle()->isExposed());
        const auto visible = GuiResourceSnapshot::capture();
        bool found = false;
        for (const auto &row : visible["qt_top_widgets"])
            if (row["object_name"] == "snapshot-visible-control") { found = true; QVERIFY(row["visible"].get<bool>()); }
        QVERIFY(found);
        widget.reset(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto removed = GuiResourceSnapshot::capture();
        for (const auto &row : removed["qt_top_widgets"]) QVERIFY(row["object_name"] != "snapshot-visible-control");
    }
    void createdGdiObjectsAreCountedAndReleased() {
        const auto before = GuiResourceSnapshot::counts();
        std::array<HPEN, 4> pens{};
        const auto cleanup = qScopeGuard([&] { for (auto pen : pens) if (pen) DeleteObject(pen); });
        for (auto &pen : pens) { pen = CreatePen(PS_SOLID, 1, RGB(10,20,30)); QVERIFY(pen); }
        const auto during = GuiResourceSnapshot::capture();
        QCOMPARE(during["before"]["gdi_objects"].get<unsigned>(), before["gdi_objects"].get<unsigned>() + pens.size());
        QCOMPARE(during["after"]["gdi_objects"], during["before"]["gdi_objects"]);
        for (auto &pen : pens) { QVERIFY(DeleteObject(pen)); pen = nullptr; }
        QCOMPARE(GuiResourceSnapshot::counts()["gdi_objects"], before["gdi_objects"]);
    }
    void evidenceCannotBeOverwritten() {
        QTemporaryDir directory;
        const auto path = directory.filePath("snapshot.json");
        const auto summary = GuiResourceSnapshot::save(path);
        QVERIFY(summary["modules_complete"].get<bool>());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, GuiResourceSnapshot::save(path));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
    }
};
QTEST_MAIN(GuiResourceTests)
#include "GuiResourceTests.moc"
