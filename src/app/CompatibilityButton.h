#pragma once
#include <QPointer>
#include <QPushButton>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
class QDialog;
class QTreeWidget;
class QLabel;
namespace flora {
class CompatibilityButton final : public QPushButton {
    Q_OBJECT
  public:
    explicit CompatibilityButton(QWidget *parent = nullptr);
    ~CompatibilityButton() override;
    void setCapture(const QString &path);
    const nlohmann::json &report() const { return report_; }
  signals:
    void resultReady();

  private:
    void showReport();
    void run();
    void refresh();
    QString path_;
    nlohmann::json report_;
    uint64_t serial_{};
    std::shared_ptr<std::atomic_bool> cancel_;
    QPointer<QDialog> dialog_;
    QTreeWidget *findings_{};
    QLabel *summary_{};
    QPushButton *retry_{}, *cancelButton_{}, *export_{};
};
} // namespace flora
