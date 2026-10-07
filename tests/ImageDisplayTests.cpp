#include "app/Views.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>
#include <atomic>
#include <array>

using namespace flora;
class ImageDisplayTests final : public QObject {
    Q_OBJECT
  private slots:
    void pixels_data() {
        QTest::addColumn<int>("format");
        QTest::newRow("rgba") << int(QImage::Format_RGBA8888);
        QTest::newRow("rgb-odd-stride") << int(QImage::Format_RGB888);
        QTest::newRow("gray") << int(QImage::Format_Grayscale8);
        QTest::newRow("premultiplied") << int(QImage::Format_ARGB32_Premultiplied);
    }
    void pixels() {
        QFETCH(int, format);
        QImage source(257, 3, QImage::Format(format));
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor((x * 17 + y) % 256, (x * 83 + y * 9) % 256,
                                                 (x * 197 + y * 47) % 256, x % 256));
        const auto original = source.copy();
        const auto expected = QPixmap::fromImage(source).toImage().convertToFormat(QImage::Format_RGBA8888);
        auto prepared = prepareImageForDisplay(source);
        QCOMPARE(source, original);
        QCOMPARE(prepared.original, original);
        ImageView view;
        view.setPreparedImage(std::move(prepared));
        QCOMPARE(view.image(), original);
        QCOMPARE(view.displayImage().convertToFormat(QImage::Format_RGBA8888), expected);
        // Publishing retains source pixels and replaces the old overlay/extent.
        QImage mask(source.size(), QImage::Format_RGBA8888);
        mask.fill(Qt::white);
        view.setOverlayMask(mask);
        QVERIFY(view.hasOverlay());
        view.setPreparedImage(prepareImageForDisplay(source));
        QVERIFY(!view.hasOverlay());
        QCOMPARE(view.sceneRect().size(), QSizeF(source.size()));
        view.setPreparedImage(prepareImageForDisplay({}));
        QVERIFY(view.image().isNull());
        QVERIFY(view.displayImage().isNull());
        QVERIFY(view.sceneRect().isEmpty());
    }
    void invalidPublicationPreservesImage() {
        QImage source(17, 9, QImage::Format_RGBA8888);
        source.fill(QColor(7, 19, 81, 117));
        ImageView view;
        view.setPreparedImage(prepareImageForDisplay(source));
        const auto displayed = view.displayImage();
        const std::array<PreparedImage, 4> invalid{{
            {source, {}}, {{}, source}, {source, source},
            {source, QImage(1, 1, QImage::Format_ARGB32_Premultiplied)}}};
        for (const auto &image : invalid) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.setPreparedImage(image));
            QCOMPARE(view.image(), source);
            QCOMPARE(view.displayImage(), displayed);
        }
    }
    void cancellationAndRetry() {
        QImage source(257, 19, QImage::Format_RGBA8888);
        source.fill(QColor(71, 199, 37, 43));
        unsigned checks = 0;
        const auto expected = prepareImageForDisplay(source, [&] { ++checks; return false; });
        QVERIFY(checks >= 3);
        for (unsigned stop = 1; stop <= checks; ++stop) {
            unsigned at = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, prepareImageForDisplay(source, [&] { return ++at == stop; }));
            const auto retry = prepareImageForDisplay(source);
            QCOMPARE(retry.original, source);
            QCOMPARE(retry.display, expected.display);
        }
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, prepareImageForDisplay({}, [] { return true; }));
        qInfo() << "Presentation cancellation/retry positions" << checks;
    }
    void backgroundCancellation() {
        QImage source(4096, 4096, QImage::Format_RGBA8888);
        source.fill(QColor(17, 53, 149, 113));
        auto gate = std::make_shared<QSemaphore>();
        auto entered = std::make_shared<std::atomic_bool>(false);
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        QFutureWatcher<bool> watcher;
        const auto mainThread = QThread::currentThread();
        watcher.setFuture(QtConcurrent::run([=] {
            try {
                prepareImageForDisplay(source, [=] {
                    if (!entered->exchange(true)) {
                        if (QThread::currentThread() == mainThread) return true;
                        gate->tryAcquire(1, 10000);
                    }
                    return cancelled->load();
                });
                return false;
            } catch (const OperationCancelled &) { return true; }
        }));
        QTRY_VERIFY_WITH_TIMEOUT(entered->load(), 10000);
        QVERIFY(!watcher.isFinished());
        bool heartbeat = false;
        QTimer::singleShot(0, &watcher, [&] {
            heartbeat = true;
            cancelled->store(true);
            gate->release();
        });
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(), 10000);
        QVERIFY(heartbeat);
        QVERIFY(watcher.result());
        QCOMPARE(prepareImageForDisplay(source).original, source);
    }
    void largePublication_data() {
        QTest::addColumn<int>("alpha");
        QTest::newRow("translucent") << 211;
        QTest::newRow("opaque") << 255;
    }
    void largePublication() {
        QFETCH(int, alpha);
        QImage source(8192, 4096, QImage::Format_RGBA8888);
        QVERIFY(!source.isNull());
        source.fill(QColor(53, 97, 179, alpha));
        ImageView view;
        QElapsedTimer timer;
        timer.start();
        view.setImage(source);
        view.channel("RGBA");
        const auto elapsed = timer.nsecsElapsed();
        QCOMPARE(view.image(), source);
        QCOMPARE(view.displayImage(), QPixmap::fromImage(source).toImage());
        qInfo() << "Legacy GUI publication ns" << elapsed << "pixels" << source.size();
        ImageView preparedView;
        timer.restart();
        auto prepared = prepareImageForDisplay(source);
        const auto preparationTime = timer.nsecsElapsed();
        timer.restart();
        preparedView.setPreparedImage(std::move(prepared));
        const auto publishTime = timer.nsecsElapsed();
        QCOMPARE(preparedView.image(), source);
        QCOMPARE(preparedView.displayImage().convertToFormat(QImage::Format_RGBA8888),
                 view.displayImage().convertToFormat(QImage::Format_RGBA8888));
        qInfo() << "Prepared conversion ns" << preparationTime << "GUI publication ns" << publishTime;
    }
};
QTEST_MAIN(ImageDisplayTests)
#include "ImageDisplayTests.moc"
