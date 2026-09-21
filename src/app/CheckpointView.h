#pragma once
#include <QTemporaryDir>
#include <QWidget>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

class QComboBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QToolBar;
namespace flora {
class NativeDebugControls;
class CheckpointView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit CheckpointView(QString stage, QWidget *parent = nullptr);
    ~CheckpointView() override;
    void setContext(QString key, bool available);
    void setWorkerBusy(bool busy);
    void loadOutput(const QString &directory, bool catalog);
    void keepOutput(std::unique_ptr<QTemporaryDir> directory);
    Json exportReport() const;
    void exportArchive(const QString &path) const;
    const QString &stage() const { return stage_; }
    NativeDebugControls *controls() const { return controls_; }
  signals:
    void captureRequested(const QStringList &arguments, bool catalog);
    void error(const QString &message);

  private:
    struct Data;
    std::shared_ptr<Data> data_;
    std::unique_ptr<QTemporaryDir> output_, selector_;
    QString stage_, context_, loadedContext_, summaryText_;
    bool available_ = false, busy_ = false, rendering_ = false;
    NativeDebugControls *controls_;
    QToolBar *captureBar_, *stepBar_;
    QComboBox *mode_, *navigation_, *files_, *frames_, *match_;
    QLineEdit *instruction_;
    QSpinBox *sourceLine_;
    QLabel *summary_, *stack_;
    QTreeWidget *instructions_, *hits_, *registers_, *variables_;
    QPlainTextEdit *source_;
    QTabWidget *codeTabs_, *valueTabs_;
    Json values_ = Json::array(), visible_ = Json::array(), viewLocation_;
    std::optional<std::string> frame_;
    void invoke(const std::function<void()> &action);
    void request(bool catalog, bool trace, bool selected = false);
    size_t position(bool trace = true) const;
    void go(size_t index);
    void showHit();
    void renderFrame();
    void showSource(bool follow = false);
    void move(int direction);
    void step(bool out);
    void seek(bool runTo);
    void toggleBreakpoint();
    void updateBreakpoints();
};
} // namespace flora
