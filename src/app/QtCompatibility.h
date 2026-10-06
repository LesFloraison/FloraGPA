#pragma once
class QWidget;
namespace flora {
// Scope the Qt 6.11.2 action-refresh workaround to this widget and descendants.
void installToolButtonConnectionGuard(QWidget *root);
}
