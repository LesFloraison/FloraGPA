#pragma once
#include "application/NativeDebugConfig.h"
#include <QWidget>

class QComboBox;
class QLineEdit;
class QTreeWidget;
namespace flora {
// Shared compact breakpoint/watch panel for the native checkpoint view.
class NativeDebugControls final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit NativeDebugControls(QWidget *parent = nullptr);
    void reset(Json result, Json rows, SourceTrace::ValueLoader loader, bool retain = false);
    NativeDebugSettings &settings();
    Json configuration() const;
    void importConfiguration(const Json &config);
    void setSourcePoint(int64_t file, int64_t line);
    void setRecord(std::optional<size_t> record, std::optional<std::string> frame = {});
    void refresh();
    const Json &watchResults() const { return watchResults_; }
  signals:
    void error(const QString &message);
    void rulesChanged();
    void sourcePointSelected(qlonglong file, qlonglong line);

  private:
    Json result_, rows_, points_ = Json::array(), watchResults_ = Json::array();
    SourceTrace::ValueLoader loader_;
    std::unique_ptr<NativeDebugSettings> settings_;
    std::optional<size_t> record_;
    std::optional<std::string> frame_;
    SourceTrace::Point point_{-1, 0};
    QLineEdit *condition_, *count_, *watch_;
    QComboBox *mode_;
    QTreeWidget *rules_, *values_;
    void invoke(const std::function<void()> &action);
    void updateWatches();
};
} // namespace flora
