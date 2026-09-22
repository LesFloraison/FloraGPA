#pragma once
#include "application/Experiment.h"
#include <QDialog>
class QLabel;
class QTreeWidget;
namespace flora {
class FrameRangeDialog final : public QDialog {
    Q_OBJECT
  public:
    FrameRangeDialog(const Frame &frame, const Experiment &experiment, Id currentEvent, int scope,
                     const QString &indices, QWidget *parent = nullptr);
    QString selectedIndices() const;

  private:
    void refresh();
    QTreeWidget *ranges_;
    QLabel *status_;
};
} // namespace flora
