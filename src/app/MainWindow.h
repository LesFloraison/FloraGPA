#pragma once
#include "Models.h"
#include "Views.h"
#include "application/Experiment.h"
#include <QComboBox>
#include <QDockWidget>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

namespace flora {
class CommandStateView;
class PredicateView;
class AnnotationsView;
class StatisticsView;
class GpuProfileView;
class CoverageView;
class QuadView;
class CheckpointView;
class PixelHistoryView;
class ReplayDebugView;
class RdcCountersView;
class ReplayMeshView;
class ShaderProjectDialog;
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow();
    ~MainWindow() override;
    void openCapture(const QString &path);
    void replay(bool timings = false);
    bool busy() const { return process_.state() != QProcess::NotRunning || loader_.isRunning(); }
    QString capturePath() const { return capturePath_; }
    void setShaderTool(const QString &path) { shaderTool_ = path; }
  signals:
    void captureLoaded();
    void taskFinished(bool success);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    void buildUi();
    void loadSettings();
    void selectEvent(Id id);
    void locateEvent(Id id);
    void inspectResource(Id id);
    void previewTexture();
    void previewBuffer();
    void exportBuffer();
    void editBuffer(bool importFile = false);
    void editTexture(bool output);
    void exportTexture();
    void showBufferDetails(const QJsonObject &report);
    void clearBufferDetails();
    void editCounter();
    void editConstant();
    void openExperiment();
    bool saveExperiment();
    void experimentChanged();
    void updateExperimentActions();
    void editClear();
    void editSetter();
    void editDepthStencil();
    void editRasterizer();
    void editBlend();
    void editSampler();
    void editSrv();
    void editView();
    void replaceUpdateSource();
    void compileShader();
    void openShaderProject();
    void recoverShader();
    void readShaderAssembly();
    void assembleShader();
    bool chooseShaderTool();
    void stashShaderDrafts();
    void updateProfileContext();
    void buildCoverageUi();
    void buildQuadUi();
    void updateCoverageContext(const QString &experimentKey);
    void selectCoveragePixel(const QString &report, int x, int y, const QColor &color);
    void showShaderProjectEditor(const nlohmann::json &project);
    void inspectGeometry();
    void exportGeometry();
    void inspectEvent(Id id);
    void inspectCaptureStructure();
    void showPipeline(const State &state);
    void startWorker(QStringList args, bool timings);
    void finishWorker(int, QProcess::ExitStatus);
    void cancel();
    void showError(const QString &error);
    void exportImage();
    void exportOutputStorage();
    void selectOutputPixel(int x, int y, const QColor &color);
    void exportBytes();
    void setBusy(bool);
    void properties(const QString &title, const QList<QPair<QString, QString>> &rows);
    void updateStatistics();
    void updateCheckpointContext();
    QString historyContextKey() const;
    enum class RdcAnalysis { History, Debug, Counters, Mesh };
    void readRdcAnalysis(ReplayDebugView *view = nullptr, RdcAnalysis kind = RdcAnalysis::History);
    bool finishRdcAnalysis(const nlohmann::json &result);
    void startHistoryWorker();
    PixelHistoryView *history_ = nullptr;
    std::array<ReplayDebugView *, 3> replayDebug_{};
    ReplayDebugView *runningDebug_ = nullptr;
    RdcCountersView *rdcCounters_ = nullptr;
    ReplayMeshView *replayMesh_ = nullptr;
    RdcAnalysis runningAnalysis_ = RdcAnalysis::History;
    nlohmann::json historyRequest_;
    uint64_t historyRequestId_{};
    QString historyKey_, historyRdc_;
    struct HistoryCapture {
        std::shared_ptr<QTemporaryDir> directory;
        QString path;
    };
    std::map<QString, HistoryCapture> historyCaptures_;
    QStringList historyCacheOrder_;
    std::array<CheckpointView *, 3> checkpoints_{};
    CheckpointView *runningCheckpoint_ = nullptr;
    bool runningCheckpointCatalog_ = true;
    std::shared_ptr<const Frame> frame_;
    std::unique_ptr<Experiment> experiment_;
    QString projectPath_;
    bool projectDirty_ = false;
    QAction *undoAction_ = nullptr, *redoAction_ = nullptr, *enableAction_ = nullptr;
    QAction *clearAction_ = nullptr, *updateSourceAction_ = nullptr;
    QAction *setterAction_ = nullptr;
    QAction *depthStencilAction_ = nullptr;
    QAction *rasterizerAction_ = nullptr;
    QAction *blendAction_ = nullptr;
    QAction *samplerAction_ = nullptr;
    QAction *srvAction_ = nullptr;
    QAction *viewAction_ = nullptr;
    QAction *bufferEditAction_ = nullptr, *bufferImportAction_ = nullptr;
    QAction *textureInputAction_ = nullptr, *textureOutputAction_ = nullptr;
    QAction *textureExportAction_ = nullptr;
    QAction *constantEditAction_ = nullptr;
    QAction *counterEditAction_ = nullptr;
    QTreeWidget *counters_ = nullptr;
    QTreeWidget *constants_ = nullptr;
    uint64_t bufferDetailsRevision_ = 0;
    Id bufferDetailsEvent_ = 0, bufferDetailsResource_ = 0;
    QString capturePath_, pendingPath_;
    CaptureModel *commands_, *resources_;
    CaptureFilter *commandFilter_, *resourceFilter_;
    BufferModel *bufferModel_;
    QWidget *bufferPane_;
    QComboBox *bufferBoundary_, *bufferMode_;
    QLineEdit *bufferOffset_, *bufferLength_;
    QLabel *bufferLabel_;
    QTimer bufferTimer_;
    Id displayedBuffer_ = 0;
    QTableView *apiView_, *resourceView_, *bufferView_;
    QTreeWidget *pipeline_, *properties_, *metrics_, *statistics_;
    CommandStateView *capturedState_;
    CommandStateView *replayedState_;
    PredicateView *predicateView_;
    AnnotationsView *annotations_;
    StatisticsView *gpuStatistics_;
    GpuProfileView *gpuProfile_;
    QuadView *quad_;
    uint64_t runningQuadRequest_{};
    CoverageView *coverage_;
    uint64_t runningCoverageRequest_{};
    uint64_t runningProfileRequest_{};
    int profileTimeoutMs_ = 240000;
    uint64_t runningStatisticsRequest_{};
    uint64_t runningPredicateRequest_ = 0;
    uint64_t runningPipelineRequest_ = 0;
    QPlainTextEdit *shader_, *log_;
    QTabWidget *shaderPane_;
    QPlainTextEdit *sourceEditor_;
    QComboBox *sourceFiles_;
    QTreeWidget *shaderReflection_;
    QLineEdit *shaderEntry_;
    Id runningShader_ = 0;
    QString runningSource_, runningEntry_;
    bool runningRecover_ = false;
    bool runningAssemblyRead_ = false;
    QString shaderTool_;
    Id shaderDocument_ = 0;
    nlohmann::json shaderDocuments_ = nlohmann::json::object();
    nlohmann::json shaderEntries_ = nlohmann::json::object();
    std::string runningShaderContext_;
    nlohmann::json runningShaderProject_;
    QPointer<ShaderProjectDialog> runningShaderProjectEditor_;
    QTabWidget *centerTabs_, *leftTabs_;
    QWidget *geometryPane_;
    QTabWidget *geometryViews_;
    MeshView *mesh_;
    GeometryModel *geometryModel_;
    QTableView *geometryView_;
    QComboBox *geometryTable_;
    QComboBox *geometryStage_;
    QSpinBox *geometryStream_;
    QLineEdit *geometryInstance_;
    QLabel *geometryLabel_;
    QJsonObject geometry_;
    std::unique_ptr<QTemporaryDir> geometryDir_;
    ImageView *image_;
    ImageView *textureImage_;
    QWidget *texturePane_;
    QComboBox *textureBoundary_, *textureChannels_;
    QComboBox *texturePlane_;
    QSpinBox *textureSample_;
    QLineEdit *textureFormat_, *textureLow_, *textureHigh_;
    QJsonObject textureMetadata_;
    std::unique_ptr<QTemporaryDir> textureDir_;
    QSpinBox *mip_, *layer_, *slice_;
    QLabel *textureLabel_;
    QTimer textureTimer_;
    QString runningKind_;
    EventChart *chart_;
    QMainWindow *workspace_;
    QLabel *frameLabel_, *imageLabel_, *pixelLabel_, *zoomLabel_, *selectionLabel_;
    QComboBox *boundary_, *channels_, *adapter_;
    QComboBox *outputTarget_;
    QSpinBox *outputLayer_, *outputSample_;
    QLineEdit *outputLow_, *outputHigh_;
    QAction *outputStorageAction_;
    std::unique_ptr<QTemporaryDir> outputDir_;
    nlohmann::json outputReport_;
    uint64_t outputGeneration_ = 0, displayedOutputGeneration_ = 0;
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
    std::optional<unsigned> selectedShaderStage_;
    bool runningTimings_ = false;
    QJsonObject report_;
    QByteArray defaultDockState_;
};
} // namespace flora
