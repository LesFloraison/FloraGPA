#include "application/RenderDocLibrary.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class RenderDocLibraryTests : public QObject {
    Q_OBJECT
  private slots:
    void relocationAndOverrides() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto app = temp.path() + "/portable space 中文";
        const auto system = temp.path() + "/Program Files";
        const auto bundle = app + "/RenderDoc/renderdoc.dll";
        const auto installed = system + "/RenderDoc/renderdoc.dll";
        auto touch = [](const QString &path) {
            if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
            QFile f(path);
            return f.open(QIODevice::WriteOnly);
        };
        QVERIFY(touch(installed));
        QCOMPARE(flora::resolveRenderDocLibrary(app, {}, system), installed);
        QVERIFY(touch(bundle));
        QCOMPARE(flora::resolveRenderDocLibrary(app, {}, system), bundle);
        // A vanished preference after moving a portable directory must not
        // disable an otherwise valid bundled backend.
        QCOMPARE(flora::resolveRenderDocLibrary(app, temp.path() + "/old/renderdoc.dll", system), bundle);
        QCOMPARE(flora::resolveRenderDocLibrary(app, installed, system), installed);
        QCOMPARE(flora::resolveRenderDocLibrary(app, app + "/RenderDoc", system), bundle);
        const auto moved = temp.path() + "/moved 中文";
        QVERIFY(QDir().rename(app, moved));
        QCOMPARE(flora::resolveRenderDocLibrary(moved, bundle, system), moved + "/RenderDoc/renderdoc.dll");
        QVERIFY(QFile::remove(moved + "/RenderDoc/renderdoc.dll"));
        QCOMPARE(flora::resolveRenderDocLibrary(moved, bundle, system), installed);
    }
};
QTEST_GUILESS_MAIN(RenderDocLibraryTests)
#include "RenderDocLibraryTests.moc"
