#include "app/Views.h"
#include <QJsonObject>
#include <QSignalSpy>
#include <QToolTip>
#include <QWheelEvent>
#include <QtTest>

using flora::EventChart;
namespace {
constexpr qulonglong baseId = 9007199254740993ULL;
QJsonArray rows(int count, bool zero = false) {
    QJsonArray result;
    for (int i = 0; i < count; ++i)
        result.append(QJsonObject{{"event", QString::number(baseId + 17 * i)},
            {"microseconds", zero ? 0 : i == 0 ? 1000 : 5}, {"command", "DrawInstanced"}});
    return result;
}
void wheel(EventChart &chart, QPointF position, int delta, bool pixel = false) {
    QWheelEvent e(position, chart.mapToGlobal(position.toPoint()), pixel ? QPoint(0, delta) : QPoint{},
        pixel ? QPoint{} : QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&chart, &e);
}
}
class EventChartTests : public QObject {
    Q_OBJECT
  private slots:
    void overviewDrag() {
        EventChart chart; chart.resize(1070, 260); chart.setTimings(rows(100)); chart.show();
        wheel(chart, {548, 80}, 12000);
        QSignalSpy selected(&chart, &EventChart::eventSelected);
        const auto move = [&](QPointF position, Qt::MouseButtons buttons) {
            QMouseEvent e(QEvent::MouseMove, position, chart.mapToGlobal(position), Qt::NoButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(&chart, &e);
        };
        QTest::mousePress(&chart, Qt::LeftButton, Qt::NoModifier, {548, 244});
        move({748, 244}, Qt::LeftButton);
        QCOMPARE(chart.visibleBegin(), 60); QCOMPARE(chart.visibleCount(), 20);
        QCOMPARE(chart.cursor().shape(), Qt::ClosedHandCursor);
        wheel(chart, {548, 80}, -120); QCOMPARE(chart.visibleCount(), 20);
        move({-1000, 80}, Qt::LeftButton); QCOMPARE(chart.visibleBegin(), 0);
        move({3000, 80}, Qt::LeftButton); QCOMPARE(chart.visibleBegin(), 80);
        QTest::mouseRelease(&chart, Qt::LeftButton, Qt::NoModifier, {1000, 244});
        move({548, 244}, Qt::NoButton); QCOMPARE(chart.visibleBegin(), 80);
        QVERIFY(selected.empty());
        // Clicking outside the range cannot start a drag or jump the range.
        QTest::mousePress(&chart, Qt::LeftButton, Qt::NoModifier, {148, 244});
        move({548, 244}, Qt::LeftButton); QCOMPARE(chart.visibleBegin(), 80);
        QTest::mouseRelease(&chart, Qt::LeftButton, Qt::NoModifier, {548, 244});
        QTest::mousePress(&chart, Qt::LeftButton, Qt::NoModifier, {948, 244});
        QEvent lostCapture(QEvent::UngrabMouse); QApplication::sendEvent(&chart, &lostCapture);
        move({548, 244}, Qt::LeftButton); QCOMPARE(chart.visibleBegin(), 80);
        QTest::mouseRelease(&chart, Qt::LeftButton, Qt::NoModifier, {548, 244});
        QTest::mousePress(&chart, Qt::LeftButton, Qt::NoModifier, {948, 244});
        chart.setTimings(rows(10));
        move({548, 244}, Qt::LeftButton); QCOMPARE(chart.visibleBegin(), 0); QCOMPARE(chart.visibleCount(), 10);
        QTest::mouseRelease(&chart, Qt::LeftButton, Qt::NoModifier, {548, 244});
        QVERIFY(selected.empty());
    }
    void zoomPanAndScale() {
        EventChart chart; chart.resize(1070, 260); chart.setTimings(rows(100));
        QSignalSpy selected(&chart, &EventChart::eventSelected);
        QCOMPARE(chart.visibleMaximumUs(), 1080.0);
        wheel(chart, {548, 80}, 120);
        QCOMPARE(chart.visibleBegin(), 10); QCOMPARE(chart.visibleCount(), 80);
        QCOMPARE(chart.visibleMaximumUs(), 5.4);
        // The event coordinate under the mouse remains anchored at ordinal 50.
        QCOMPARE(chart.visibleBegin() + chart.visibleCount() / 2, 50);
        wheel(chart, {548, 244}, -120);
        QCOMPARE(chart.visibleBegin(), 18); QCOMPARE(chart.visibleCount(), 80);
        wheel(chart, {548, 244}, -12000);
        QCOMPARE(chart.visibleBegin(), 20);
        wheel(chart, {548, 244}, 12000);
        QCOMPARE(chart.visibleBegin(), 0); QCOMPARE(chart.visibleMaximumUs(), 1080.0);
        wheel(chart, {298, 80}, 120);
        QCOMPARE(chart.visibleBegin(), 4); QCOMPARE(chart.visibleCount(), 64);
        wheel(chart, {298, 80}, 12000);
        QCOMPARE(chart.visibleCount(), 20);
        wheel(chart, {548, 244}, -12000);
        QCOMPARE(chart.visibleBegin(), 80);
        wheel(chart, {548, 80}, -12000);
        QCOMPARE(chart.visibleBegin(), 0); QCOMPARE(chart.visibleCount(), 100);
        QVERIFY(selected.empty());
    }
    void selectionAndTooltip() {
        EventChart chart; chart.resize(1070, 260); chart.setTimings(rows(100)); chart.show();
        wheel(chart, {548, 80}, 12000);
        QCOMPARE(chart.visibleCount(), 20);
        QCOMPARE(chart.visibleBegin(), 40);
        QSignalSpy selected(&chart, &EventChart::eventSelected);
        QTest::mouseClick(&chart, Qt::LeftButton, Qt::NoModifier, {573, 80});
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected[0][0].toULongLong(), baseId + 17 * 50);
        QMouseEvent move(QEvent::MouseMove, QPointF(573, 80), chart.mapToGlobal(QPoint(573, 80)),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&chart, &move);
        QVERIFY(QToolTip::text().contains(QString::number(baseId + 17 * 50)));
        QVERIFY(QToolTip::text().contains("5.000"));
        QTest::mouseClick(&chart, Qt::LeftButton, Qt::NoModifier, {573, 244});
        QTest::mouseClick(&chart, Qt::LeftButton, Qt::NoModifier, {573, 205});
        QTest::mouseClick(&chart, Qt::RightButton, Qt::NoModifier, {573, 80});
        QCOMPARE(selected.size(), 1);
        const auto first = chart.visibleBegin(), count = chart.visibleCount();
        chart.resize(1470, 350); chart.grab();
        QCOMPARE(chart.visibleBegin(), first); QCOMPARE(chart.visibleCount(), count);
    }
    void smallEmptyAndHighResolution() {
        EventChart chart; chart.resize(1070, 260);
        for (int count : {0, 1, 19, 20}) {
            chart.setTimings(rows(count, true));
            wheel(chart, {548, 80}, 12000); wheel(chart, {548, 244}, -12000);
            QCOMPARE(chart.visibleCount(), count); QCOMPARE(chart.visibleBegin(), 0);
            QCOMPARE(chart.visibleMaximumUs(), 1.08);
            QVERIFY(!chart.grab().isNull());
        }
        chart.setTimings(rows(100));
        wheel(chart, {548, 80}, 60); QCOMPARE(chart.visibleCount(), 100);
        wheel(chart, {548, 80}, 60); QCOMPARE(chart.visibleCount(), 80);
        wheel(chart, {548, 80}, 20, true); QCOMPARE(chart.visibleCount(), 80);
        wheel(chart, {548, 80}, 20, true); QCOMPARE(chart.visibleCount(), 64);
        wheel(chart, {5, 80}, 120); QCOMPARE(chart.visibleCount(), 64);
        chart.clear(); QCOMPARE(chart.visibleCount(), 0); QCOMPARE(chart.visibleBegin(), 0);
        chart.setTimings(rows(30)); QCOMPARE(chart.visibleCount(), 30); QCOMPARE(chart.visibleBegin(), 0);
    }
};
QTEST_MAIN(EventChartTests)
#include "EventChartTests.moc"
