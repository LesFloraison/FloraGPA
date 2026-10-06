#include "QtCompatibility.h"
#include <QActionEvent>
#include <QApplication>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QToolButton>

namespace flora {
namespace {
class ToolButtonConnectionGuard final : public QObject {
    QPointer<QWidget> root_;
    QSet<QToolButton *> pending_;
  public:
    explicit ToolButtonConnectionGuard(QWidget *root) : QObject(root), root_(root) {
        setObjectName("qtToolButtonConnectionGuard");
        qApp->installEventFilter(this);
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::ActionChanged) {
            auto button = qobject_cast<QToolButton *>(object);
            if (button && root_ && (button == root_ || root_->isAncestorOf(button))) {
                auto action = static_cast<QActionEvent *>(event)->action();
                if (action && action == button->defaultAction() && !pending_.contains(button)) {
                    pending_.insert(button);
                    // Let QAction::changed and Qt's automatic menu handling finish.
                    // Coalesce updates into one cleanup per button/event-loop turn.
                    QTimer::singleShot(0, this, [this, button, alive = QPointer<QToolButton>(button)] {
                        pending_.remove(button);
                        if (!alive || !root_ || !(alive == root_ || root_->isAncestorOf(alive))) return;
                        auto current = alive->defaultAction();
                        if (!current) return;
                        // Qt 6.11.2 disconnects only its specific private changed
                        // handler here; triggered and application slots survive.
                        alive->setDefaultAction(nullptr);
                        alive->setDefaultAction(current);
                    });
                }
            }
        }
        return QObject::eventFilter(object, event);
    }
};
}
void installToolButtonConnectionGuard(QWidget *root) {
    if (root && QByteArrayView(qVersion()) == "6.11.2" &&
        !root->findChild<QObject *>("qtToolButtonConnectionGuard", Qt::FindDirectChildrenOnly))
        new ToolButtonConnectionGuard(root);
}
} // namespace flora
