#pragma once
#include "Views.h"
#include <array>
#include <nlohmann/json.hpp>
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
namespace flora {
class CoverageView final : public QWidget {
    Q_OBJECT
  public:
    explicit CoverageView(QWidget *parent = nullptr);
    void setContext(std::shared_ptr<const Frame> frame, Id event, const QString &key,
                    std::optional<State> state, const QString &error = {});
    void setWorkerBusy(bool busy);
    bool finish(uint64_t request, const nlohmann::json &result, const QString &directory = {});
    void exportResult(const QString &path) const;
    void setEmbedded();
    void captureTarget(const QString &target, uint32_t layer);
    nlohmann::json settings() const;
    const nlohmann::json &result() const { return result_; }
    QImage mask() const { return QImage::fromData(files_[1], "PNG"); }
    QImage diagnostic() const { return QImage::fromData(files_[3], "PNG"); }
  signals:
    void readRequested(const QString &request, qulonglong serial);
    void cancelRequested();
    void pixelRequested(const QString &report, int x, int y, const QColor &color);
    void settingsChanged();
    void diagnosticRequested();

  private:
    void invalidate();
    void updateActions();
    void read();
    std::shared_ptr<const Frame> frame_;
    Id event_{};
    QString key_;
    uint64_t revision_{};
    bool available_{}, busy_{}, pending_{};
    nlohmann::json result_;
    std::array<QByteArray, 4> files_;
    QAction *read_, *cancel_, *export_;
    QComboBox *mode_, *target_;
    QLineEdit *layer_;
    QCheckBox *depth_;
    ImageView *image_;
    QLabel *summary_, *pixel_, *zoom_;
};
} // namespace flora
