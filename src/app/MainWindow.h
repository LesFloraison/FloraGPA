#pragma once
#include "Models.h"
#include "Views.h"
#include <QComboBox>
#include <QDockWidget>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

namespace flora {
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow();
    ~MainWindow() override;
    void openCapture(const QString &path);
    void replay(bool timings = false);
    bool busy() const { return process_.state() != QProcess::NotRunning || loader_.isRunning(); }
    QString capturePath() const { return capturePath_; }
  signals:
    void captureLoaded();
    void taskFinished(bool success);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    void buildUi();
    void loadSettings();
    void selectEvent(Id id);
    void inspectResource(Id id);
    void inspectEvent(Id id);
    void showPipeline(const State &state);
    void startWorker(QStringList args, bool timings);
    void finishWorker(int, QProcess::ExitStatus);
    void cancel();
    void showError(const QString &error);
    void exportImage();
    void exportBytes();
    void setBusy(bool);
    void properties(const QString &title, const QList<QPair<QString, QString>> &rows);
    void updateStatistics();
    std::shared_ptr<const Frame> frame_;
    QString capturePath_, pendingPath_;
    CaptureModel *commands_, *resources_;
    CaptureFilter *commandFilter_, *resourceFilter_;
    BufferModel *bufferModel_;
    QTableView *apiView_, *resourceView_, *bufferView_;
    QTreeWidget *pipeline_, *properties_, *metrics_, *statistics_;
    QPlainTextEdit *shader_, *log_;
    QTabWidget *centerTabs_, *leftTabs_;
    ImageView *image_;
    EventChart *chart_;
    QMainWindow *workspace_;
    QLabel *frameLabel_, *imageLabel_, *pixelLabel_, *zoomLabel_, *selectionLabel_;
    QComboBox *boundary_, *channels_, *adapter_;
    QProgressBar *progress_;
    QAction *openAction_, *replayAction_, *collectAction_, *cancelAction_, *exportAction_;
    QDockWidget *logDock_;
    QProcess process_;
    QFutureWatcher<std::shared_ptr<const Frame>> loader_;
    std::unique_ptr<QTemporaryDir> jobDir_;
    QTimer replayTimer_, timeout_;
    QByteArray stderrBuffer_;
    QString errorText_;
    void *job_ = nullptr;
    uint64_t revision_ = 0, runningRevision_ = 0;
    Id selectedEvent_ = 0, selectedResource_ = 0;
    bool runningTimings_ = false;
    QJsonObject report_;
    QByteArray defaultDockState_;
};
} // namespace flora
