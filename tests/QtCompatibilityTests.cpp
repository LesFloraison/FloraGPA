#include "app/QtCompatibility.h"
#include <QAction>
#include <QMenu>
#include <QSignalSpy>
#include <QToolBar>
#include <QToolButton>
#include <QtTest>

class CountedAction : public QAction {
  public:
    using QAction::QAction;
    int changedConnections() const { return receivers(SIGNAL(changed())); }
};
class QtCompatibilityTests final : public QObject {
    Q_OBJECT
  private slots:
    void unguardedCounterexample() {
        if (QByteArrayView(qVersion()) != "6.11.2") QSKIP("Pinned Qt 6.11.2 counterexample");
        CountedAction action("Run");
        QToolButton button;
        button.setDefaultAction(&action);
        const auto initial = action.changedConnections();
        for (int i = 0; i < 100; ++i) { action.setEnabled(false); action.setEnabled(true); }
        QCOMPARE(action.changedConnections(), initial + 200);
    }
    void boundedConnectionsAndActionState() {
        QWidget root;
        flora::installToolButtonConnectionGuard(&root);
        flora::installToolButtonConnectionGuard(&root);
        CountedAction action("Run");
        QToolButton button(&root);
        button.setDefaultAction(&action);
        QSignalSpy triggered(&action, &QAction::triggered), changed(&action, &QAction::changed);
        action.setCheckable(true);
        QCoreApplication::processEvents();
        const auto warm = action.changedConnections();
        for (int i = 0; i < 1000; ++i) {
            action.setEnabled(false); QVERIFY(!button.isEnabled());
            action.setEnabled(true); QVERIFY(button.isEnabled());
            action.setText(QString::number(i)); QCOMPARE(button.text(), action.text());
            action.setToolTip(QString("Tip %1").arg(i)); QCOMPARE(button.toolTip(), action.toolTip());
            action.setChecked(i % 2); QCOMPARE(button.isChecked(), action.isChecked());
        }
        QCoreApplication::processEvents();
        QCOMPARE(action.changedConnections(), warm);
        QVERIFY(changed.size() >= 4000); // User observers are retained.
        button.click();
        QCOMPARE(triggered.size(), 1);
        QCOMPARE(button.isChecked(), action.isChecked());
    }
    void dynamicMenusAndExplicitPopupModes() {
        QWidget root;
        flora::installToolButtonConnectionGuard(&root);
        CountedAction action("Run"), next("Next");
        QToolBar bar(&root);
        bar.addAction(&action);
        auto button = qobject_cast<QToolButton *>(bar.widgetForAction(&action));
        QVERIFY(button);
        QCOMPARE(button->popupMode(), QToolButton::DelayedPopup);
        action.setEnabled(false); action.setEnabled(true);
        QMenu menu;
        menu.addAction("Option");
        action.setMenu(&menu);
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);
        QCoreApplication::processEvents();
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);
        button->setPopupMode(QToolButton::InstantPopup);
        action.setText("Changed");
        QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
        button->setDefaultAction(&next);
        QSignalSpy fired(&next, &QAction::triggered);
        action.setEnabled(false);
        QVERIFY(button->isEnabled());
        QCOMPARE(button->defaultAction(), &next);
        button->setPopupMode(QToolButton::DelayedPopup);
        next.setChecked(false);
        button->click();
        QCOMPARE(fired.size(), 1);
        button->setDefaultAction(nullptr);
        next.setEnabled(false);
        QVERIFY(!button->defaultAction());
    }
    void scopeAndDestroyedWidgets() {
        QWidget outside;
        auto root = std::make_unique<QWidget>();
        flora::installToolButtonConnectionGuard(root.get());
        CountedAction inside("Inside"), other("Other");
        auto button = new QToolButton(root.get());
        button->setDefaultAction(&inside);
        QToolButton unrelated(&outside);
        unrelated.setDefaultAction(&other);
        inside.setEnabled(false); inside.setEnabled(true);
        QCoreApplication::processEvents();
        const auto n = inside.changedConnections(), old = other.changedConnections();
        for (int i = 0; i < 10; ++i) { inside.setText(QString::number(i)); other.setText(QString::number(i)); }
        QCoreApplication::processEvents();
        QCOMPARE(inside.changedConnections(), n);
        if (QByteArrayView(qVersion()) == "6.11.2") QCOMPARE(other.changedConnections(), old + 10);
        inside.setText("Queued at destruction");
        root.reset();
        QCoreApplication::processEvents();
        QCOMPARE(inside.changedConnections(), 0);
        inside.setEnabled(false); other.setEnabled(false);
    }
};
QTEST_MAIN(QtCompatibilityTests)
#include "QtCompatibilityTests.moc"

