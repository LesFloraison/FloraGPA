#pragma once
#include "Views.h"
#include "DiagnosticOutput.h"
#include <array>
#include <nlohmann/json.hpp>
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
namespace flora {
class QuadView final : public QWidget {
    Q_OBJECT
  public:
    explicit QuadView(QWidget *parent = nullptr);
    void setContext(std::shared_ptr<const Frame> frame, Id event, const QString &key,
                    std::optional<State> state, const QString &error = {});
    void restoreSettings(const nlohmann::json &ui);
    nlohmann::json settings() const;
    void setWorkerBusy(bool busy);
    bool finish(uint64_t request, const nlohmann::json &error);
    bool accept(uint64_t request, QuadOutput output);
    QString contextKey() const { return key_; }
    void exportResult(const QString &path) const;
  signals:
    void readRequested(const QString &request, qulonglong serial);
    void cancelRequested();

  private:
    void invalidate();
    void updateActions();
    void read();
    void pick(int x, int y);
    std::shared_ptr<const Frame> frame_;
    Id event_{};
    QString key_;
    uint64_t revision_{};
    bool available_{}, busy_{}, pending_{};
    nlohmann::json result_;
    std::array<QByteArray, 6> files_;
    QAction *read_, *cancel_, *export_;
    QComboBox *mode_, *target_;
    QLineEdit *layer_;
    ImageView *image_;
    QLabel *summary_, *accounting_, *cell_, *zoom_;
    QPlainTextEdit *details_;
};
} // namespace flora
