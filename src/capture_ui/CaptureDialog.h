#pragma once
#include "CaptureClient.h"
#include "AutoDetectClient.h"
#include <QDialog>
class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QPlainTextEdit;
class QCheckBox;
namespace flora::capture_ui {
class CaptureDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit CaptureDialog(const QString &controller, QWidget *parent = nullptr);
    ~CaptureDialog() override;
  signals:
    void openCaptureRequested(const QString &path);
  protected:
    void closeEvent(QCloseEvent *event) override;
  private:
    void updateControls();
    void showResponse(const QString &command, const QJsonObject &result);
    CaptureClient client_;
    AutoDetectClient autoDetect_;
    QCheckBox* autoCheck_;
    QComboBox* sessions_;
    QLabel* autoStatus_;
    QString controller_;
    QTimer poll_;
    QLineEdit *exe_, *arguments_, *cwd_, *out_;
    QLabel *status_;
    QComboBox *swaps_;
    QComboBox *hudMode_;
    QLineEdit *hudKey_, *captureKey_;
    QPushButton *launch_, *capture_, *cancel_, *disconnect_, *open_;
    QPlainTextEdit *log_;
    bool available_ = false, activeCapture_ = false;
    QString completed_, lastResult_;
    QString lastHudRequest_;
};
}
