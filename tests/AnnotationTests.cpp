#include "SyntheticCapture.h"
#include "TextureEditCapture.h"
#include "app/AnnotationsView.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "application/Annotations.h"
#include "core/Commands.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
template <class... T> std::vector<uint8_t> pack(T... args) {
    std::vector<uint8_t> out;
    (append(out, args), ...);
    return out;
}
void marker(Capture &c, Id id, uint16_t kind, Id owner, const QString &name = {}, int level = 0) {
    auto bytes = pack(Id(0), owner);
    if (kind != 0x327d)
        append(bytes, int32_t(level));
    if (kind != 0x327c) {
        append(bytes, uint32_t((name.size() + 1) * 2));
        for (auto ch : name)
            append(bytes, ch.unicode());
        append(bytes, uint16_t(0));
    }
    c.add(id, 7, kind, bytes);
}
Capture fixture(bool identity = true, bool end = true) {
    Capture c;
    c.add(1, 5, 0x99, std::vector<uint8_t>(24));
    c.add(9, 3, 3, std::vector<uint8_t>(22320));
    if (identity) {
        auto raw = pack(Id(0), Id(600), int32_t(0));
        auto guid = QByteArray::fromHex("6ca9bfc089e0fb448eaf26f8796190da");
        raw.insert(raw.end(), guid.begin(), guid.end());
        append(raw, Id(1));
        c.add(10, 7, 0x3278, raw);
    }
    marker(c, 20, 0x327b, 600, "Main 🌿");
    marker(c, 25, 0x327d, 600, "Nested marker");
    c.add(30, 7, 0x37, pack(Id(9), Id(0), Id(1), uint32_t(3), uint32_t(0)));
    if (end)
        marker(c, 40, 0x327c, 600);
    return c;
}
QTreeWidgetItem *find(QTreeWidget *view, Id id) {
    QTreeWidgetItemIterator it(view);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole).toULongLong() == id)
            return *it;
        ++it;
    }
    return nullptr;
}
} // namespace
class AnnotationTests final : public QObject {
    Q_OBJECT
  private slots:
    void replayAndMalformedWire() {
        QTemporaryDir dir;
        auto c = fixture();
        // Only annotations are replayed; annotation identities are not GPU objects.
        Capture records;
        for (const auto &entry : c.entries)
            if (entry.type >= 0x3278 && entry.type <= 0x327e) {
                std::vector<uint8_t> raw(c.bytes.begin() + entry.offset,
                                         c.bytes.begin() + entry.offset + entry.size);
                records.add(entry.id, 7, entry.type, raw);
                validateAnnotationCommand(entry.type, raw);
                for (size_t size = 0; size < raw.size(); ++size)
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                             validateAnnotationCommand(entry.type, Bytes(raw.data(), size)));
                raw.push_back(0);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateAnnotationCommand(entry.type, raw));
            }
        for (uint16_t type : {0x3279, 0x327a, 0x327e}) {
            const auto raw = pack(Id(0), Id(600), uint32_t(0xffffffff));
            records.add(type, 7, type, raw);
            for (size_t size = 0; size < raw.size(); ++size)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         validateAnnotationCommand(type, Bytes(raw.data(), size)));
        }
        records.save(dir.path() + "/replay.gpa_frame");
        Frame frame((dir.path() + "/replay.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.counts.at("annotation_records"), 7ull);
        replay.run();
        QCOMPARE(replay.counts.at("annotation_records"), 7ull);
        options.until = 25;
        options.before = true;
        Replay before(frame, options);
        before.run();
        QCOMPARE(before.counts.at("annotation_records"), 2ull);
    }
    void hierarchyAndIdentity() {
        QTemporaryDir dir;
        auto c = fixture();
        c.save(dir.path() + "/capture.gpa_frame");
        Frame frame((dir.path() + "/capture.gpa_frame").toStdWString());
        auto r = inspectAnnotations(frame);
        const auto &n = r["nodes"][0];
        QCOMPARE(n["end"], Json(40));
        QCOMPARE(n["interval_command_count"], Json(4));
        QCOMPARE(n["context_ids"], Json::array({1}));
        QCOMPARE(n["draws"], Json::array({{{"id", 30}, {"name", "Draw"}, {"context", 1}}}));
        QCOMPARE(r["nodes"][1]["parent"], Json(20));
        QVERIFY(r["issues"].empty());
        exportAnnotations(r, (dir.path() + "/export").toStdWString());
        QFile f(dir.path() + "/export/annotations.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(Json::parse(f.readAll().toStdString()), r);
    }
    void interruptions() {
        QTemporaryDir dir;
        auto c = fixture(true, false);
        c.add(35, 7, 0x327a, pack(Id(0), Id(0), uint32_t(0)));
        marker(c, 40, 0x327c, 600);
        c.save(dir.path() + "/release.gpa_frame");
        Frame frame((dir.path() + "/release.gpa_frame").toStdWString());
        auto r = inspectAnnotations(frame);
        QCOMPARE(r["nodes"][0]["status"], Json("interrupted_by_unknown_owner"));
        QVERIFY(r["nodes"][0]["draws"].empty());
        QCOMPARE(r["nodes"][0]["identity_segment"]["end"], Json(35));
        QCOMPARE(r["nodes"].back()["status"], Json("unmatched_end"));
    }
    void viewNavigationAndReset() {
        QTemporaryDir dir;
        auto c = fixture();
        const auto path = dir.path() + "/capture.gpa_frame";
        c.save(path);
        auto frame = std::make_shared<Frame>(path.toStdWString());
        AnnotationsView view;
        view.resize(1000, 650);
        view.show();
        QSignalSpy done(&view, &AnnotationsView::inspectionFinished),
            events(&view, &AnnotationsView::eventRequested);
        auto read = view.findChild<QAction *>("readAnnotations");
        auto end = view.findChild<QAction *>("locateAnnotationEnd");
        auto draw = view.findChild<QAction *>("locateAnnotationDraw");
        auto tree = view.findChild<QTreeWidget *>("annotationNodes");
        auto members = view.findChild<QTreeWidget *>("annotationMembers");
        auto filter = view.findChild<QLineEdit *>("annotationFilter");
        view.setFrame(frame);
        read->trigger();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.takeFirst()[0].toBool());
        auto node = find(tree, 20);
        QVERIFY(node);
        tree->setCurrentItem(node);
        QCOMPARE(members->topLevelItemCount(), 1);
        QVERIFY(end->isEnabled());
        end->trigger();
        QCOMPARE(events.takeFirst()[0].toULongLong(), 40ull);
        members->setCurrentItem(members->topLevelItem(0));
        draw->trigger();
        QCOMPARE(events.takeFirst()[0].toULongLong(), 30ull);
        view.setWorkerBusy(true);
        QVERIFY(!end->isEnabled());
        QVERIFY(!draw->isEnabled());
        draw->trigger();
        QVERIFY(events.empty());
        view.setWorkerBusy(false);
        filter->setText("Nested 600");
        QVERIFY(find(tree, 20));
        QVERIFY(find(tree, 25));
        QVERIFY(!find(tree, 40));
        QCOMPARE(members->topLevelItemCount(), 0);
        const auto destination = dir.path() + "/filtered-export";
        QVERIFY(QDir().mkpath(destination));
        bool exported = false;
        QTimer::singleShot(0, &view, [&] {
            auto dialog = view.findChild<QFileDialog *>();
            if (dialog) {
                dialog->selectFile(destination);
                exported = QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
        });
        view.findChild<QAction *>("exportAnnotations")->trigger();
        QVERIFY(exported);
        QFile exportFile(destination + "/annotations.json");
        QVERIFY(exportFile.open(QIODevice::ReadOnly));
        auto document = Json::parse(exportFile.readAll().toStdString());
        QCOMPARE(document.at("nodes").size(), size_t(3));
        QCOMPARE(document.at("context_links").size(), size_t(1));
        filter->clear();
        tree->setCurrentItem(find(tree, 20));
        auto evidence = qEnvironmentVariable("FLORA_ANNOTATION_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(view.grab().save(evidence + "/annotations.png"));
        }
        auto other = fixture(false, false);
        other.save(dir.path() + "/other.gpa_frame");
        view.setFrame(std::make_shared<Frame>((dir.path() + "/other.gpa_frame").toStdWString()));
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(!end->isEnabled());
        read->trigger();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.takeFirst()[0].toBool());
        tree->setCurrentItem(find(tree, 20));
        QCOMPARE(members->topLevelItemCount(), 0);
        QVERIFY(!end->isEnabled());
        QCOMPARE(view.findChild<QLabel *>("annotationMembership")->text(), QString("Membership unresolved"));
        view.setFrame(frame);
        read->trigger();
        view.setFrame(nullptr);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done.takeFirst()[0].toBool());
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(!read->isEnabled());
    }
    void mainWindowNavigation() {
        QTemporaryDir dir;
        auto c = textureEditCapture(28, 1, true);
        auto identity = fixture();
        for (const auto &entry : identity.entries)
            if (entry.type == 0x3278)
                c.add(70, 7, entry.type,
                      std::vector<uint8_t>(identity.bytes.begin() + entry.offset,
                                           identity.bytes.begin() + entry.offset + entry.size));
        marker(c, 80, 0x327b, 600, "Frame pass");
        marker(c, 85, 0x327d, 600, "Draw inputs");
        marker(c, 220, 0x327c, 600);
        const auto path = dir.path() + "/annotations.gpa_frame";
        c.save(path);
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        window.findChild<QAction *>("showAnnotations")->trigger();
        auto view = window.findChild<AnnotationsView *>();
        QVERIFY(view->isVisible());
        QSignalSpy done(view, &AnnotationsView::inspectionFinished);
        view->findChild<QAction *>("readAnnotations")->trigger();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.takeFirst()[0].toBool());
        auto tree = view->findChild<QTreeWidget *>("annotationNodes");
        tree->setCurrentItem(find(tree, 80));
        auto members = view->findChild<QTreeWidget *>("annotationMembers");
        QCOMPARE(members->topLevelItemCount(), 2);
        auto evidence = qEnvironmentVariable("FLORA_ANNOTATION_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(window.grab().save(evidence + "/annotations-main-window.png"));
        }
        view->findChild<QAction *>("locateAnnotationEnd")->trigger();
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), 220ull);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        members->setCurrentItem(members->topLevelItem(0));
        view->findChild<QAction *>("locateAnnotationDraw")->trigger();
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), 100ull);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("FloraGPA-AnnotationTests");
    app.setApplicationName("FloraGPA-AnnotationTests");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    applyAppearance(app);
    AnnotationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "AnnotationTests.moc"
