#include "MainWindow.h"
#include "BlendDialog.h"
#include "CommandStateView.h"
#include "ConstantBufferDialog.h"
#include "IaSetterDialog.h"
#include "OutputDialog.h"
#include "PipelineSetterDialog.h"
#include "PredicateView.h"
#include "RasterizerDialog.h"
#include "SamplerDialog.h"
#include "SrvDialog.h"
#include "ViewDialog.h"
#include "application/ClassInspector.h"
#include "application/CommandEdits.h"
#include "application/Constants.h"
#include "application/ContextInspector.h"
#include "application/FrameOutput.h"
#include "application/PredicateInspector.h"
#include "application/SessionUi.h"
#include "application/SetterEdits.h"
#include "application/ShaderInspector.h"
#include "core/BufferBindings.h"
#include "replay/Replay.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace flora {
namespace {
QTableView *table(QAbstractItemModel *model, QWidget *parent = nullptr) {
    auto view = new QTableView(parent);
    view->setModel(model);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(25);
    view->horizontalHeader()->setStretchLastSection(true);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setWordWrap(false);
    view->setSortingEnabled(false);
    view->setColumnWidth(0, 58);
    view->setColumnWidth(1, 185);
    return view;
}
QTreeWidget *tree(const QStringList &labels) {
    auto view = new QTreeWidget;
    view->setHeaderLabels(labels);
    view->setAlternatingRowColors(true);
    view->setUniformRowHeights(true);
    view->setRootIsDecorated(true);
    view->header()->setStretchLastSection(true);
    return view;
}
QTreeWidgetItem *row(QTreeWidget *tree, const QString &name, const QString &value) {
    return new QTreeWidgetItem(tree, QStringList{name, value});
}
QWidget *filtered(QTableView *view, CaptureFilter *proxy, QComboBox *options) {
    auto container = new QWidget;
    auto layout = new QVBoxLayout(container);
    layout->setContentsMargins(6, 6, 6, 0);
    layout->setSpacing(5);
    auto filter = new QLineEdit;
    filter->setPlaceholderText("Filter…");
    filter->setClearButtonEnabled(true);
    QObject::connect(filter, &QLineEdit::textChanged, proxy,
                     [proxy](const QString &text) { proxy->setFilterFixedString(text); });
    layout->addWidget(filter);
    if (options)
        layout->addWidget(options);
    layout->addWidget(view);
    return container;
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save file");
}
QString hex(Bytes bytes, size_t limit = 256) {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(std::min(limit, bytes.size())))
            .toHex(' '));
}
QString boundaryLabel(const QJsonObject &report) {
    auto when = report["value_time"].toString();
    if (when == "capture_initial")
        return "Initial";
    auto event = report["event"].toString();
    if (event.isEmpty())
        return "Final";
    return (when == "before_event" ? QString("Before %1") : QString("After %1")).arg(event);
}
} // namespace
MainWindow::MainWindow() {
    buildUi();
    loadSettings();
    replayTimer_.setSingleShot(true);
    replayTimer_.setInterval(180);
    connect(&replayTimer_, &QTimer::timeout, this, [this] { replay(); });
    textureTimer_.setSingleShot(true);
    textureTimer_.setInterval(180);
    connect(&textureTimer_, &QTimer::timeout, this, &MainWindow::previewTexture);
    bufferTimer_.setSingleShot(true);
    bufferTimer_.setInterval(180);
    connect(&bufferTimer_, &QTimer::timeout, this, &MainWindow::previewBuffer);
    timeout_.setSingleShot(true);
    timeout_.setInterval(180000);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        cancel();
        showError("Worker timed out.");
    });
    connect(&loader_, &QFutureWatcher<std::shared_ptr<const Frame>>::finished, this, [this] {
        try {
            auto loaded = loader_.result();
            frame_ = std::move(loaded);
            capturePath_ = pendingPath_;
            ++revision_;
            selectedEvent_ = selectedResource_ = 0;
            selectedShaderStage_.reset();
            experiment_ = std::make_unique<Experiment>(*frame_);
            projectPath_.clear();
            projectDirty_ = false;
            updateExperimentActions();
            commands_->setFrame(frame_);
            resources_->setFrame(frame_);
            chart_->clear();
            pipeline_->clear();
            capturedState_->setSelection(frame_, 0);
            replayedState_->setSelection(frame_, 0);
            predicateView_->setSelection(frame_, 0);
            metrics_->clear();
            properties_->clear();
            shader_->clear();
            sourceFiles_->clear();
            sourceEditor_->clear();
            shaderReflection_->clear();
            textureImage_->setImage({});
            textureLabel_->clear();
            textureMetadata_ = {};
            textureDir_.reset();
            textureExportAction_->setEnabled(false);
            geometry_ = {};
            geometryModel_->setTable({});
            mesh_->setMesh({});
            geometryLabel_->clear();
            geometryDir_.reset();
            bufferModel_->setBytes({});
            bufferLabel_->clear();
            clearBufferDetails();
            displayedBuffer_ = 0;
            report_ = {};
            outputDir_.reset();
            outputReport_ = nullptr;
            outputStorageAction_->setEnabled(false);
            {
                QSignalBlocker targetBlock(outputTarget_), layerBlock(outputLayer_),
                    sampleBlock(outputSample_), channelBlock(channels_);
                while (outputTarget_->count() > 12)
                    outputTarget_->removeItem(12);
                const auto inventory = presentationInventory(*frame_);
                for (const auto &chain : inventory["swap_chains"])
                    if (!chain["target"].is_null()) {
                        auto selector = QString::fromStdString(chain["selector"].get<std::string>());
                        outputTarget_->addItem(selector, selector);
                    }
                outputTarget_->setCurrentIndex(0);
                outputLayer_->setValue(-1);
                outputSample_->setValue(-1);
                channels_->setCurrentText("RGBA");
                outputLow_->setText("0");
                outputHigh_->setText("1");
            }
            image_->setImage({});
            setWindowTitle(QFileInfo(capturePath_).completeBaseName() + "[*] — FloraGPA");
            frameLabel_->setText(QFileInfo(capturePath_).fileName());
            selectionLabel_->setText(QString("%1 API calls").arg(commands_->rowCount()));
            boundary_->setCurrentIndex(0);
            updateStatistics();
            setBusy(false);
            emit captureLoaded();
            replay();
        } catch (const std::exception &e) {
            setBusy(false);
            showError(QString::fromUtf8(e.what()));
            emit taskFinished(false);
        }
    });
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    connect(&process_, &QProcess::started, this, [this] {
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process_.processId()));
        bool ok = job_ &&
                  SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof info) &&
                  handle && AssignProcessToJobObject(job_, handle);
        if (handle)
            CloseHandle(handle);
        if (!ok) {
            errorText_ = "Cannot isolate the replay worker.";
            process_.kill();
        }
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        stderrBuffer_ += process_.readAllStandardError();
        for (;;) {
            auto newline = stderrBuffer_.indexOf('\n');
            if (newline < 0)
                break;
            auto line = QString::fromUtf8(stderrBuffer_.left(newline)).trimmed();
            stderrBuffer_.remove(0, newline + 1);
            if (line.startsWith("progress ")) {
                auto values = line.split(' ');
                if (values.size() == 4) {
                    progress_->setRange(0, 100);
                    progress_->setValue(int(values[2].toDouble() / std::max(1., values[3].toDouble()) * 100));
                }
            } else if (!line.isEmpty()) {
                errorText_ = line;
                log_->appendPlainText(line);
            }
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this,
            [this] { process_.readAllStandardOutput(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            timeout_.stop();
            setBusy(false);
            showError(process_.errorString());
            if (runningKind_ == "predicate")
                predicateView_->finish(runningPredicateRequest_,
                                       {{"error", process_.errorString().toStdString()}});
            if (runningKind_ == "replay-pipeline")
                replayedState_->finishReplay(runningPipelineRequest_,
                                             {{"error", process_.errorString().toStdString()}});
            emit taskFinished(false);
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &MainWindow::finishWorker);
}
MainWindow::~MainWindow() {
    replayTimer_.stop();
    timeout_.stop();
    if (job_)
        CloseHandle(job_);
    process_.kill();
    process_.waitForFinished(3000);
    loader_.waitForFinished();
}
void MainWindow::buildUi() {
    resize(1680, 1000);
    setMinimumSize(1000, 640);
    setWindowTitle("FloraGPA");
    setWindowIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    auto file = menuBar()->addMenu("&File");
    openAction_ = file->addAction("&Open Capture…", QKeySequence::Open, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Open DX11 capture", {},
                                                 "GPA frames (*.gpa_frame *.gpaframe);;All files (*)");
        if (!path.isEmpty())
            openCapture(path);
    });
    openAction_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    file->addAction("Open Experiment…", this, &MainWindow::openExperiment)->setObjectName("openExperiment");
    file->addAction("Save Experiment…", QKeySequence::Save, this, [this] { saveExperiment(); })
        ->setObjectName("saveExperiment");
    auto edit = menuBar()->addMenu("&Edit");
    undoAction_ = edit->addAction("Undo", QKeySequence::Undo, this, [this] {
        if (experiment_ && experiment_->undo())
            experimentChanged();
    });
    redoAction_ = edit->addAction("Redo", QKeySequence::Redo, this, [this] {
        if (!experiment_ || !experiment_->redo())
            return;
        try {
            ReplayOptions check;
            experiment_->apply(*frame_, check);
            experimentChanged();
        } catch (const std::exception &e) {
            experiment_->undo();
            showError(QString::fromUtf8(e.what()));
        }
    });
    enableAction_ = edit->addAction("Disable Event", this, [this] {
        if (!experiment_ || !selectedEvent_)
            return;
        try {
            experiment_->setEnabled(*frame_, selectedEvent_, !experiment_->enabled(selectedEvent_));
            experimentChanged();
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
        }
    });
    clearAction_ = edit->addAction("Edit Clear Values…", this, &MainWindow::editClear);
    clearAction_->setObjectName("editClear");
    setterAction_ = edit->addAction("Edit Setter…", this, &MainWindow::editSetter);
    setterAction_->setObjectName("editSetter");
    depthStencilAction_ = edit->addAction("Edit Depth / Stencil…", this, &MainWindow::editDepthStencil);
    depthStencilAction_->setObjectName("editDepthStencil");
    rasterizerAction_ = edit->addAction("Edit Rasterizer…", this, &MainWindow::editRasterizer);
    rasterizerAction_->setObjectName("editRasterizer");
    blendAction_ = edit->addAction("Edit Blend / Samples…", this, &MainWindow::editBlend);
    blendAction_->setObjectName("editBlend");
    samplerAction_ = edit->addAction("Edit Sampler…", this, &MainWindow::editSampler);
    samplerAction_->setObjectName("editSampler");
    srvAction_ = edit->addAction("Edit Shader Resource View…", this, &MainWindow::editSrv);
    srvAction_->setObjectName("editSrv");
    viewAction_ = edit->addAction("Edit View Resource…", this, &MainWindow::editView);
    viewAction_->setObjectName("editView");
    updateSourceAction_ = edit->addAction("Replace Update Source…", this, &MainWindow::replaceUpdateSource);
    updateSourceAction_->setObjectName("replaceUpdateSource");
    updateExperimentActions();
    file->addSeparator();
    exportAction_ =
        file->addAction("Export Image…", QKeySequence("Ctrl+Shift+S"), this, &MainWindow::exportImage);
    outputStorageAction_ = file->addAction("Export Output Storage…", this, &MainWindow::exportOutputStorage);
    outputStorageAction_->setObjectName("exportOutputStorage");
    outputStorageAction_->setEnabled(false);
    file->addAction("Export Resource…", this, &MainWindow::exportBytes);
    file->addSeparator();
    file->addAction("Exit", QKeySequence::Quit, this, &QWidget::close);
    auto analyze = menuBar()->addMenu("&Analysis");
    replayAction_ = analyze->addAction("Replay", QKeySequence("F5"), this, [this] { replay(); });
    replayAction_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    collectAction_ =
        analyze->addAction("Collect GPU Metrics", QKeySequence("F6"), this, [this] { replay(true); });
    cancelAction_ = analyze->addAction("Cancel", QKeySequence("Escape"), this, &MainWindow::cancel);
    cancelAction_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    cancelAction_->setEnabled(false);
    auto structureAction =
        analyze->addAction("Contexts && Command Lists…", this, &MainWindow::inspectCaptureStructure);
    structureAction->setObjectName("inspectCaptureStructure");
    auto viewMenu = menuBar()->addMenu("&View");
    auto help = menuBar()->addMenu("&Help");
    help->addAction("About FloraGPA", this, [this] {
        QMessageBox::about(this, "FloraGPA",
                           "FloraGPA 0.1\nNative DX11 frame analyzer\nC++20 · Qt 6 · Visual Studio 2022");
    });
    auto bar = addToolBar("Capture");
    bar->setMovable(false);
    bar->setIconSize({20, 20});
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->addAction(openAction_);
    bar->addSeparator();
    bar->addAction(replayAction_);
    bar->addAction(cancelAction_);
    bar->addSeparator();
    frameLabel_ = new QLabel("No capture");
    frameLabel_->setMinimumWidth(180);
    bar->addWidget(frameLabel_);
    auto spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    adapter_ = new QComboBox;
    adapter_->setObjectName("replayAdapter");
    adapter_->addItems({"Hardware · DX11", "WARP · DX11"});
    adapter_->setToolTip("Replay adapter");
    bar->addWidget(adapter_);
    auto central = new QWidget;
    auto root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    setCentralWidget(central);
    auto chartPane = new QWidget;
    auto chartLayout = new QVBoxLayout(chartPane);
    chartLayout->setContentsMargins(0, 0, 0, 0);
    chartLayout->setSpacing(0);
    auto controls = new QToolBar;
    controls->setIconSize({16, 16});
    auto metric = new QComboBox;
    metric->addItem("Y: GPU Time Elapsed");
    controls->addWidget(metric);
    auto grouping = new QComboBox;
    grouping->addItem("X: API events");
    controls->addWidget(grouping);
    controls->addSeparator();
    controls->addAction(collectAction_);
    auto expand = new QWidget;
    expand->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    controls->addWidget(expand);
    selectionLabel_ = new QLabel("—");
    controls->addWidget(selectionLabel_);
    chartLayout->addWidget(controls);
    chart_ = new EventChart;
    chartLayout->addWidget(chart_);
    connect(chart_, &EventChart::eventSelected, this, &MainWindow::selectEvent);
    auto vertical = new QSplitter(Qt::Vertical);
    vertical->setHandleWidth(4);
    vertical->addWidget(chartPane);
    root->addWidget(vertical);
    workspace_ = new QMainWindow;
    workspace_->setObjectName("workspace");
    workspace_->setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                               QMainWindow::AllowTabbedDocks);
    vertical->addWidget(workspace_);
    vertical->setStretchFactor(0, 0);
    vertical->setStretchFactor(1, 1);
    vertical->setSizes({265, 650});
    commands_ = new CaptureModel(CaptureModel::Kind::Commands, this);
    commandFilter_ = new CaptureFilter(this);
    commandFilter_->setSourceModel(commands_);
    commandFilter_->setFilterKeyColumn(-1);
    commandFilter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    commandFilter_->workOnly = true;
    apiView_ = table(commandFilter_);
    apiView_->setContextMenuPolicy(Qt::ActionsContextMenu);
    apiView_->addActions({enableAction_, clearAction_, setterAction_, depthStencilAction_, rasterizerAction_,
                          blendAction_, samplerAction_, srvAction_, updateSourceAction_});
    apiView_->setObjectName("apiLog");
    auto exportApi = new QAction("Export API Log…", this);
    exportApi->setObjectName("exportApiLog");
    apiView_->addAction(exportApi);
    connect(exportApi, &QAction::triggered, this, [this] {
        if (!frame_)
            return;
        auto path = QFileDialog::getExistingDirectory(this, "Export API Log");
        if (path.isEmpty())
            return;
        if ((QFile::exists(path + "/commands.json") || QFile::exists(path + "/commands.csv")) &&
            QMessageBox::question(this, "Export API Log",
                                  "Replace existing commands.json and commands.csv?") != QMessageBox::Yes)
            return;
        try {
            exportCommands(*frame_, std::filesystem::path(path.toStdWString()),
                           commandFilter_->searchText.toStdString(), commandFilter_->referencedResource,
                           commandFilter_->workOnly);
            statusBar()->showMessage("API log exported", 3000);
        } catch (const std::exception &error) {
            showError(QString::fromUtf8(error.what()));
        }
    });
    apiView_->setColumnWidth(1, 185);
    apiView_->setColumnHidden(2, true);
    resources_ = new CaptureModel(CaptureModel::Kind::Resources, this);
    resourceFilter_ = new CaptureFilter(this);
    resourceFilter_->setSourceModel(resources_);
    resourceFilter_->setFilterKeyColumn(-1);
    resourceFilter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    resourceView_ = table(resourceFilter_);
    resourceView_->setObjectName("resources");
    auto apiKinds = new QComboBox;
    apiKinds->setObjectName("apiKinds");
    apiKinds->addItems({"GPU commands", "All API calls"});
    connect(apiKinds, &QComboBox::currentIndexChanged, this, [this](int i) {
        commandFilter_->workOnly = i == 0;
        commandFilter_->refresh();
    });
    auto resourceKinds = new QComboBox;
    resourceKinds->addItems({"All resources", "Textures", "Buffers", "Shaders"});
    connect(resourceKinds, &QComboBox::currentIndexChanged, this, [this](int i) {
        resourceFilter_->resourceType = i;
        resourceFilter_->refresh();
    });
    leftTabs_ = new QTabWidget;
    leftTabs_->setDocumentMode(true);
    auto apiPane = filtered(apiView_, commandFilter_, apiKinds);
    apiPane->findChild<QLineEdit *>()->setObjectName("apiSearch");
    auto referenceFilter = new QLineEdit;
    referenceFilter->setObjectName("apiResourceFilter");
    referenceFilter->setPlaceholderText("Referenced resource ID…");
    referenceFilter->setClearButtonEnabled(true);
    connect(referenceFilter, &QLineEdit::textChanged, this, [this, referenceFilter](const QString &value) {
        bool valid = false;
        auto id = value.toULongLong(&valid);
        referenceFilter->setToolTip(value.isEmpty() || valid ? "" : "Enter an unsigned resource ID");
        if (!value.isEmpty() && (!valid || value.startsWith('-')))
            return;
        commandFilter_->referencedResource = value.isEmpty() ? std::nullopt : std::optional<Id>(id);
        commandFilter_->refresh();
    });
    static_cast<QVBoxLayout *>(apiPane->layout())->insertWidget(2, referenceFilter);
    leftTabs_->addTab(apiPane, "API Log");
    leftTabs_->addTab(filtered(resourceView_, resourceFilter_, resourceKinds), "Resources");
    statistics_ = tree({"Statistic", "Value"});
    leftTabs_->addTab(statistics_, "Statistics");
    auto left = new QDockWidget("Frame", workspace_);
    left->setObjectName("frameDock");
    left->setWidget(leftTabs_);
    left->setMinimumWidth(240);
    workspace_->addDockWidget(Qt::LeftDockWidgetArea, left);
    viewMenu->addAction(left->toggleViewAction());
    centerTabs_ = new QTabWidget;
    centerTabs_->setObjectName("analysisTabs");
    centerTabs_->setDocumentMode(true);
    workspace_->setCentralWidget(centerTabs_);
    auto output = new QWidget;
    auto outputLayout = new QVBoxLayout(output);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(0);
    auto imageBar = new QToolBar;
    auto displayBar = new QToolBar;
    boundary_ = new QComboBox;
    boundary_->setObjectName("outputBoundary");
    boundary_->addItems({"Final", "Before event", "After event"});
    imageBar->addWidget(boundary_);
    outputTarget_ = new QComboBox;
    outputTarget_->setObjectName("outputTarget");
    for (auto target :
         {"auto", "present", "rt0", "rt1", "rt2", "rt3", "rt4", "rt5", "rt6", "rt7", "depth", "stencil"})
        outputTarget_->addItem(QString(target).toUpper(), target);
    outputTarget_->setToolTip("Output target");
    imageBar->addWidget(outputTarget_);
    outputLayer_ = new QSpinBox;
    outputLayer_->setObjectName("outputLayer");
    outputLayer_->setRange(-1, 65535);
    outputLayer_->setSpecialValueText("Auto layer");
    outputLayer_->setPrefix("Layer ");
    outputLayer_->setValue(-1);
    outputLayer_->setToolTip("Absolute array layer or volume slice");
    imageBar->addWidget(outputLayer_);
    outputSample_ = new QSpinBox;
    outputSample_->setObjectName("outputSample");
    outputSample_->setRange(-1, 31);
    outputSample_->setSpecialValueText("Resolve");
    outputSample_->setPrefix("Sample ");
    outputSample_->setValue(-1);
    outputSample_->setToolTip("MSAA sample index; Resolve averages samples");
    imageBar->addWidget(outputSample_);
    imageBar->addSeparator();
    channels_ = new QComboBox;
    channels_->addItems({"RGB", "RGBA", "R", "G", "B", "A"});
    channels_->setObjectName("outputChannel");
    displayBar->addWidget(channels_);
    outputLow_ = new QLineEdit("0");
    outputHigh_ = new QLineEdit("1");
    outputLow_->setObjectName("outputLow");
    outputHigh_->setObjectName("outputHigh");
    outputLow_->setMaximumWidth(70);
    outputHigh_->setMaximumWidth(70);
    outputLow_->setToolTip("Display range minimum");
    outputHigh_->setToolTip("Display range maximum");
    displayBar->addWidget(outputLow_);
    displayBar->addWidget(new QLabel("–"));
    displayBar->addWidget(outputHigh_);
    imageBar->addSeparator();
    imageBar->addAction("Fit", this, [this] { image_->fit(); });
    imageBar->addAction("100%", this, [this] { image_->actualSize(); });
    imageBar->addAction("Export", this, &MainWindow::exportImage);
    auto imageSpacer = new QWidget;
    imageSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    displayBar->addWidget(imageSpacer);
    imageLabel_ = new QLabel("—");
    displayBar->addWidget(imageLabel_);
    outputLayout->addWidget(imageBar);
    outputLayout->addWidget(displayBar);
    image_ = new ImageView;
    image_->setObjectName("frameOutput");
    outputLayout->addWidget(image_);
    centerTabs_->addTab(output, "Output");
    texturePane_ = new QWidget;
    auto textureLayout = new QVBoxLayout(texturePane_);
    textureLayout->setContentsMargins(0, 0, 0, 0);
    textureLayout->setSpacing(0);
    auto textureBar = new QToolBar;
    textureBoundary_ = new QComboBox;
    textureBoundary_->setObjectName("textureBoundary");
    textureBoundary_->addItems({"Capture initial", "Before event", "After event"});
    textureBar->addWidget(textureBoundary_);
    mip_ = new QSpinBox;
    layer_ = new QSpinBox;
    slice_ = new QSpinBox;
    for (auto pair : {std::pair{mip_, "Mip "}, std::pair{layer_, "Layer "}, std::pair{slice_, "Slice "}}) {
        pair.first->setPrefix(pair.second);
        textureBar->addWidget(pair.first);
        connect(pair.first, &QSpinBox::valueChanged, this, [this] { textureTimer_.start(); });
    }
    textureChannels_ = new QComboBox;
    textureChannels_->addItems({"RGBA", "RGB", "R", "G", "B", "A"});
    textureChannels_->setObjectName("textureChannels");
    auto textureDisplayBar = new QToolBar;
    textureDisplayBar->addWidget(textureChannels_);
    textureSample_ = new QSpinBox;
    textureSample_->setObjectName("textureSample");
    textureSample_->setRange(-1, 31);
    textureSample_->setPrefix("Sample ");
    textureSample_->setSpecialValueText("Resolve");
    textureSample_->setValue(-1);
    textureSample_->setToolTip("Resolve all samples, or inspect one MSAA sample.");
    textureDisplayBar->addWidget(textureSample_);
    textureFormat_ = new QLineEdit;
    textureFormat_->setObjectName("textureFormat");
    textureFormat_->setPlaceholderText("Format: Auto");
    textureFormat_->setMaximumWidth(115);
    textureFormat_->setToolTip("DXGI typed format number (decimal or 0x hex).");
    textureDisplayBar->addWidget(textureFormat_);
    texturePlane_ = new QComboBox;
    texturePlane_->setObjectName("texturePlane");
    texturePlane_->addItem("Plane: Auto", "auto");
    texturePlane_->addItem("Y", "y");
    texturePlane_->addItem("UV", "uv");
    texturePlane_->setToolTip("Raw planar channels; no RGB color conversion.");
    textureDisplayBar->addWidget(texturePlane_);
    textureLow_ = new QLineEdit("0");
    textureHigh_ = new QLineEdit("1");
    textureLow_->setObjectName("textureLow");
    textureHigh_->setObjectName("textureHigh");
    textureDisplayBar->addWidget(new QLabel(" Range "));
    for (auto rangeField : {textureLow_, textureHigh_}) {
        rangeField->setMaximumWidth(75);
        textureDisplayBar->addWidget(rangeField);
    }
    textureExportAction_ = textureDisplayBar->addAction("Export…", this, &MainWindow::exportTexture);
    textureExportAction_->setObjectName("exportTexture");
    textureExportAction_->setEnabled(false);
    textureInputAction_ = textureBar->addAction("Import Input…", this, [this] { editTexture(false); });
    textureOutputAction_ = textureBar->addAction("Import Output…", this, [this] { editTexture(true); });
    textureInputAction_->setObjectName("importTextureInput");
    textureOutputAction_->setObjectName("importTextureOutput");
    textureInputAction_->setToolTip("Replace one input subresource for the selected event.");
    textureOutputAction_->setToolTip("Replace one output subresource before the selected event.");
    textureInputAction_->setEnabled(false);
    textureOutputAction_->setEnabled(false);
    textureLabel_ = new QLabel("—");
    textureBar->addWidget(textureLabel_);
    textureLayout->addWidget(textureBar);
    textureLayout->addWidget(textureDisplayBar);
    textureImage_ = new ImageView;
    textureImage_->setObjectName("textureOutput");
    textureLayout->addWidget(textureImage_);
    centerTabs_->addTab(texturePane_, "Texture");
    connect(textureBoundary_, &QComboBox::currentIndexChanged, this, [this] { textureTimer_.start(); });
    connect(textureChannels_, &QComboBox::currentIndexChanged, this, [this] { textureTimer_.start(); });
    connect(textureSample_, &QSpinBox::valueChanged, this, [this] { textureTimer_.start(); });
    connect(texturePlane_, &QComboBox::currentIndexChanged, this, [this] { textureTimer_.start(); });
    for (auto textureField : {textureFormat_, textureLow_, textureHigh_})
        connect(textureField, &QLineEdit::editingFinished, this, [this] { textureTimer_.start(); });

    pipeline_ = tree({"Stage / Binding", "Resource", "Details"});
    pipeline_->setObjectName("pipeline");
    pipeline_->setColumnWidth(0, 225);
    pipeline_->setColumnWidth(1, 110);
    auto pipelineTabs = new QTabWidget;
    pipelineTabs->setObjectName("pipelineTabs");
    pipelineTabs->setDocumentMode(true);
    pipelineTabs->addTab(pipeline_, "Snapshot");
    capturedState_ = new CommandStateView;
    pipelineTabs->addTab(capturedState_, "Captured State");
    replayedState_ = new CommandStateView(CommandStateView::Source::Replayed);
    pipelineTabs->addTab(replayedState_, "Replay State");
    predicateView_ = new PredicateView;
    pipelineTabs->addTab(predicateView_, "Predicate");
    connect(predicateView_, &PredicateView::readRequested, this,
            [this](qulonglong resource, qulonglong event, bool after, qulonglong request) {
                runningPredicateRequest_ = request;
                if (busy() || !frame_) {
                    predicateView_->finish(request, {{"error", "Worker is busy"}});
                    return;
                }
                QStringList args{"predicate", capturePath_,
                                 "--event",   QString::number(event),
                                 "--id",      QString::number(resource)};
                if (!after)
                    args << "--before";
                startWorker(args, false);
                if (process_.state() == QProcess::NotRunning)
                    predicateView_->finish(request, {{"error", "Cannot start predicate inspection"}});
            });
    centerTabs_->addTab(pipelineTabs, "Pipeline");
    connect(capturedState_, &CommandStateView::eventRequested, this, &MainWindow::locateEvent);
    connect(capturedState_, &CommandStateView::resourceRequested, this, &MainWindow::inspectResource);
    connect(replayedState_, &CommandStateView::eventRequested, this, &MainWindow::locateEvent);
    connect(replayedState_, &CommandStateView::resourceRequested, this, &MainWindow::inspectResource);
    connect(replayedState_, &CommandStateView::replayRequested, this,
            [this](qulonglong event, bool after, qulonglong request) {
                runningPipelineRequest_ = request;
                if (busy() || !frame_) {
                    replayedState_->finishReplay(request, {{"error", "Worker is busy"}});
                    return;
                }
                QStringList args{"replay-pipeline", capturePath_, "--event", QString::number(event)};
                if (!after)
                    args << "--before";
                startWorker(args, false);
                if (process_.state() == QProcess::NotRunning)
                    replayedState_->finishReplay(request, {{"error", "Cannot start pipeline inspection"}});
            });
    shader_ = new QPlainTextEdit;
    shader_->setReadOnly(true);
    shader_->setLineWrapMode(QPlainTextEdit::NoWrap);
    shader_->setFont(QFont("Cascadia Mono", 10));
    shader_->setObjectName("shader");
    shaderPane_ = new QTabWidget;
    shaderPane_->setDocumentMode(true);
    auto sourcePane = new QWidget;
    auto sourceLayout = new QVBoxLayout(sourcePane);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceFiles_ = new QComboBox;
    sourceFiles_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sourceFiles_->setMinimumContentsLength(25);
    sourceEditor_ = new QPlainTextEdit;
    sourceEditor_->setObjectName("shaderSource");
    sourceEditor_->setReadOnly(false);
    sourceEditor_->setFont(QFont("Cascadia Mono", 10));
    sourceEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto editBar = new QToolBar;
    editBar->addAction("Import HLSL", this, [this] {
        auto path =
            QFileDialog::getOpenFileName(this, "Import HLSL", {}, "HLSL files (*.hlsl *.fx);;All files (*)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            showError("Cannot read HLSL source.");
            return;
        }
        sourceEditor_->setPlainText(QString::fromUtf8(file.readAll()));
        shaderPane_->setCurrentIndex(0);
    });
    shaderEntry_ = new QLineEdit("main");
    shaderEntry_->setMaximumWidth(160);
    shaderEntry_->setToolTip("HLSL entry point");
    editBar->addWidget(shaderEntry_);
    editBar->addAction("Compile && Apply", this, &MainWindow::compileShader);
    sourceLayout->addWidget(editBar);
    sourceLayout->addWidget(sourceFiles_);
    sourceLayout->addWidget(sourceEditor_);
    shaderPane_->addTab(sourcePane, "Source");
    shaderPane_->addTab(shader_, "DXBC");
    shaderReflection_ = tree({"Binding / Variable", "Slot / Offset", "Type / Size"});
    shaderReflection_->setObjectName("shaderReflection");
    shaderReflection_->setColumnWidth(0, 250);
    shaderReflection_->setColumnWidth(1, 120);
    shaderPane_->addTab(shaderReflection_, "Reflection");
    connect(sourceFiles_, &QComboBox::currentIndexChanged, this,
            [this](int index) { sourceEditor_->setPlainText(sourceFiles_->itemData(index).toString()); });
    centerTabs_->addTab(shaderPane_, "Shader");
    bufferModel_ = new BufferModel(this);
    bufferView_ = table(bufferModel_);
    bufferView_->setObjectName("bufferTable");
    bufferView_->setFont(QFont("Cascadia Mono", 10));
    bufferView_->setColumnWidth(0, 105);
    bufferView_->setColumnWidth(1, 420);
    bufferPane_ = new QWidget;
    auto bufferLayout = new QVBoxLayout(bufferPane_);
    bufferLayout->setContentsMargins(0, 0, 0, 0);
    bufferLayout->setSpacing(0);
    auto bufferBar = new QToolBar;
    bufferBoundary_ = new QComboBox;
    bufferBoundary_->setObjectName("bufferBoundary");
    bufferBoundary_->addItems({"Initial", "Before event", "After event"});
    bufferBar->addWidget(bufferBoundary_);
    bufferMode_ = new QComboBox;
    bufferMode_->setObjectName("bufferMode");
    bufferMode_->addItems({"Hex / ASCII", "32-bit words"});
    bufferMode_->setToolTip("Numeric columns interpret raw storage; they are not inferred types.");
    bufferBar->addWidget(bufferMode_);
    bufferOffset_ = new QLineEdit("0");
    bufferOffset_->setObjectName("bufferOffset");
    bufferOffset_->setMaximumWidth(110);
    bufferOffset_->setToolTip("Byte offset (decimal or 0x hex)");
    bufferBar->addWidget(bufferOffset_);
    bufferLength_ = new QLineEdit;
    bufferLength_->setObjectName("bufferLength");
    bufferLength_->setMaximumWidth(110);
    bufferLength_->setPlaceholderText("To end");
    bufferLength_->setToolTip("Byte length (empty reads to the end)");
    bufferBar->addWidget(bufferLength_);
    bufferBar->addAction("Read", this, &MainWindow::previewBuffer);
    bufferBar->addAction("Export", this, &MainWindow::exportBuffer);
    bufferEditAction_ = bufferBar->addAction("Edit Bytes…", this, [this] { editBuffer(); });
    bufferEditAction_->setObjectName("editBuffer");
    bufferImportAction_ = bufferBar->addAction("Import Patch…", this, [this] { editBuffer(true); });
    bufferEditAction_->setEnabled(false);
    bufferImportAction_->setEnabled(false);
    bufferLabel_ = new QLabel;
    bufferLayout->addWidget(bufferBar);
    bufferLayout->addWidget(bufferLabel_);
    auto bufferTabs = new QTabWidget;
    bufferTabs->setObjectName("bufferTabs");
    bufferTabs->addTab(bufferView_, "Bytes");
    auto constantPane = new QWidget;
    auto constantLayout = new QVBoxLayout(constantPane);
    constantLayout->setContentsMargins(0, 0, 0, 0);
    constantLayout->setSpacing(0);
    auto constantBar = new QToolBar;
    constantEditAction_ = constantBar->addAction("Edit Value…", this, &MainWindow::editConstant);
    constantEditAction_->setObjectName("editConstant");
    constantEditAction_->setEnabled(false);
    constantEditAction_->setToolTip("Read Before event, then select a reflected field to edit.");
    constants_ = tree({"Field", "Type", "Offset", "Value"});
    constants_->setObjectName("constantFields");
    constants_->setColumnWidth(0, 250);
    constants_->setColumnWidth(1, 180);
    constants_->setColumnWidth(2, 80);
    constantLayout->addWidget(constantBar);
    constantLayout->addWidget(constants_);
    bufferTabs->addTab(constantPane, "Constants");
    auto counterPane = new QWidget;
    auto counterLayout = new QVBoxLayout(counterPane);
    counterLayout->setContentsMargins(0, 0, 0, 0);
    counterLayout->setSpacing(0);
    auto counterBar = new QToolBar;
    counterEditAction_ = counterBar->addAction("Edit Counter…", this, &MainWindow::editCounter);
    counterEditAction_->setObjectName("editCounter");
    counterEditAction_->setEnabled(false);
    counterEditAction_->setToolTip("Read Before event, then select a UAV counter to edit.");
    counters_ = tree({"View", "Kind", "Bindings", "Value", "Elements", "Stride"});
    counters_->setObjectName("uavCounters");
    counters_->setRootIsDecorated(false);
    counters_->setColumnWidth(0, 80);
    counters_->setColumnWidth(1, 140);
    counters_->setColumnWidth(2, 160);
    counters_->setColumnWidth(3, 120);
    counterLayout->addWidget(counterBar);
    counterLayout->addWidget(counters_);
    bufferTabs->addTab(counterPane, "UAV Counters");
    connect(counters_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateExperimentActions);
    connect(counters_, &QTreeWidget::itemDoubleClicked, this, [this] { editCounter(); });
    bufferLayout->addWidget(bufferTabs);
    connect(constants_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateExperimentActions);
    connect(constants_, &QTreeWidget::itemDoubleClicked, this, [this] { editConstant(); });
    centerTabs_->addTab(bufferPane_, "Buffer");
    connect(bufferBoundary_, &QComboBox::currentIndexChanged, this, [this] {
        clearBufferDetails();
        bufferTimer_.start();
    });
    connect(bufferMode_, &QComboBox::currentIndexChanged, this, [this](int mode) {
        bufferModel_->setWords(mode == 1);
        bufferView_->setColumnWidth(1, mode ? 145 : 420);
    });
    geometryPane_ = new QWidget;
    auto geometryLayout = new QVBoxLayout(geometryPane_);
    geometryLayout->setContentsMargins(0, 0, 0, 0);
    geometryLayout->setSpacing(0);
    auto geometryBar = new QToolBar;
    geometryBar->addAction("Inspect IA", this, &MainWindow::inspectGeometry);
    geometryBar->addAction("Export", this, &MainWindow::exportGeometry);
    geometryTable_ = new QComboBox;
    geometryTable_->addItem("Expanded vertices", "expanded_vertices");
    geometryTable_->addItem("Unique vertices", "unique_vertices");
    geometryTable_->addItem("Index mapping", "references");
    geometryBar->addWidget(geometryTable_);
    geometryLabel_ = new QLabel;
    geometryBar->addWidget(geometryLabel_);
    geometryLayout->addWidget(geometryBar);
    auto geometrySplit = new QSplitter(Qt::Vertical);
    mesh_ = new MeshView;
    mesh_->setObjectName("iaMesh");
    geometrySplit->addWidget(mesh_);
    geometryModel_ = new GeometryModel(this);
    geometryView_ = table(geometryModel_);
    geometryView_->setObjectName("geometryTable");
    geometryView_->horizontalHeader()->setStretchLastSection(false);
    geometryView_->horizontalHeader()->setDefaultSectionSize(130);
    geometrySplit->addWidget(geometryView_);
    geometrySplit->setSizes({300, 220});
    geometryLayout->addWidget(geometrySplit);
    centerTabs_->addTab(geometryPane_, "Geometry");
    connect(geometryTable_, &QComboBox::currentIndexChanged, this, [this] {
        geometryModel_->setTable(
            geometry_["tables"].toObject()[geometryTable_->currentData().toString()].toObject());
    });
    for (const auto &name : {"Pixel History", "Shader Debug"}) {
        int tab = centerTabs_->addTab(new QWidget, name);
        centerTabs_->setTabEnabled(tab, false);
        centerTabs_->setTabToolTip(tab, "Migration pending");
    }
    auto rightTabs = new QTabWidget;
    rightTabs->setObjectName("inspectorTabs");
    rightTabs->setDocumentMode(true);
    properties_ = tree({"Property", "Value"});
    properties_->setObjectName("properties");
    connect(properties_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!frame_ || item->data(0, Qt::UserRole).toString().isEmpty())
            return;
        try {
            const auto ref = nlohmann::json::parse(item->data(0, Qt::UserRole).toString().toStdString());
            const auto selectionText = item->data(1, Qt::UserRole).toString();
            if (!selectionText.isEmpty()) {
                const auto selection = nlohmann::json::parse(selectionText.toStdString());
                inspectResource(selection["resource"].get<Id>());
                auto type = frame_->entry(selection["resource"]).type;
                if (type >= 0x84 && type <= 0x87) {
                    mip_->setValue(selection["mip"].get<int>());
                    layer_->setValue(selection["layer"].get<int>());
                    slice_->setValue(selection["slice"].get<int>());
                }
            } else if (ref.value("category", 0) == 7)
                selectEvent(ref["id"]);
            else if (ref.value("category", 0) == 5)
                inspectResource(ref["id"]);
        } catch (const std::exception &error) {
            showError(QString::fromUtf8(error.what()));
        }
    });
    metrics_ = tree({"Metric", "Value"});
    metrics_->setColumnWidth(0, 175);
    rightTabs->addTab(properties_, "Properties");
    rightTabs->addTab(metrics_, "Metrics");
    auto right = new QDockWidget("Inspector", workspace_);
    right->setObjectName("inspectorDock");
    right->setWidget(rightTabs);
    right->setMinimumWidth(230);
    workspace_->addDockWidget(Qt::RightDockWidgetArea, right);
    viewMenu->addAction(right->toggleViewAction());
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(10000);
    log_->setFont(QFont("Cascadia Mono", 9));
    logDock_ = new QDockWidget("Tasks / Log", workspace_);
    logDock_->setObjectName("logDock");
    logDock_->setWidget(log_);
    workspace_->addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    logDock_->hide();
    viewMenu->addAction(logDock_->toggleViewAction());
    workspace_->resizeDocks({left, right}, {300, 280}, Qt::Horizontal);
    defaultDockState_ = workspace_->saveState(1);
    viewMenu->addSeparator();
    viewMenu->addAction("Reset Layout", this, [this] { workspace_->restoreState(defaultDockState_, 1); });
    pixelLabel_ = new QLabel;
    statusBar()->addWidget(pixelLabel_, 1);
    progress_ = new QProgressBar;
    progress_->setMaximumWidth(180);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(12);
    progress_->hide();
    statusBar()->addPermanentWidget(progress_);
    zoomLabel_ = new QLabel;
    zoomLabel_->setMinimumWidth(42);
    statusBar()->addPermanentWidget(zoomLabel_);
    connect(image_, &ImageView::pixelHovered, pixelLabel_, &QLabel::setText);
    connect(image_, &ImageView::pixelSelected, this, &MainWindow::selectOutputPixel);
    connect(image_, &ImageView::zoomChanged, this,
            [this](int percent) { zoomLabel_->setText(QString("%1%").arg(percent)); });
    auto outputChanged = [this] {
        if (!frame_)
            return;
        ++revision_;
        ++outputGeneration_;
        if (process_.state() != QProcess::NotRunning)
            cancel();
        replayTimer_.start();
    };
    connect(channels_, &QComboBox::currentTextChanged, this, outputChanged);
    connect(outputTarget_, &QComboBox::currentIndexChanged, this, [this, outputChanged] {
        if (outputLow_->text() == "0") {
            if (outputTarget_->currentData().toString() == "stencil" && outputHigh_->text() == "1")
                outputHigh_->setText("255");
            else if (outputTarget_->currentData().toString() != "stencil" && outputHigh_->text() == "255")
                outputHigh_->setText("1");
        }
        outputChanged();
    });
    connect(outputLayer_, &QSpinBox::valueChanged, this, outputChanged);
    connect(outputSample_, &QSpinBox::valueChanged, this, outputChanged);
    connect(outputLow_, &QLineEdit::editingFinished, this, outputChanged);
    connect(outputHigh_, &QLineEdit::editingFinished, this, outputChanged);
    connect(boundary_, &QComboBox::currentIndexChanged, this, [this] {
        if (frame_) {
            ++revision_;
            if (process_.state() != QProcess::NotRunning)
                cancel();
            replayTimer_.start();
        }
    });
    connect(adapter_, &QComboBox::currentIndexChanged, this, [this] {
        ++outputGeneration_;
        replayedState_->invalidate();
        predicateView_->invalidate();
        ++revision_;
        if (process_.state() != QProcess::NotRunning)
            cancel();
        chart_->clear();
        if (frame_)
            replayTimer_.start();
    });
    connect(apiView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &current) {
                if (current.isValid())
                    selectEvent(current.data(Qt::UserRole).toULongLong());
            });
    connect(resourceView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &current) {
                if (current.isValid())
                    inspectResource(current.data(Qt::UserRole).toULongLong());
            });
    connect(pipeline_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        auto id = item->data(1, Qt::UserRole).toULongLong();
        if (id) {
            const QStringList stages{"VS", "HS", "DS", "GS", "PS", "CS"};
            const auto stage = item->parent() ? -1 : stages.indexOf(item->text(0));
            inspectResource(id);
            if (stage >= 0)
                selectedShaderStage_ = unsigned(stage);
        }
    });
    setBusy(false);
}
void MainWindow::loadSettings() {
    QSettings s;
    restoreGeometry(s.value("window/geometry").toByteArray());
    workspace_->restoreState(s.value("window/docks").toByteArray(), 1);
}
void MainWindow::closeEvent(QCloseEvent *e) {
    if (projectDirty_) {
        auto answer = QMessageBox::question(this, "Unsaved experiment", "Save experiment changes?",
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !saveExperiment())) {
            e->ignore();
            return;
        }
    }
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/docks", workspace_->saveState(1));
    cancel();
    e->accept();
}
void MainWindow::setBusy(bool busy) {
    if (busy && textureExportAction_)
        textureExportAction_->setEnabled(false);
    replayedState_->setWorkerBusy(busy);
    predicateView_->setWorkerBusy(busy);
    openAction_->setEnabled(!busy);
    viewAction_->setEnabled(!busy && frame_ && experiment_);
    replayAction_->setEnabled(!busy && bool(frame_));
    collectAction_->setEnabled(!busy && bool(frame_));
    cancelAction_->setEnabled(busy && !loader_.isRunning());
    progress_->setVisible(busy);
    if (busy) {
        progress_->setRange(0, 0);
        constantEditAction_->setEnabled(false);
        counterEditAction_->setEnabled(false);
    } else
        progress_->setValue(0);
    exportAction_->setEnabled(!image_->image().isNull());
}
void MainWindow::openCapture(const QString &path) {
    if (projectDirty_) {
        auto answer = QMessageBox::question(this, "Unsaved experiment", "Save experiment changes?",
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !saveExperiment()))
            return;
    }
    if (loader_.isRunning())
        return;
    cancel();
    replayTimer_.stop();
    pendingPath_ = path;
    setBusy(true);
    statusBar()->showMessage("Opening capture…");
    loader_.setFuture(QtConcurrent::run([path] {
        auto frame = std::make_shared<Frame>(std::filesystem::path(path.toStdWString()));
        frame->sha256();
        return std::shared_ptr<const Frame>(frame);
    }));
}
void MainWindow::replay(bool timings) {
    if (!frame_ || loader_.isRunning())
        return;
    if (process_.state() != QProcess::NotRunning) {
        cancel();
        replayTimer_.start();
        return;
    }
    QStringList args{"replay",           capturePath_,
                     "--output-target",  outputTarget_->currentData().toString(),
                     "--output-channel", channels_->currentText().toLower(),
                     "--output-low",     outputLow_->text(),
                     "--output-high",    outputHigh_->text()};
    if (outputLayer_->value() >= 0)
        args << "--output-layer" << QString::number(outputLayer_->value());
    if (outputSample_->value() >= 0)
        args << "--output-sample" << QString::number(outputSample_->value());
    if (boundary_->currentIndex() != 0 && selectedEvent_ && !timings) {
        args << "--event" << QString::number(selectedEvent_);
        if (boundary_->currentIndex() == 1)
            args << "--before";
    }
    if (timings)
        args << "--timings";
    startWorker(args, timings);
}
void MainWindow::startWorker(QStringList args, bool timings) {
    if (process_.state() != QProcess::NotRunning)
        return;
    replayTimer_.stop();
    textureTimer_.stop();
    bufferTimer_.stop();
    jobDir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/FloraGPA-XXXXXX");
    if (!jobDir_->isValid()) {
        showError("Cannot create worker directory.");
        return;
    }
    args << "--out" << jobDir_->path() + "/result";
    if (args.first() == "compile") {
        auto path = jobDir_->path() + "/edited.hlsl";
        try {
            writeFile(path, runningSource_.toUtf8());
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
            return;
        }
        args << "--source" << path << "--entry" << runningEntry_;
    }
    if (experiment_ && !experiment_->document().at("history").empty()) {
        auto path = jobDir_->path() + "/experiment.json";
        try {
            experiment_->save(path);
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
            return;
        }
        args << "--experiment" << path;
    }
    if (adapter_->currentIndex() == 1)
        args << "--warp";
    runningRevision_ = revision_;
    runningTimings_ = timings;
    runningKind_ = args.first();
    stderrBuffer_.clear();
    errorText_.clear();
    setBusy(true);
    statusBar()->showMessage(timings ? "Collecting GPU metrics…" : "Replaying…");
    timeout_.start();
    process_.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe", args);
}
void MainWindow::cancel() {
    ++revision_;
    replayTimer_.stop();
    textureTimer_.stop();
    bufferTimer_.stop();
    if (process_.state() != QProcess::NotRunning) {
        if (job_)
            TerminateJobObject(job_, 1);
        process_.kill();
    }
    timeout_.stop();
}
void MainWindow::finishWorker(int code, QProcess::ExitStatus status) {
    timeout_.stop();
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    setBusy(false);
    if (runningRevision_ != revision_) {
        if (runningKind_ == "predicate")
            predicateView_->finish(runningPredicateRequest_, {{"error", "Cancelled"}});
        if (runningKind_ == "replay-pipeline")
            replayedState_->finishReplay(runningPipelineRequest_, {{"error", "Cancelled"}});
        statusBar()->showMessage("Cancelled", 2000);
        emit taskFinished(false);
        return;
    }
    try {
        if (code || status != QProcess::NormalExit) {
            auto error = QJsonDocument::fromJson(errorText_.toUtf8()).object()["error"].toString(errorText_);
            throw std::runtime_error((error.isEmpty() ? "Replay worker failed" : error).toStdString());
        }
        QFile file(jobDir_->path() + "/result/report.json");
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Worker report is missing");
        QJsonParseError error;
        const auto reportBytes = file.readAll();
        report_ = QJsonDocument::fromJson(reportBytes, &error).object();
        if (error.error != QJsonParseError::NoError || !report_["completed"].toBool())
            throw std::runtime_error("Worker did not complete");
        if (runningKind_ == "replay-pipeline") {
            QFile stateFile(jobDir_->path() + "/result/replay-pipeline.json");
            if (!stateFile.open(QIODevice::ReadOnly))
                throw std::runtime_error("Pipeline state output is missing");
            auto bytes = stateFile.readAll();
            auto state = nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
            auto accepted = replayedState_->finishReplay(runningPipelineRequest_, std::move(state));
            statusBar()->showMessage(accepted ? "Replay state ready" : "Pipeline result discarded", 3000);
            emit taskFinished(accepted);
            return;
        }
        if (runningKind_ == "predicate") {
            QFile predicateFile(jobDir_->path() + "/result/predicate.json");
            if (!predicateFile.open(QIODevice::ReadOnly))
                throw std::runtime_error("Predicate output is missing");
            auto bytes = predicateFile.readAll();
            auto result = nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
            auto accepted = predicateView_->finish(runningPredicateRequest_, result);
            statusBar()->showMessage(accepted ? "Predicate ready" : "Predicate result discarded", 3000);
            emit taskFinished(accepted);
            return;
        }
        if (runningKind_ == "compile") {
            QFile binary(jobDir_->path() + "/result/replacement.dxbc");
            if (!binary.open(QIODevice::ReadOnly))
                throw std::runtime_error("Compiled bytecode is missing");
            auto bytes = binary.readAll();
            experiment_->setShader(
                *frame_, runningShader_,
                Bytes(reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())),
                runningSource_.toStdString(), runningEntry_.toStdString());
            experimentChanged();
            statusBar()->showMessage("Shader applied", 3000);
            emit taskFinished(true);
            return;
        }
        if (runningKind_ == "geometry") {
            QFile geometryFile(jobDir_->path() + "/result/geometry.json");
            if (!geometryFile.open(QIODevice::ReadOnly))
                throw std::runtime_error("Geometry output is missing");
            QJsonParseError parseError;
            auto geometry = QJsonDocument::fromJson(geometryFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError)
                throw std::runtime_error("Invalid geometry result");
            geometry_ = geometry.object();
            geometryModel_->setTable(
                geometry_["tables"].toObject()[geometryTable_->currentData().toString()].toObject());
            mesh_->setMesh(geometry_["mesh"].toObject());
            geometryLabel_->setText(QString("  Event %1 · %2 references · %3 unique")
                                        .arg(geometry_["event"].toString())
                                        .arg(geometry_["vertex_references"].toInteger())
                                        .arg(geometry_["unique_vertices"].toInteger()));
            const auto automatic = geometry_["draw_auto"].toObject();
            geometryLabel_->setToolTip(
                automatic.isEmpty()
                    ? QString{}
                    : QString::fromUtf8(QJsonDocument(automatic).toJson(QJsonDocument::Indented)));
            if (!automatic.isEmpty())
                geometryLabel_->setText(geometryLabel_->text() + (automatic["history_verified"].toBool()
                                                                      ? " · SO verified"
                                                                      : " · Captured count"));
            geometryDir_ = std::move(jobDir_);
            centerTabs_->setCurrentWidget(geometryPane_);
            statusBar()->showMessage("IA geometry ready", 3000);
            emit taskFinished(true);
            return;
        }
        if (runningKind_ == "buffer") {
            QFile bufferFile(jobDir_->path() + "/result/buffer.bin");
            if (!bufferFile.open(QIODevice::ReadOnly))
                throw std::runtime_error("Buffer output is missing");
            bufferModel_->setBytes(bufferFile.readAll(), uint64_t(report_["offset"].toInteger()));
            displayedBuffer_ = report_["resource"].toString().toULongLong();
            showBufferDetails(report_);
            bufferLabel_->setText(QString("  B:%1 · %2 bytes · %3")
                                      .arg(displayedBuffer_)
                                      .arg(report_["length"].toInteger())
                                      .arg(boundaryLabel(report_)));
            statusBar()->showMessage("Buffer ready", 3000);
            emit taskFinished(true);
            return;
        }
        QImage result(jobDir_->path() + "/result/frame.png");
        const bool outputAvailable = runningKind_ != "replay" || report_["image_available"].toBool(true);
        if (result.isNull() && outputAvailable)
            throw std::runtime_error("Worker output image is missing");
        if (runningKind_ == "texture") {
            textureImage_->setImage(std::move(result));
            textureImage_->channel("RGBA");
            textureMetadata_ = report_["texture"].toObject();
            textureDir_ = std::move(jobDir_);
            textureExportAction_->setEnabled(true);
            textureLabel_->setText(QString("%1 × %2 · %3")
                                       .arg(report_["width"].toInt())
                                       .arg(report_["height"].toInt())
                                       .arg(boundaryLabel(report_)));
            if (textureMetadata_.contains("selected_plane")) {
                const auto plane = textureMetadata_["selected_plane"].toObject();
                textureLabel_->setText(textureLabel_->text() + " · " + plane["name"].toString().toUpper());
                textureLabel_->setToolTip(textureMetadata_["capture_plane_notice"].toString());
            } else
                textureLabel_->setToolTip(
                    textureMetadata_["msaa"].toObject()["initialization_note"].toString());
            statusBar()->showMessage("Texture ready", 3000);
            emit taskFinished(true);
            return;
        }
        image_->setImage(std::move(result));
        image_->channel("RGBA");
        imageLabel_->setText(QString("T:%1  ·  %2 × %3 · %4")
                                 .arg(report_["resource"].toString())
                                 .arg(report_["width"].toInt())
                                 .arg(report_["height"].toInt())
                                 .arg(boundaryLabel(report_)));
        const auto display = report_["output_display"].toObject();
        if (outputAvailable && !display.isEmpty())
            imageLabel_->setText(imageLabel_->text() + QString(" · M%1 L%2")
                                                           .arg(display["mip"].toInt())
                                                           .arg(display["slice"].toInt()
                                                                    ? display["slice"].toInt()
                                                                    : display["layer"].toInt()));
        if (!outputAvailable)
            imageLabel_->setText("No output image");
        imageLabel_->setToolTip(report_["image_status"].toString());
        const auto msaa = report_["output_msaa"].toObject();
        if (!msaa.isEmpty())
            imageLabel_->setToolTip(imageLabel_->toolTip() + "\n" + msaa["initialization_note"].toString());
        outputReport_ = outputAvailable ? nlohmann::json::parse(reportBytes.constData(),
                                                                reportBytes.constData() + reportBytes.size())
                                        : nlohmann::json();
        displayedOutputGeneration_ = outputGeneration_;
        outputDir_ = std::move(jobDir_);
        outputStorageAction_->setEnabled(outputAvailable);
        if (runningTimings_) {
            findChild<QTabWidget *>("inspectorTabs")->setCurrentWidget(metrics_);
            chart_->setTimings(report_["timings"].toArray());
            metrics_->clear();
            auto stats = report_["pipeline_statistics"].toObject();
            auto group = new QTreeWidgetItem(metrics_, {"Pipeline Statistics", ""});
            for (auto it = stats.begin(); it != stats.end(); ++it)
                new QTreeWidgetItem(group, {it.key(), it.value().toString()});
            group->setExpanded(true);
            double total = 0;
            for (auto x : report_["timings"].toArray())
                total += x.toObject()["microseconds"].toDouble();
            row(metrics_, "GPU work (µs)", QString::number(total, 'f', 3));
        }
        exportAction_->setEnabled(outputAvailable);
        statusBar()->showMessage(report_["adapter"].toString() + "  ·  Replay complete", 7000);
        log_->appendPlainText(QString("Replay complete · %1 × %2 · %3")
                                  .arg(report_["width"].toInt())
                                  .arg(report_["height"].toInt())
                                  .arg(report_["rgba_sha256"].toString()));
        emit taskFinished(true);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
        if (runningKind_ == "replay-pipeline")
            replayedState_->finishReplay(runningPipelineRequest_, {{"error", e.what()}});
        if (runningKind_ == "predicate")
            predicateView_->finish(runningPredicateRequest_, {{"error", e.what()}});
        emit taskFinished(false);
    }
}
void MainWindow::showError(const QString &error) {
    log_->appendPlainText(error);
    statusBar()->showMessage(error);
    logDock_->show();
}
void MainWindow::properties(const QString &title, const QList<QPair<QString, QString>> &values) {
    properties_->clear();
    auto group = new QTreeWidgetItem(properties_, {title, {}});
    for (auto &[name, value] : values) {
        auto item = new QTreeWidgetItem(group, {name, value});
        item->setToolTip(1, value);
    }
    group->setExpanded(true);
    properties_->resizeColumnToContents(0);
}
void MainWindow::locateEvent(Id id) {
    if (!frame_)
        return;
    auto source = commands_->index(commands_->rowOf(id), 0);
    if (!source.isValid())
        return;
    if (!commandFilter_->mapFromSource(source).isValid()) {
        findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        findChild<QLineEdit *>("apiSearch")->clear();
        findChild<QLineEdit *>("apiResourceFilter")->clear();
    }
    leftTabs_->setCurrentIndex(0);
    selectEvent(id);
    auto index = commandFilter_->mapFromSource(source);
    apiView_->setCurrentIndex(index);
    apiView_->scrollTo(index);
}
void MainWindow::selectEvent(Id id) {
    if (!frame_ || selectedEvent_ == id)
        return;
    selectedEvent_ = id;
    capturedState_->setSelection(frame_, id);
    replayedState_->setSelection(frame_, id);
    predicateView_->setSelection(frame_, id);
    selectedResource_ = 0;
    selectedShaderStage_.reset();
    clearBufferDetails();
    updateExperimentActions();
    ++revision_;
    chart_->setSelection(id);
    selectionLabel_->setText(QString("Event %1").arg(id));
    auto index = commandFilter_->mapFromSource(commands_->index(commands_->rowOf(id), 0));
    if (index.isValid() && apiView_->currentIndex().data(Qt::UserRole).toULongLong() != id) {
        QSignalBlocker blocker(apiView_->selectionModel());
        apiView_->setCurrentIndex(index);
        apiView_->scrollTo(index);
    }
    try {
        inspectEvent(id);
        if (isDraw(frame_->entry(id).type)) {
            ReplayOptions options;
            if (experiment_)
                experiment_->apply(*frame_, options);
            showPipeline(effectiveBindings(*frame_, id, frame_->state(frame_->event(id).state), options));
        } else
            pipeline_->clear();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
    if (boundary_->currentIndex() == 0) {
        QSignalBlocker blocker(boundary_);
        boundary_->setCurrentIndex(2);
    }
    if (process_.state() != QProcess::NotRunning)
        cancel();
    replayTimer_.start();
}
void MainWindow::inspectEvent(Id id) {
    const auto &details = commands_->command(id);
    auto display = [](const nlohmann::json &v) {
        return QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
    };
    properties(display(details["name"]),
               {{"ID", QString::number(id)},
                {"Type", QString("0x%1").arg(details["type"].get<uint16_t>(), 4, 16, QChar('0'))},
                {"Wire bytes", display(details["wire_size"])},
                {"Decode", display(details["status"])}});
    auto root = properties_->topLevelItem(0);
    root->setToolTip(0, QString::fromStdString(details.value("note", "")));
    if (details.contains("error"))
        new QTreeWidgetItem(root, {"Error", display(details["error"])});
    auto fields = new QTreeWidgetItem(properties_, {"Captured fields", ""});
    fields->setData(0, Qt::UserRole + 1, "apiFields");
    for (const auto &f : details["fields"]) {
        auto item = new QTreeWidgetItem(fields, {display(f["name"]), display(f["value"])});
        auto tip = QString("Offset %1 · %2 bytes · %3\n%4")
                       .arg(f["offset"].get<uint64_t>())
                       .arg(f["size"].get<uint64_t>())
                       .arg(display(f["encoding"]), display(f["hex"]));
        item->setToolTip(0, tip);
        item->setToolTip(1, tip);
    }
    fields->setExpanded(true);
    auto references = new QTreeWidgetItem(properties_, {"References", ""});
    references->setData(0, Qt::UserRole + 1, "apiReferences");
    for (const auto &ref : details["references"]) {
        auto item = new QTreeWidgetItem(references, {display(ref["field"]), display(ref["id"])});
        item->setToolTip(1, ref["exists"] == true ? "Double-click to inspect" : "Missing captured record");
        if (ref["exists"] == true) {
            item->setData(0, Qt::UserRole, QString::fromStdString(ref.dump()));
            try {
                auto selection = commandResourceSelection(*frame_, ref, details);
                if (!selection.is_null())
                    item->setData(1, Qt::UserRole, QString::fromStdString(selection.dump()));
            } catch (const std::exception &error) {
                item->setToolTip(1, QString::fromUtf8(error.what()));
            }
        }
    }
    references->setExpanded(true);
    std::function<void(QTreeWidgetItem *, const nlohmann::json &)> append;
    append = [&](QTreeWidgetItem *parent, const nlohmann::json &value) {
        if (value.is_object())
            for (auto it = value.begin(); it != value.end(); ++it) {
                bool nested = it.value().is_structured();
                auto child = new QTreeWidgetItem(
                    parent, {QString::fromStdString(it.key()), nested ? QString{} : display(it.value())});
                if (nested)
                    append(child, it.value());
            }
        else if (value.is_array())
            for (size_t i = 0; i < value.size(); ++i) {
                auto child = new QTreeWidgetItem(
                    parent, {QString::number(i), value[i].is_structured() ? QString{} : display(value[i])});
                if (value[i].is_structured())
                    append(child, value[i]);
            }
    };
    for (auto key :
         {"annotation", "query_capture", "query_result", "command_list", "command_list_finish", "replay"})
        if (details.contains(key)) {
            auto group = new QTreeWidgetItem(properties_, {QString::fromLatin1(key), ""});
            append(group, details[key]);
        }
    // Context kind and interface version are independent. Preserve inferred unknown fields.
    Id contextId = 0;
    for (const auto &field : details["fields"])
        if (field["name"] == (isDraw(frame_->entry(id).type) ? "context" : "object"))
            contextId = field["value"].get<Id>();
    if (contextId) {
        try {
            auto info = describeContext(*frame_, contextId);
            auto group =
                new QTreeWidgetItem(properties_, {"Context", info.version ? "Captured" : "Inferred"});
            group->setData(0, Qt::UserRole + 1, "contextDetails");
            append(group, contextJson(info));
        } catch (const std::exception &) {
        } // The API object can be a device/resource rather than a context.
    }
    if (details.contains("remaining_hex")) {
        auto remaining =
            new QTreeWidgetItem(properties_, {"Undecoded bytes", display(details["remaining_offset"])});
        remaining->setToolTip(1, display(details["remaining_hex"]));
        new QTreeWidgetItem(remaining, {"Hex", display(details["remaining_hex"])});
    }
    if (details.contains("replay_unavailable"))
        root->setToolTip(1, display(details["replay_unavailable"]));
    if (isClearCommand(frame_->entry(id).type) && details["status"] == "decoded") {
        try {
            auto group = new QTreeWidgetItem(properties_, {"Experiment values", ""});
            append(group, experiment_->clear(*frame_, id));
        } catch (const std::exception &) {
        } // Inspection remains available for unreplayable contexts.
    }
}
void MainWindow::inspectCaptureStructure() {
    if (!frame_)
        return;
    auto contexts = inspectContexts(*frame_);
    nlohmann::json lists;
    try {
        lists = inspectCommandLists(*frame_);
    } catch (const std::exception &error) {
        lists = {{"error", error.what()}};
    }
    QDialog dialog(this);
    dialog.setObjectName("captureStructureDialog");
    dialog.setWindowTitle("Contexts and Command Lists");
    dialog.resize(840, 600);
    auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget;
    layout->addWidget(tabs);
    auto add = [&](const QString &title, const nlohmann::json &document) {
        auto view = tree({"Field", "Value"});
        view->setObjectName(title == "Contexts" ? "contextInventory" : "commandListInventory");
        view->setColumnWidth(0, 290);
        std::function<void(QTreeWidgetItem *, const nlohmann::json &)> fill;
        fill = [&](QTreeWidgetItem *parent, const nlohmann::json &value) {
            auto item = [&](const QString &key, const nlohmann::json &child) {
                if (key == "assumption" || key == "scope" || key == "limits" || key == "note" ||
                    key == "source") {
                    auto detail =
                        QString::fromStdString(child.is_string() ? child.get<std::string>() : child.dump(2));
                    if (parent)
                        parent->setToolTip(0, parent->toolTip(0) + '\n' + detail);
                    else
                        view->setToolTip(view->toolTip() + '\n' + detail);
                    return;
                }
                auto label =
                    child.is_structured()
                        ? QString{}
                        : QString::fromStdString(child.is_string() ? child.get<std::string>() : child.dump());
                auto row = parent ? new QTreeWidgetItem(parent, {key, label})
                                  : new QTreeWidgetItem(view, {key, label});
                if (child.is_structured())
                    fill(row, child);
                if (child.is_number_unsigned() && (key == "event" || key == "unmap_event" || key == "id")) {
                    auto id = child.get<Id>();
                    auto found = frame_->entries().find(id);
                    if (found != frame_->entries().end() && found->second.category == 7) {
                        row->setData(1, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                        row->setToolTip(1, "Double-click to locate API event");
                    }
                }
            };
            if (value.is_object())
                for (auto it = value.begin(); it != value.end(); ++it)
                    item(QString::fromStdString(it.key()), it.value());
            else if (value.is_array())
                for (size_t i = 0; i < value.size(); ++i)
                    item(QString::number(i), value[i]);
        };
        fill(nullptr, document);
        view->expandToDepth(1);
        tabs->addTab(view, title);
        connect(view, &QTreeWidget::itemDoubleClicked, &dialog, [&dialog, this](QTreeWidgetItem *item, int) {
            auto id = item->data(1, Qt::UserRole).toULongLong();
            if (id) {
                dialog.accept();
                locateEvent(id);
            }
        });
    };
    add("Contexts", contexts);
    add("Command Lists", lists);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto exportButton = buttons->addButton("Export JSON…", QDialogButtonBox::ActionRole);
    exportButton->setObjectName("exportCaptureStructure");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        auto commandLists = tabs->currentIndex() == 1;
        auto path = QFileDialog::getSaveFileName(&dialog, "Export Capture Structure",
                                                 commandLists ? "command-lists.json" : "contexts.json",
                                                 "JSON (*.json)");
        if (path.isEmpty())
            return;
        try {
            writeFile(path, QByteArray::fromStdString((commandLists ? lists : contexts).dump(2) + "\n"));
        } catch (const std::exception &error) {
            showError(QString::fromUtf8(error.what()));
        }
    });
    dialog.exec();
}
void MainWindow::showPipeline(const State &s) {
    pipeline_->clear();
    auto link = [&](QTreeWidgetItem *parent, const QString &name, Id id, const QString &detail = QString()) {
        auto item = new QTreeWidgetItem(parent, {name, id ? QString::number(id) : "—", detail});
        item->setData(1, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        return item;
    };
    auto ia = new QTreeWidgetItem(pipeline_, {"IA", "", "Input Assembler"});
    link(ia, "Input layout", s.layout);
    link(ia, "Index buffer", s.ib, QString("Format %1 · offset %2").arg(s.ibFormat).arg(s.ibOffset));
    for (int i = 0; i < 32; ++i)
        if (s.vb[i])
            link(ia, QString("Vertex buffer %1").arg(i), s.vb[i],
                 QString("Stride %1 · offset %2").arg(s.strides[i]).arg(s.offsets[i]));
    ia->setExpanded(true);
    const QStringList names{"VS", "HS", "DS", "GS", "PS", "CS"};
    for (int k = 0; k < 6; ++k) {
        auto &stage = s.stages[k];
        auto group = new QTreeWidgetItem(pipeline_,
                                         {names[k], stage.shader ? QString::number(stage.shader) : "—", ""});
        group->setData(1, Qt::UserRole, QVariant::fromValue<qulonglong>(stage.shader));
        for (uint32_t i = 0; i < std::min(stage.classCount, 256u); ++i)
            link(group, QString("Class instance %1").arg(i), stage.classes[i]);
        for (int i = 0; i < 14; ++i)
            if (stage.cb[i]) {
                auto label = QString("CB %1").arg(i);
                if (auto range = stage.cbRanges.find(i); range != stage.cbRanges.end() && range->second[0])
                    label += QString(" [%1 + %2]").arg(*range->second[0]).arg(*range->second[1]);
                link(group, label, stage.cb[i]);
            }
        for (int i = 0; i < 128; ++i)
            if (stage.srv[i])
                link(group, QString("SRV %1").arg(i), stage.srv[i]);
        for (int i = 0; i < 16; ++i)
            if (stage.samplers[i])
                link(group, QString("Sampler %1").arg(i), stage.samplers[i]);
    }
    auto so = new QTreeWidgetItem(pipeline_, {"SO", "", "Stream Output"});
    for (UINT slot = 0; slot < std::min(s.soCount, 4u); ++slot)
        link(so, QString("Buffer %1").arg(slot), s.so[slot],
             s.soOffsets[slot] == UINT32_MAX ? "Append" : QString("Offset %1").arg(s.soOffsets[slot]));
    so->setExpanded(s.soCount != 0);
    so->setToolTip(2, "Captured binding offsets; live append positions have no native getter.");
    auto rs = new QTreeWidgetItem(pipeline_, {"RS", "", "Rasterizer"});
    link(rs, "State", s.rasterizer);
    link(rs, "Viewports", s.viewports);
    link(rs, "Scissors", s.scissors);
    auto om = new QTreeWidgetItem(pipeline_, {"OM", "", "Output Merger"});
    link(om, "Blend", s.blend);
    link(om, "Depth / stencil", s.depthState);
    for (UINT i = 0; i < std::min(s.rtCount, 8u); ++i)
        link(om, QString(i < s.omStart ? "RTV %1" : "UAV %1").arg(i), s.rtv[i]);
    link(om, "DSV", s.dsv);
    om->setExpanded(true);
    auto predicate = new QTreeWidgetItem(pipeline_, {"Predication", "", "Conditional Execution"});
    link(predicate, "Predicate", s.predicate, QString("Value %1").arg(s.predicateValue));
    predicate->setExpanded(s.predicate != 0);
}
void MainWindow::inspectResource(Id id) {
    selectedShaderStage_.reset();
    if (!frame_)
        return;
    try {
        auto &e = frame_->entry(id);
        if (e.category != 5)
            return;
        if (selectedResource_ != id) {
            clearBufferDetails();
            ++revision_;
            if (process_.state() != QProcess::NotRunning)
                cancel();
            textureTimer_.stop();
            bufferTimer_.stop();
        }
        selectedResource_ = id;
        updateExperimentActions();
        if (contextVersion(e.type)) {
            auto context = describeContext(*frame_, id);
            properties(
                "Device Context",
                {{"ID", QString::number(id)},
                 {"Kind", context.deferred ? "Deferred" : "Immediate"},
                 {"Interface", QString("ID3D11DeviceContext%1")
                                   .arg(*context.version ? QString::number(*context.version) : QString{})},
                 {"Device", QString::number(context.device)},
                 {"Creation flags", QString::number(*context.flags)},
                 {"Captured pointer", QString("0x%1").arg(*context.pointer, 0, 16)}});
            return;
        }
        if (e.type == 0x9a) {
            auto list = inspectCommandList(*frame_, id);
            properties("Command List",
                       {{"ID", QString::number(id)},
                        {"Parent context", QString::number(list["parent_context"].get<Id>())},
                        {"Parent kind", QString::fromStdString(list["parent_context_type"])},
                        {"Original player parent",
                         QString::number(list["original_player_fields"]["parent_context"].get<Id>())},
                        {"Execution", "Not restored"}});
            return;
        }
        auto resource = frame_->resource(id);
        if (e.type == 0x96) {
            auto descriptor = readPredicate(*frame_, id);
            properties("Predicate", {{"ID", QString::number(id)},
                                     {"Query", descriptor.type == 5 ? "Occlusion" : "SO overflow"},
                                     {"Flags", QString::number(descriptor.flags)}});
            predicateView_->selectResource(id);
            auto tabs = findChild<QTabWidget *>("pipelineTabs");
            tabs->setCurrentWidget(predicateView_);
            centerTabs_->setCurrentWidget(tabs);
            return;
        }
        if (e.type == 0x97 || e.type == 0x98) {
            auto detail = inspectClass(*frame_, id);
            auto display = [](const nlohmann::json &value) {
                return QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
            };
            QList<QPair<QString, QString>> values{{"ID", QString::number(id)}};
            for (auto it = detail.begin(); it != detail.end(); ++it) {
                if (it.key() == "resource_kind")
                    continue;
                if (it.key() == "desc") {
                    for (auto field = it->begin(); field != it->end(); ++field)
                        values.append({QString::fromStdString(field.key()), display(*field)});
                } else
                    values.append({QString::fromStdString(it.key()), display(*it)});
            }
            properties(QString::fromStdString(resourceName(e.type)), values);
            return;
        }
        QList<QPair<QString, QString>> values{{"ID", QString::number(id)},
                                              {"Type", QString("0x%1").arg(e.type, 4, 16, QChar('0'))},
                                              {"Device", QString::number(resource.device)}};
        if (!resources_->debugNames(id).isEmpty())
            values.append({"Name", resources_->debugNames(id)});
        if (e.type >= 0x8c && e.type <= 0x8f) {
            Reader r(frame_->payload(id));
            r.skip(16);
            auto target = r.read<Id>();
            values.append(QPair<QString, QString>{"Resource", QString::number(target)});
            values.append(QPair<QString, QString>{"Format", QString::number(r.read<uint32_t>())});
            values.append(QPair<QString, QString>{"Dimension", QString::number(r.read<uint32_t>())});
            properties(QString::fromStdString(resourceName(e.type)), values);
            return;
        }
        if (e.type >= 0x90 && e.type <= 0x95) {
            auto effective = experiment_->shaderBytes(*frame_, id);
            Bytes bytes(effective);
            shader_->setPlainText(QString::fromStdString(disassemble(bytes)));
            auto metadata = inspectResourceShader(*frame_, id, bytes);
            auto linkage = shaderClassLinkage(*frame_, id);
            values.append({"Interface slots", QString::number(metadata["interface_slots"].get<UINT>())});
            if (linkage)
                values.append({"Class linkage", QString::number(linkage)});
            sourceFiles_->clear();
            sourceEditor_->clear();
            shaderReflection_->clear();
            if (metadata.contains("stream_output")) {
                const auto &so = metadata["stream_output"];
                values.append({"SO declaration", QString::number(so["id"].get<Id>())});
                auto stream = so["rasterized_stream"].get<uint32_t>();
                values.append({"Rasterized stream", stream == UINT32_MAX ? "None" : QString::number(stream)});
                values.append({"SO strides", QString::fromStdString(so["strides"].dump())});
                if (metadata["passthrough"].get<bool>())
                    values.append(QPair<QString, QString>{"Execution", "Passthrough"});
                auto group = new QTreeWidgetItem(shaderReflection_, {"Stream Output", "", ""});
                for (const auto &element : so["entries"]) {
                    auto name = element["semantic"].is_null()
                                    ? QString("Gap")
                                    : QString::fromStdString(element["semantic"].get<std::string>()) +
                                          QString::number(element["index"].get<UINT>());
                    new QTreeWidgetItem(group, {name, QString::number(element["output_slot"].get<UINT>()),
                                                QString("Stream %1 · components %2–%3")
                                                    .arg(element["stream"].get<UINT>())
                                                    .arg(element["start_component"].get<UINT>())
                                                    .arg(element["start_component"].get<UINT>() +
                                                         element["component_count"].get<UINT>() - 1)});
                }
                group->setExpanded(true);
            }
            for (auto &file : metadata["embedded_sources"]["files"])
                sourceFiles_->addItem(QString::fromStdString(file["name"].get<std::string>()),
                                      QString::fromStdString(file["text"].get<std::string>()));
            shaderPane_->setCurrentIndex(sourceFiles_->count() ? 0 : 1);
            auto saved = experiment_->shaderSource(id);
            shaderEntry_->setText(QString::fromStdString(saved.value("source_entry", std::string("main"))));
            if (saved.contains("source_text")) {
                sourceFiles_->addItem("Applied HLSL",
                                      QString::fromStdString(saved["source_text"].get<std::string>()));
                sourceFiles_->setCurrentIndex(sourceFiles_->count() - 1);
                shaderPane_->setCurrentIndex(0);
            }
            sourceFiles_->setToolTip(
                QString::fromStdString(metadata["embedded_sources"]["status"].get<std::string>()));
            for (auto &binding : metadata["bindings"])
                new QTreeWidgetItem(shaderReflection_,
                                    {QString::fromStdString(binding["name"].get<std::string>()),
                                     QString::number(binding["slot"].get<UINT>()),
                                     QString("Type %1 · count %2")
                                         .arg(binding["type"].get<UINT>())
                                         .arg(binding["count"].get<UINT>())});
            for (auto &cb : metadata["constant_buffers"]) {
                auto group = new QTreeWidgetItem(shaderReflection_,
                                                 {QString::fromStdString(cb["name"].get<std::string>()), "",
                                                  QString("%1 B").arg(cb["size"].get<UINT>())});
                for (auto &v : cb["variables"])
                    new QTreeWidgetItem(group, {QString::fromStdString(v["name"].get<std::string>()),
                                                QString::number(v["offset"].get<UINT>()),
                                                QString("%1 B").arg(v["size"].get<UINT>())});
            }
            if (metadata["profile"].is_string())
                values.append(QPair<QString, QString>{
                    "Profile", QString::fromStdString(metadata["profile"].get<std::string>())});
            centerTabs_->setCurrentWidget(shaderPane_);
            values.append(QPair<QString, QString>{"DXBC bytes", QString::number(bytes.size())});
        }
        if (e.type == 0x83) {
            const QStringList names{"Byte width", "Usage",      "Bind flags",
                                    "CPU access", "Misc flags", "Structure stride"};
            for (size_t i = 0; i < resource.desc.size(); ++i)
                values.append(QPair<QString, QString>{names[int(i)], QString::number(resource.desc[i])});
            bufferOffset_->setText("0");
            bufferLength_->clear();
            bufferModel_->setBytes({});
            displayedBuffer_ = 0;
            centerTabs_->setCurrentWidget(bufferPane_);
            replayTimer_.stop();
            if (resource.data || bufferBoundary_->currentIndex() != 0) {
                bufferLabel_->clear();
                bufferTimer_.start();
            } else
                bufferLabel_->setText("  No initial bytes");
            values.append(QPair<QString, QString>{"Data", resource.data ? "Capture initial" : "Unavailable"});
        }
        if (e.type >= 0x84 && e.type <= 0x87) {
            auto info = textureInfo(resource);
            values.append(QPair<QString, QString>{
                "Dimensions", QString("%1 × %2 × %3").arg(info.width).arg(info.height).arg(info.depth)});
            values.append(QPair<QString, QString>{"Mip levels", QString::number(info.mips)});
            values.append(QPair<QString, QString>{"Array layers", QString::number(info.layers)});
            values.append(QPair<QString, QString>{"Format", QString::number(info.format)});
            values.append(QPair<QString, QString>{"Samples", QString::number(info.samples)});
            values.append(QPair<QString, QString>{"Data ID", QString::number(resource.data)});
            QSignalBlocker blockMip(mip_), blockLayer(layer_), blockSlice(slice_);
            QSignalBlocker blockSample(textureSample_), blockPlane(texturePlane_);
            textureSample_->setRange(-1, int(std::min(info.samples, 32u)) - 1);
            textureSample_->setValue(-1);
            textureSample_->setEnabled(info.samples > 1);
            texturePlane_->setCurrentIndex(0);
            texturePlane_->setEnabled(info.format >= 103 && info.format <= 105);
            textureFormat_->clear();
            textureExportAction_->setEnabled(false);
            mip_->setRange(0, int(info.mips) - 1);
            layer_->setRange(0, int(info.layers) - 1);
            slice_->setRange(0, int(info.depth) - 1);
            mip_->setValue(0);
            layer_->setValue(0);
            slice_->setValue(0);
            textureImage_->setImage({});
            centerTabs_->setCurrentWidget(texturePane_);
            replayTimer_.stop();
            textureTimer_.start();
        }
        properties(QString::fromStdString(resourceName(e.type)), values);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::updateStatistics() {
    statistics_->clear();
    uint64_t draws = 0, dispatches = 0, resources = 0, bytes = 0;
    std::map<std::string, uint64_t> kinds;
    for (auto &[id, e] : frame_->entries()) {
        if (e.category == 7 && isDraw(e.type)) {
            if (e.type == 0x35 || e.type == 0x36)
                ++dispatches;
            else
                ++draws;
        }
        if (e.category == 5) {
            ++resources;
            kinds[resourceName(e.type)]++;
        }
        bytes += e.size;
    }
    row(statistics_, "Draws", QString::number(draws));
    row(statistics_, "Dispatches", QString::number(dispatches));
    row(statistics_, "API calls", QString::number(commands_->rowCount()));
    row(statistics_, "Resources", QString::number(resources));
    row(statistics_, "Capture size", QString("%1 MiB").arg(double(frame_->size()) / 1048576., 0, 'f', 1));
    for (auto &[name, count] : kinds)
        row(statistics_, QString::fromStdString(name), QString::number(count));
}
void MainWindow::compileShader() {
    if (!frame_ || !selectedResource_ || busy())
        return;
    auto &entry = frame_->entry(selectedResource_);
    if (entry.type < 0x90 || entry.type > 0x95) {
        showError("Select a shader resource.");
        return;
    }
    if (sourceEditor_->toPlainText().trimmed().isEmpty()) {
        showError("HLSL source is empty.");
        return;
    }
    runningShader_ = selectedResource_;
    runningSource_ = sourceEditor_->toPlainText();
    runningEntry_ = shaderEntry_->text().trimmed();
    startWorker({"compile", capturePath_, "--id", QString::number(runningShader_)}, false);
}
void MainWindow::inspectGeometry() {
    if (!frame_ || !selectedEvent_ || busy())
        return;
    replayTimer_.stop();
    textureTimer_.stop();
    bufferTimer_.stop();
    startWorker({"geometry", capturePath_, "--event", QString::number(selectedEvent_)}, false);
}
void MainWindow::exportGeometry() {
    if (!geometryDir_)
        return;
    auto root = QFileDialog::getExistingDirectory(this, "Export geometry");
    if (root.isEmpty())
        return;
    auto base = root + "/FloraGPA-Geometry-" + geometry_["event"].toString(), path = base;
    for (int i = 1; QFileInfo::exists(path); ++i)
        path = base + '-' + QString::number(i);
    if (!QDir().mkpath(path)) {
        showError("Cannot create export directory.");
        return;
    }
    QDir source(geometryDir_->path() + "/result");
    for (auto file : source.entryList({"*.csv", "geometry.json", "geometry.obj"}, QDir::Files))
        if (!QFile::copy(source.filePath(file), path + '/' + file)) {
            showError("Geometry export failed.");
            return;
        }
    statusBar()->showMessage("Geometry exported", 3000);
}
void MainWindow::previewTexture() {
    if (!frame_ || !selectedResource_)
        return;
    const auto &entry = frame_->entry(selectedResource_);
    if (entry.type < 0x84 || entry.type > 0x87)
        return;
    if (process_.state() != QProcess::NotRunning) {
        cancel();
        textureTimer_.start();
        return;
    }
    ++revision_;
    textureExportAction_->setEnabled(false);
    textureImage_->setImage({});
    textureLabel_->clear();
    textureMetadata_ = {};
    textureDir_.reset();
    QStringList args{"texture", capturePath_,
                     "--id",    QString::number(selectedResource_),
                     "--mip",   QString::number(mip_->value()),
                     "--layer", QString::number(layer_->value()),
                     "--slice", QString::number(slice_->value())};
    args << "--channel" << textureChannels_->currentText().toLower() << "--low" << textureLow_->text()
         << "--high" << textureHigh_->text() << "--plane" << texturePlane_->currentData().toString();
    if (textureSample_->isEnabled() && textureSample_->value() >= 0)
        args << "--sample" << QString::number(textureSample_->value());
    if (!textureFormat_->text().trimmed().isEmpty()) {
        bool valid = false;
        const auto value = textureFormat_->text().trimmed().toULongLong(&valid, 0);
        if (!valid || value > UINT32_MAX) {
            showError("Invalid typed format.");
            return;
        }
        args << "--typed-format" << QString::number(value);
    }
    if (textureBoundary_->currentIndex()) {
        if (!selectedEvent_) {
            showError("Select an API event first.");
            return;
        }
        args << "--event" << QString::number(selectedEvent_);
        if (textureBoundary_->currentIndex() == 1)
            args << "--before";
    }
    startWorker(args, false);
}
void MainWindow::exportTexture() {
    if (!textureDir_ || textureMetadata_.isEmpty() || busy())
        return;
    const bool luma = textureMetadata_["recovered_luma_only"].toBool();
    const auto label = luma ? "Y plane" : "Texture";
    const auto filters = QString("%1 DDS (*.dds);;Preview PNG (*.png);;%2 RAW (*.bin)")
                             .arg(label)
                             .arg(luma ? "Y plane" : "Subresource");
    auto path = QFileDialog::getSaveFileName(
        this, "Export Texture", QString("texture-%1.dds").arg(textureMetadata_["resource_id"].toInteger()),
        filters);
    if (path.isEmpty())
        return;
    const auto suffix = QFileInfo(path).suffix().toLower();
    const auto files = textureMetadata_["export_files"].toObject();
    const auto source = files.value('.' + suffix).toString(files[".dds"].toString());
    try {
        QFile file(textureDir_->path() + "/result/" + source);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Texture export asset is unavailable");
        const auto bytes = file.readAll();
        if (bytes.size() != file.size())
            throw std::runtime_error("Cannot read complete texture asset");
        writeFile(path, bytes);
        statusBar()->showMessage("Texture exported", 3000);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::previewBuffer() {
    if (!frame_ || !selectedResource_ || frame_->entry(selectedResource_).type != 0x83)
        return;
    if (process_.state() != QProcess::NotRunning) {
        cancel();
        bufferTimer_.start();
        return;
    }
    bool valid = false;
    auto offset = bufferOffset_->text().toULongLong(&valid, 0);
    if (!valid) {
        showError("Invalid buffer offset.");
        return;
    }
    QStringList args{"buffer",   capturePath_,           "--id", QString::number(selectedResource_),
                     "--offset", QString::number(offset)};
    if (!bufferLength_->text().trimmed().isEmpty()) {
        auto length = bufferLength_->text().toULongLong(&valid, 0);
        if (!valid) {
            showError("Invalid buffer length.");
            return;
        }
        args << "--length" << QString::number(length);
    }
    if (bufferBoundary_->currentIndex()) {
        if (!selectedEvent_) {
            showError("Select an API event first.");
            return;
        }
        args << "--event" << QString::number(selectedEvent_);
        if (bufferBoundary_->currentIndex() == 1)
            args << "--before";
    }
    ++revision_;
    bufferModel_->setBytes({});
    clearBufferDetails();
    displayedBuffer_ = 0;
    bufferLabel_->clear();
    startWorker(args, false);
}
void MainWindow::exportBuffer() {
    if (!displayedBuffer_)
        return;
    auto path = QFileDialog::getSaveFileName(
        this, "Export buffer", QString("buffer-%1.bin").arg(displayedBuffer_), "Binary data (*.bin)");
    if (path.isEmpty())
        return;
    try {
        writeFile(path, bufferModel_->bytes());
        statusBar()->showMessage("Buffer exported", 3000);
    } catch (const std::exception &error) {
        showError(QString::fromUtf8(error.what()));
    }
}
void MainWindow::updateExperimentActions() {
    if (!undoAction_)
        return;
    undoAction_->setEnabled(experiment_ && experiment_->canUndo());
    redoAction_->setEnabled(experiment_ && experiment_->canRedo());
    const auto type = frame_ && selectedEvent_ ? frame_->entry(selectedEvent_).type : 0;
    bool editable = isDraw(uint16_t(type)) || isWritableCommand(uint16_t(type));
    enableAction_->setEnabled(editable);
    if (clearAction_)
        clearAction_->setEnabled(isClearCommand(uint16_t(type)));
    if (setterAction_)
        setterAction_->setEnabled(isEditableSetter(uint16_t(type)));
    if (depthStencilAction_)
        depthStencilAction_->setEnabled(isDraw(uint16_t(type)) && type != 0x35 && type != 0x36);
    if (rasterizerAction_)
        rasterizerAction_->setEnabled(isDraw(uint16_t(type)) && type != 0x35 && type != 0x36);
    if (blendAction_)
        blendAction_->setEnabled(isDraw(uint16_t(type)) && type != 0x35 && type != 0x36);
    if (samplerAction_)
        samplerAction_->setEnabled(isDraw(uint16_t(type)));
    if (viewAction_)
        viewAction_->setEnabled(frame_ && experiment_ && !busy());
    if (srvAction_)
        srvAction_->setEnabled(isDraw(uint16_t(type)));
    if (updateSourceAction_)
        updateSourceAction_->setEnabled(type == 0x247);
    const bool bufferEditable = isDraw(uint16_t(type)) && selectedResource_ && frame_ &&
                                frame_->entry(selectedResource_).type == 0x83;
    if (bufferEditAction_)
        bufferEditAction_->setEnabled(bufferEditable);
    if (bufferImportAction_)
        bufferImportAction_->setEnabled(bufferEditable);
    const auto resourceType = frame_ && selectedResource_ ? frame_->entry(selectedResource_).type : 0;
    const bool textureEditable =
        !busy() && experiment_ && isDraw(uint16_t(type)) && resourceType >= 0x84 && resourceType <= 0x86;
    if (textureInputAction_)
        textureInputAction_->setEnabled(textureEditable);
    if (textureOutputAction_)
        textureOutputAction_->setEnabled(textureEditable);
    if (constantEditAction_) {
        const auto item = constants_->currentItem();
        const auto field = item ? item->data(0, Qt::UserRole).toJsonObject() : QJsonObject();
        constantEditAction_->setEnabled(
            bufferEditable && !busy() && bufferDetailsRevision_ == revision_ &&
            bufferDetailsEvent_ == selectedEvent_ && bufferDetailsResource_ == selectedResource_ &&
            bufferBoundary_->currentIndex() == 1 && field["status"].toString() == "ready");
    }
    if (counterEditAction_) {
        const auto item = counters_->currentItem();
        counterEditAction_->setEnabled(
            experiment_ && !busy() && item && bufferDetailsRevision_ == revision_ &&
            bufferDetailsEvent_ == selectedEvent_ && bufferDetailsEvent_ &&
            bufferDetailsResource_ == selectedResource_ && bufferBoundary_->currentIndex() == 1);
    }
    enableAction_->setText(
        editable && experiment_ && !experiment_->enabled(selectedEvent_) ? "Enable Event" : "Disable Event");
}
void MainWindow::experimentChanged() {
    const auto shaderStage = selectedShaderStage_;
    ++outputGeneration_;
    replayedState_->invalidate();
    predicateView_->invalidate();
    projectDirty_ = true;
    setWindowModified(true);
    ++revision_;
    clearBufferDetails();
    chart_->clear();
    metrics_->clear();
    updateExperimentActions();
    if (selectedEvent_) {
        inspectEvent(selectedEvent_);
        if (isDraw(frame_->entry(selectedEvent_).type)) {
            ReplayOptions options;
            experiment_->apply(*frame_, options);
            const auto state = effectiveBindings(*frame_, selectedEvent_,
                                                 frame_->state(frame_->event(selectedEvent_).state), options);
            showPipeline(state);
            if (shaderStage) {
                selectedResource_ = state.stages[*shaderStage].shader;
                if (!selectedResource_) {
                    shader_->clear();
                    sourceEditor_->clear();
                    sourceFiles_->clear();
                    shaderReflection_->clear();
                    properties("Shader", {{"Resource", "None"}});
                }
            }
        }
    }
    if (process_.state() != QProcess::NotRunning)
        cancel();
    if (selectedResource_) {
        auto type = frame_->entry(selectedResource_).type;
        if (type >= 0x90 && type <= 0x95) {
            inspectResource(selectedResource_);
            selectedShaderStage_ = shaderStage;
        }
        if (type >= 0x84 && type <= 0x87 && centerTabs_->currentWidget() == texturePane_) {
            replayTimer_.stop();
            textureTimer_.start();
            return;
        }
        if (type == 0x83 && centerTabs_->currentWidget() == bufferPane_) {
            replayTimer_.stop();
            bufferTimer_.start();
            return;
        }
    }
    replayTimer_.start();
}
void MainWindow::clearBufferDetails() {
    bufferDetailsEvent_ = bufferDetailsResource_ = 0;
    bufferDetailsRevision_ = 0;
    constants_->clear();
    counters_->clear();
    constantEditAction_->setEnabled(false);
    counterEditAction_->setEnabled(false);
}
void MainWindow::showBufferDetails(const QJsonObject &report) {
    clearBufferDetails();
    bufferDetailsRevision_ = revision_;
    bufferDetailsEvent_ =
        report["value_time"].toString() == "before_event" ? report["event"].toString().toULongLong() : 0;
    bufferDetailsResource_ = report["resource"].toString().toULongLong();
    for (const auto &entry : report["constant_bindings"].toArray()) {
        const auto binding = entry.toObject();
        auto title =
            QString("%1 · b%2").arg(binding["stage"].toString().toUpper()).arg(binding["slot"].toInt());
        if (!binding["name"].toString().isEmpty())
            title += " · " + binding["name"].toString();
        auto group = new QTreeWidgetItem(constants_, {title});
        group->setToolTip(0, QString("Shader %1 · first constant %2 · count %3")
                                 .arg(binding["shader"].toInteger())
                                 .arg(binding["first_constant"].toInteger())
                                 .arg(binding["constant_count"].isNull()
                                          ? "All"
                                          : QString::number(binding["constant_count"].toInteger())));
        for (const auto &v : binding["variables"].toArray()) {
            auto variable = v.toObject();
            auto fields = variable["fields"].toArray();
            auto parent =
                fields.size() > 1 ? new QTreeWidgetItem(group, {variable["name"].toString()}) : group;
            for (const auto &f : fields) {
                auto field = f.toObject();
                const auto json = nlohmann::json::parse(QJsonDocument(field).toJson().toStdString());
                auto ready = field["status"].toString() == "ready";
                auto value = ready ? QString::fromStdString(constantValue(json).dump())
                             : field["status"].toString() == "outside_bound_range" ? "Outside bound range"
                                                                                   : "Unsupported layout";
                auto item =
                    new QTreeWidgetItem(parent, {field["path"].toString(), field["type_label"].toString(),
                                                 field.contains("buffer_offset")
                                                     ? QString::number(field["buffer_offset"].toInteger())
                                                     : "—",
                                                 value});
                item->setData(0, Qt::UserRole, field);
                item->setToolTip(3, ready ? value : field["reason"].toString(value));
            }
        }
        group->setExpanded(true);
    }
    for (const auto &entry : report["uav_counters"].toArray()) {
        const auto counter = entry.toObject();
        QStringList bindings;
        for (const auto &bound : counter["bindings"].toArray()) {
            auto binding = bound.toObject();
            bindings
                << QString("%1 u%2").arg(binding["stage"].toString().toUpper()).arg(binding["slot"].toInt());
        }
        auto item = new QTreeWidgetItem(
            counters_, {QString::number(counter["view"].toInteger()),
                        counter["kind"].toString() == "append_consume" ? "Append / Consume" : "Counter",
                        bindings.isEmpty() ? "Command reference" : bindings.join(", "),
                        QString::number(counter["value"].toInteger()),
                        QString::number(counter["num_elements"].toInteger()),
                        QString::number(counter["stride"].toInteger())});
        item->setData(0, Qt::UserRole, counter);
        item->setToolTip(4, QString("First element %1").arg(counter["first_element"].toInteger()));
        QStringList references;
        for (const auto &field : counter["reference_fields"].toArray())
            references << field.toString();
        item->setToolTip(2, references.join(", "));
    }
    updateExperimentActions();
}
void MainWindow::editCounter() {
    updateExperimentActions();
    if (!counterEditAction_->isEnabled())
        return;
    const auto counter = counters_->currentItem()->data(0, Qt::UserRole).toJsonObject();
    const auto view = Id(counter["view"].toInteger()), event = bufferDetailsEvent_,
               resource = bufferDetailsResource_;
    const auto revision = revision_;
    QDialog dialog(this);
    dialog.setObjectName("counterDialog");
    dialog.setWindowTitle(QString("UAV Counter — %1").arg(view));
    dialog.setMinimumWidth(370);
    auto form = new QFormLayout(&dialog);
    form->addRow("Buffer", new QLabel(QString::number(resource)));
    auto scope = new QComboBox;
    scope->setObjectName("counterScope");
    const bool eventEditable = isDraw(frame_->entry(event).type) && !counter["bindings"].toArray().isEmpty();
    if (eventEditable)
        scope->addItem("Before event", false);
    scope->addItem("Frame initial", true);
    scope->setToolTip(
        "Frame initial applies when the view is created. Captured resets still take precedence.");
    form->addRow("Scope", scope);
    auto value = new QLineEdit(QString::number(counter["value"].toInteger()));
    value->setObjectName("counterValue");
    value->setToolTip("Unsigned 32-bit counter (decimal or 0x hex)");
    form->addRow("Value", value);
    connect(scope, &QComboBox::currentIndexChanged, &dialog, [&] {
        auto seed = experiment_->initialUavCounter(view);
        value->setText(
            QString::number(scope->currentData().toBool() && seed ? *seed : counter["value"].toInteger()));
    });
    auto error = new QLabel;
    error->setWordWrap(true);
    error->hide();
    form->addRow(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool changed = false;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            if (revision != revision_ || event != selectedEvent_ || resource != selectedResource_ || busy())
                throw std::runtime_error("Read Before event again before editing");
            const auto text = value->text().trimmed();
            bool valid = false;
            const auto number = text.toULongLong(&valid, 0);
            if (!valid || text.startsWith('-') || number > UINT32_MAX)
                throw std::runtime_error("Enter an unsigned 32-bit counter");
            const auto initial = scope->currentData().toBool();
            if (initial || number != uint64_t(counter["value"].toInteger()))
                changed = experiment_->setUavCounter(*frame_, view, uint32_t(number),
                                                     initial ? std::nullopt : std::optional<Id>(event));
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        if (changed)
            experimentChanged();
        else
            statusBar()->showMessage("Counter unchanged", 2500);
    }
}
void MainWindow::editConstant() {
    updateExperimentActions();
    if (!constantEditAction_->isEnabled())
        return;
    try {
        auto field = nlohmann::json::parse(
            QJsonDocument(constants_->currentItem()->data(0, Qt::UserRole).toJsonObject())
                .toJson()
                .toStdString());
        field["value"] = constantValue(field);
        const auto revision = revision_;
        const auto event = bufferDetailsEvent_, resource = bufferDetailsResource_;
        const unsigned cls = field.at("class_id"), rows = field.at("rows"), cols = field.at("columns");
        QDialog dialog(this);
        dialog.setObjectName("constantDialog");
        dialog.setWindowTitle(QString::fromStdString(field.at("path").get<std::string>()));
        dialog.setMinimumWidth(400);
        auto form = new QFormLayout(&dialog);
        form->addRow("Type", new QLabel(QString::fromStdString(field.at("type_label").get<std::string>())));
        form->addRow("Binding", new QLabel(QString("Event %1 · Buffer %2").arg(event).arg(resource)));
        auto grid = new QGridLayout;
        QList<QLineEdit *> editors;
        for (unsigned r = 0; r < rows; ++r) {
            for (unsigned c = 0; c < cols; ++c) {
                const auto value = cls >= 2   ? field.at("value").at(r).at(c)
                                   : cls == 1 ? field.at("value").at(c)
                                              : field.at("value");
                auto editor = new QLineEdit(
                    QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump()));
                editor->setObjectName(QString("constantValue%1").arg(editors.size()));
                editor->setMinimumWidth(85);
                editor->setFont(QFont("Cascadia Mono", 10));
                editor->setToolTip(QString("Offset %1 · %2\nNumber, true/false, nan, inf, -inf or bits:HEX")
                                       .arg(field.at("component_offsets").at(editors.size()).get<uint64_t>())
                                       .arg(QString::fromStdString(
                                           field.at("component_hex").at(editors.size()).get<std::string>())));
                grid->addWidget(editor, int(r), int(c));
                editors.append(editor);
            }
        }
        form->addRow("Value", grid);
        auto error = new QLabel;
        error->setWordWrap(true);
        error->hide();
        form->addRow(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        form->addRow(buttons);
        bool changed = false;
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                if (revision != revision_ || event != selectedEvent_ || resource != selectedResource_ ||
                    busy())
                    throw std::runtime_error("Read Before event again before editing");
                auto values = nlohmann::json::array();
                for (auto editor : editors) {
                    auto text = editor->text().trimmed().toStdString();
                    values.push_back(text.starts_with("bits:") || text == "nan" || text == "inf" ||
                                             text == "-inf"
                                         ? nlohmann::json(text)
                                         : nlohmann::json::parse(text));
                }
                nlohmann::json value;
                if (cls >= 2) {
                    value = nlohmann::json::array();
                    for (unsigned r = 0; r < rows; ++r)
                        value.push_back(
                            nlohmann::json(values.begin() + r * cols, values.begin() + (r + 1) * cols));
                } else
                    value = cls == 1 ? values : values[0];
                auto patches = constantPatches(field, value);
                experiment_->setBufferPatches(*frame_, event, resource, patches,
                                              "Constant " + field.at("path").get<std::string>());
                changed = !patches.empty();
                dialog.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
                error->show();
            }
        });
        if (dialog.exec() == QDialog::Accepted) {
            if (changed)
                experimentChanged();
            else
                statusBar()->showMessage("Value unchanged", 2500);
        }
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editTexture(bool output) {
    if (!frame_ || !experiment_ || !selectedEvent_ || !selectedResource_ || busy())
        return;
    try {
        const auto event = selectedEvent_, resource = selectedResource_, revision = revision_;
        const auto info = textureInfo(frame_->resource(resource));
        TexturePatch patch;
        patch.mip = uint32_t(mip_->value());
        patch.layer = uint32_t(layer_->value());
        const auto subs = textureSubresources(frame_->resource(resource));
        const auto sub = std::find_if(subs.begin(), subs.end(), [&](const auto &s) {
            return s.mip == patch.mip && s.layer == patch.layer;
        });
        if (sub == subs.end())
            throw std::runtime_error("Texture subresource out of bounds");
        ReplayOptions options;
        experiment_->apply(*frame_, options);
        const auto &effective = effectiveFrame(*frame_, options);
        const auto command = effective.event(event);
        const auto state = effectiveBindings(effective, event, effective.state(command.state), options);
        validateTextureBinding(effective, command, state, resource, patch, output);
        textureTimer_.stop();
        replayTimer_.stop();
        QDialog dialog(this);
        dialog.setObjectName("textureEditDialog");
        dialog.setWindowTitle(QString("Texture %1 — Event %2").arg(resource).arg(event));
        dialog.setMinimumWidth(440);
        auto form = new QFormLayout(&dialog);
        auto scope = new QLabel(output ? "Output precondition" : "Event input");
        scope->setToolTip(output ? "Retained only after command submission."
                                 : "Original input storage is restored after the event.");
        form->addRow("Scope", scope);
        form->addRow(
            "Subresource",
            new QLabel(
                QString("Mip %1 · Layer %2 · %3 bytes").arg(patch.mip).arg(patch.layer).arg(sub->size)));
        QSpinBox *sample = nullptr;
        QLineEdit *typed = nullptr;
        if (info.samples > 1) {
            sample = new QSpinBox;
            sample->setObjectName("textureEditSample");
            sample->setRange(-1, int(info.samples) - 1);
            sample->setSpecialValueText("Select…");
            sample->setValue(textureSample_->value());
            form->addRow("Sample", sample);
            typed = new QLineEdit;
            typed->setObjectName("textureEditFormat");
            typed->setText(textureFormat_->text());
            typed->setPlaceholderText("Automatic");
            typed->setToolTip("Optional DXGI format number (decimal or 0x hex).");
            form->addRow("Typed format", typed);
        }
        auto fileRow = new QWidget;
        auto row = new QHBoxLayout(fileRow);
        row->setContentsMargins(0, 0, 0, 0);
        auto path = new QLineEdit;
        path->setObjectName("textureEditFile");
        path->setToolTip("Packed RAW subresource without a DDS header; a 3D mip includes all depth slices.");
        auto browse = new QPushButton("Browse…");
        row->addWidget(path, 1);
        row->addWidget(browse);
        form->addRow("RAW file", fileRow);
        connect(browse, &QPushButton::clicked, &dialog, [&] {
            const auto chosen = QFileDialog::getOpenFileName(&dialog, "Import Texture RAW", {},
                                                             "Raw data (*.bin *.raw);;All files (*)");
            if (!chosen.isEmpty())
                path->setText(chosen);
        });
        auto error = new QLabel;
        error->setObjectName("textureEditError");
        error->setWordWrap(true);
        error->hide();
        form->addRow(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                if (!frame_ || !experiment_ || revision_ != revision || selectedEvent_ != event ||
                    selectedResource_ != resource)
                    throw std::runtime_error("Selection changed; reopen the texture editor");
                if (sample) {
                    if (sample->value() < 0)
                        throw std::runtime_error("Select an explicit MSAA sample");
                    patch.sample = uint32_t(sample->value());
                    patch.typedFormat.reset();
                    if (!typed->text().trimmed().isEmpty()) {
                        bool ok = false;
                        auto value = typed->text().trimmed().toULongLong(&ok, 0);
                        if (!ok || value > UINT32_MAX)
                            throw std::runtime_error("Invalid typed format");
                        patch.typedFormat = uint32_t(value);
                    }
                }
                QFile file(path->text());
                if (!file.open(QIODevice::ReadOnly))
                    throw std::runtime_error("Cannot open texture RAW file");
                if (uint64_t(file.size()) != sub->size)
                    throw std::runtime_error("RAW byte count does not match the subresource");
                const auto data = file.readAll();
                if (uint64_t(data.size()) != sub->size)
                    throw std::runtime_error("Cannot read complete texture RAW file");
                patch.bytes.assign(data.begin(), data.end());
                experiment_->setTexturePatch(*frame_, event, resource, patch, output);
                dialog.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
                error->show();
            }
        });
        if (dialog.exec() == QDialog::Accepted)
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editBuffer(bool importFile) {
    if (!frame_ || !experiment_ || !selectedEvent_ || !selectedResource_)
        return;
    try {
        auto event = selectedEvent_, resource = selectedResource_;
        auto command = frame_->event(event);
        ReplayOptions options;
        experiment_->apply(*frame_, options);
        const auto state = effectiveBindings(*frame_, event, frame_->state(command.state), options);
        auto bindings = bufferBindings(effectiveFrame(*frame_, options), command, state, resource);
        if (frame_->resource(resource).type != 0x83 || bindings.empty())
            throw std::runtime_error("Select a buffer bound to this draw or dispatch");
        QByteArray imported;
        QString path;
        if (importFile) {
            path = QFileDialog::getOpenFileName(this, "Import Buffer Patch", {},
                                                "Binary data (*.bin);;All files (*)");
            if (path.isEmpty())
                return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot open buffer patch");
            if (file.size() <= 0 || uint64_t(file.size()) > frame_->resource(resource).desc.at(0))
                throw std::runtime_error("Buffer patch is empty or larger than the resource");
            imported = file.readAll();
            if (imported.size() != file.size())
                throw std::runtime_error("Cannot read the complete buffer patch");
        }
        QDialog dialog(this);
        dialog.setObjectName("bufferEditDialog");
        dialog.setWindowTitle(QString("Buffer %1 — Event %2").arg(resource).arg(event));
        dialog.setMinimumWidth(480);
        auto form = new QFormLayout(&dialog);
        auto effect = new QLabel(persistentBufferEdit(bindings) ? "Persistent output" : "Event input");
        effect->setToolTip(persistentBufferEdit(bindings)
                               ? "Writes persist only after command submission."
                               : "The original buffer is restored after this command.");
        form->addRow("Scope", effect);
        auto offset = new QLineEdit(bufferOffset_->text());
        offset->setObjectName("bufferEditOffset");
        offset->setToolTip("Byte offset (decimal or 0x hex)");
        form->addRow("Offset", offset);
        QPlainTextEdit *hexEditor = nullptr;
        if (importFile) {
            form->addRow("File", new QLabel(QFileInfo(path).fileName()));
            form->addRow("Bytes", new QLabel(QString::number(imported.size())));
        } else {
            hexEditor = new QPlainTextEdit;
            hexEditor->setObjectName("bufferEditHex");
            hexEditor->setFont(QFont("Cascadia Mono", 10));
            hexEditor->setMinimumHeight(100);
            hexEditor->setPlaceholderText("00 00 80 3f");
            if (displayedBuffer_ == resource) {
                const auto stride = bufferMode_->currentIndex() == 1 ? 4 : 16;
                const auto start = std::max(0, bufferView_->currentIndex().row()) * qsizetype(stride);
                offset->setText(QString::number(bufferModel_->offset() + uint64_t(start)));
                hexEditor->setPlainText(
                    QString::fromLatin1(bufferModel_->bytes().mid(start, stride).toHex(' ')));
            }
            form->addRow("Hex bytes", hexEditor);
        }
        auto error = new QLabel;
        error->setWordWrap(true);
        error->hide();
        form->addRow(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                bool ok = false;
                auto text = offset->text().trimmed();
                auto position = text.toULongLong(&ok, 0);
                if (!ok || text.startsWith('-'))
                    throw std::runtime_error("Enter a valid unsigned byte offset");
                QByteArray bytes = imported;
                if (hexEditor) {
                    auto hex = hexEditor->toPlainText();
                    hex.remove(QRegularExpression("\\s"));
                    if (hex.isEmpty() || hex.size() % 2 ||
                        !QRegularExpression("^[0-9a-fA-F]+$").match(hex).hasMatch())
                        throw std::runtime_error("Enter complete hexadecimal byte pairs");
                    bytes = QByteArray::fromHex(hex.toLatin1());
                }
                experiment_->setBuffer(
                    *frame_, event, resource, position,
                    {reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())});
                dialog.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
                error->show();
            }
        });
        if (dialog.exec() == QDialog::Accepted) {
            QSignalBlocker blocker(bufferBoundary_);
            bufferBoundary_->setCurrentIndex(1);
            experimentChanged();
        }
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editView() {
    if (!frame_ || !experiment_ || busy())
        return;
    try {
        const auto capture = frame_;
        const auto revision = revision_;
        auto check = [&] {
            if (capture != frame_ || revision != revision_ || busy())
                throw std::runtime_error("Capture or experiment changed; reopen this editor");
        };
        if (editViewDialog(
                this, *capture, selectedResource_,
                [&](Id view) {
                    check();
                    return experiment_->view(*capture, view);
                },
                [&](Id view, const nlohmann::json &values) {
                    check();
                    experiment_->setView(*capture, view, values);
                }))
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editSrv() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        const auto event = selectedEvent_;
        const auto revision = revision_;
        auto check = [&] {
            if (revision != revision_ || event != selectedEvent_)
                throw std::runtime_error("Selection or experiment changed; reopen this editor");
        };
        if (editSrvDialog(
                this,
                [&](const std::string &stage, unsigned slot) {
                    check();
                    return experiment_->srv(*frame_, event, stage, slot);
                },
                [&](const std::string &stage, unsigned slot, const nlohmann::json &values) {
                    check();
                    experiment_->setSrv(*frame_, event, stage, slot, values);
                }))
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editSampler() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        const auto event = selectedEvent_;
        const auto revision = revision_;
        auto check = [&] {
            if (revision != revision_ || event != selectedEvent_)
                throw std::runtime_error("Selection or experiment changed; reopen this editor");
        };
        if (editSamplerDialog(
                this,
                [&](const std::string &stage, unsigned slot) {
                    check();
                    return experiment_->sampler(*frame_, event, stage, slot);
                },
                [&](const std::string &stage, unsigned slot, const nlohmann::json &patch) {
                    check();
                    experiment_->setSampler(*frame_, event, stage, slot, patch);
                }))
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editBlend() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        const auto event = selectedEvent_;
        const auto revision = revision_;
        auto initial = experiment_->blend(*frame_, event);
        if (editBlendDialog(this, initial, [&](const nlohmann::json &patch) {
                if (revision != revision_ || event != selectedEvent_)
                    throw std::runtime_error("Selection or experiment changed; reopen this editor");
                experiment_->setBlend(*frame_, event, patch);
            }))
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editRasterizer() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        auto event = selectedEvent_;
        auto revision = revision_;
        auto initial = experiment_->rasterizer(*frame_, event);
        if (editRasterizerDialog(this, initial, [&](const nlohmann::json &patch) {
                if (revision != revision_ || event != selectedEvent_)
                    throw std::runtime_error("Selection or experiment changed; reopen this editor");
                experiment_->setRasterizer(*frame_, event, patch);
            }))
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editDepthStencil() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        const auto event = selectedEvent_;
        const auto revision = revision_;
        const auto original = experiment_->depthStencil(*frame_, event);
        QDialog dialog(this);
        dialog.setObjectName("depthStencilDialog");
        dialog.setWindowTitle(QString("Depth / Stencil — Event %1").arg(event));
        dialog.setMinimumWidth(420);
        auto layout = new QVBoxLayout(&dialog);
        auto tabs = new QTabWidget;
        layout->addWidget(tabs);
        struct Field {
            std::string path;
            QWidget *widget;
        };
        std::vector<Field> fields;
        const QStringList comparisons{"Never",   "Less",      "Equal",           "Less / equal",
                                      "Greater", "Not equal", "Greater / equal", "Always"};
        const QStringList operations{"Keep",
                                     "Zero",
                                     "Replace",
                                     "Increment (clamp)",
                                     "Decrement (clamp)",
                                     "Invert",
                                     "Increment (wrap)",
                                     "Decrement (wrap)"};
        auto page = [&](const QString &label) {
            auto widget = new QWidget;
            tabs->addTab(widget, label);
            return new QFormLayout(widget);
        };
        auto add = [&](QFormLayout *form, const QString &label, const std::string &path,
                       const QStringList &choices = QStringList{}, int first = 0) {
            auto value = original.at(nlohmann::json::json_pointer(path));
            QWidget *widget;
            if (!choices.empty()) {
                auto combo = new QComboBox;
                for (int i = 0; i < choices.size(); ++i)
                    combo->addItem(choices[i], i + first);
                combo->setCurrentIndex((value.is_boolean() ? int(value.get<bool>()) : value.get<int>()) -
                                       first);
                widget = combo;
            } else
                widget = new QLineEdit(QString::fromStdString(value.dump()));
            widget->setObjectName(QString::fromStdString(path));
            fields.push_back({path, widget});
            form->addRow(label, widget);
        };
        auto depth = page("Depth / Stencil");
        add(depth, "Depth test", "/depth_stencil/depth_enable", {"Disabled", "Enabled"});
        add(depth, "Depth writes", "/depth_stencil/depth_write_mask", {"Disabled", "Enabled"});
        add(depth, "Depth function", "/depth_stencil/depth_func", comparisons, 1);
        add(depth, "Stencil test", "/depth_stencil/stencil_enable", {"Disabled", "Enabled"});
        add(depth, "Read mask", "/depth_stencil/stencil_read_mask");
        add(depth, "Write mask", "/depth_stencil/stencil_write_mask");
        add(depth, "Reference", "/stencil_ref");
        for (auto face : {"front_face", "back_face"}) {
            auto form = page(QString(face == std::string("front_face") ? "Front face" : "Back face"));
            auto prefix = std::string("/depth_stencil/") + face + '/';
            add(form, "Function", prefix + "func", comparisons, 1);
            add(form, "Stencil fail", prefix + "fail_op", operations, 1);
            add(form, "Depth fail", prefix + "depth_fail_op", operations, 1);
            add(form, "Pass", prefix + "pass_op", operations, 1);
        }
        auto error = new QLabel;
        error->setObjectName("depthStencilError");
        error->setWordWrap(true);
        error->hide();
        layout->addWidget(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        bool changed = false;
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                if (revision_ != revision || selectedEvent_ != event)
                    throw std::runtime_error("Selection or experiment changed; reopen this editor");
                auto patch = nlohmann::json::object();
                for (const auto &field : fields) {
                    const nlohmann::json::json_pointer key(field.path);
                    nlohmann::json value;
                    if (auto combo = qobject_cast<QComboBox *>(field.widget)) {
                        if (combo->currentIndex() < 0)
                            throw std::runtime_error("Select a valid state value");
                        value = original.at(key).is_boolean()
                                    ? nlohmann::json(bool(combo->currentData().toInt()))
                                    : nlohmann::json(combo->currentData().toUInt());
                    } else {
                        auto text = qobject_cast<QLineEdit *>(field.widget)->text().trimmed();
                        bool valid = false;
                        auto number =
                            text.toULongLong(&valid, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
                        if (!valid || text.startsWith('-'))
                            throw std::runtime_error("Enter an unsigned mask or reference");
                        value = uint64_t(number);
                    }
                    if (value != original.at(key))
                        patch[key] = value;
                }
                if (!patch.empty()) {
                    experiment_->setDepthStencil(*frame_, event, patch);
                    changed = true;
                }
                dialog.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
                error->show();
            }
        });
        if (dialog.exec() == QDialog::Accepted && changed)
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editSetter() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        const auto event = selectedEvent_;
        const auto values = experiment_->setter(*frame_, event);
        if (constantBufferStage(frame_->entry(event).type)) {
            const auto revision = revision_;
            auto frame = frame_;
            if (editConstantBufferDialog(this, *frame, event, values, [&](const nlohmann::json &next) {
                    if (revision != revision_ || event != selectedEvent_)
                        throw std::runtime_error("Selection or experiment changed; reopen this editor");
                    experiment_->setSetter(*frame_, event, next);
                }))
                experimentChanged();
            return;
        }
        if (isIaSetter(frame_->entry(event).type)) {
            const auto revision = revision_;
            auto frame = frame_;
            if (editIaSetterDialog(this, *frame, event, values, [&](const nlohmann::json &next) {
                    if (revision != revision_ || event != selectedEvent_)
                        throw std::runtime_error("Selection or experiment changed; reopen this editor");
                    experiment_->setSetter(*frame_, event, next);
                }))
                experimentChanged();
            return;
        }
        if (isPipelineSetter(frame_->entry(event).type)) {
            const auto revision = revision_;
            auto frame = frame_;
            if (editPipelineSetterDialog(this, *frame, event, values, [&](const nlohmann::json &next) {
                    if (revision != revision_ || event != selectedEvent_)
                        throw std::runtime_error("Selection or experiment changed; reopen this editor");
                    experiment_->setSetter(*frame_, event, next);
                }))
                experimentChanged();
            return;
        }
        if (isSrvOutputCommand(frame_->entry(event).type)) {
            const auto revision = revision_;
            auto frame = frame_;
            if (editOutputSetterDialog(this, *frame, event, values, [&](const nlohmann::json &next) {
                    if (revision != revision_ || event != selectedEvent_)
                        throw std::runtime_error("Selection or experiment changed; reopen this editor");
                    experiment_->setSetter(*frame_, event, next);
                }))
                experimentChanged();
            return;
        }
        if (samplerSetterStage(frame_->entry(event).type) || srvSetterStage(frame_->entry(event).type)) {
            const auto revision = revision_;
            auto frame = frame_;
            if (editResourceSetterDialog(this, *frame, event, values, [&](const nlohmann::json &next) {
                    if (revision != revision_ || event != selectedEvent_)
                        throw std::runtime_error("Selection or experiment changed; reopen this editor");
                    experiment_->setSetter(*frame_, event, next);
                }))
                experimentChanged();
            return;
        }
        QDialog dialog(this);
        dialog.setObjectName("setterDialog");
        dialog.setWindowTitle(QString("SetPredication — Event %1").arg(event));
        dialog.setMinimumWidth(360);
        auto layout = new QFormLayout(&dialog);
        auto predicate = new QComboBox;
        predicate->setObjectName("setterPredicate");
        predicate->addItem("None", QVariant::fromValue(qulonglong(0)));
        for (const auto &[id, entry] : frame_->entries())
            if (entry.category == 5 && entry.type == 0x96)
                predicate->addItem(QString("Predicate %1").arg(id), QVariant::fromValue(qulonglong(id)));
        const auto id = values.at("predicate").get<Id>();
        int index = predicate->findData(QVariant::fromValue(qulonglong(id)));
        if (index < 0) {
            predicate->addItem(QString("Missing %1").arg(id), QVariant::fromValue(qulonglong(id)));
            index = predicate->count() - 1;
        }
        predicate->setCurrentIndex(index);
        layout->addRow("Predicate", predicate);
        auto value = new QLineEdit(QString::number(values.at("predicate_value").get<uint32_t>()));
        value->setObjectName("setterPredicateValue");
        value->setToolTip("Raw uint32 BOOL; decimal or 0x hexadecimal. Zero and one are canonical values.");
        layout->addRow("Predicate value", value);
        auto error = new QLabel;
        error->setObjectName("setterError");
        error->setWordWrap(true);
        error->hide();
        layout->addRow(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                const auto text = value->text().trimmed();
                bool valid = false;
                const auto bits =
                    text.toULongLong(&valid, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
                if (!valid || text.startsWith('-') || bits > UINT32_MAX)
                    throw std::runtime_error("Enter a uint32 value");
                experiment_->setSetter(*frame_, event,
                                       {{"predicate", uint64_t(predicate->currentData().toULongLong())},
                                        {"predicate_value", uint32_t(bits)}});
                dialog.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
                error->show();
            }
        });
        if (dialog.exec() == QDialog::Accepted)
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::editClear() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        auto event = selectedEvent_;
        auto values = experiment_->clear(*frame_, event);
        bool depth = frame_->entry(event).type == 0x31;
        bool integer = frame_->entry(event).type == 0x33;
        QDialog dialog(this);
        dialog.setObjectName("clearDialog");
        dialog.setWindowTitle(QString("Clear Values — Event %1").arg(event));
        dialog.setMinimumWidth(340);
        auto layout = new QFormLayout(&dialog);
        QList<QLineEdit *> fields;
        const QStringList names = depth ? QStringList{"Flags", "Depth", "Stencil"}
                                        : QStringList{"X / R", "Y / G", "Z / B", "W / A"};
        for (int i = 0; i < names.size(); ++i) {
            nlohmann::json value = depth ? values.at(i == 0   ? "flags"
                                                     : i == 1 ? "depth"
                                                              : "stencil")
                                         : values["values"][i];
            auto field = new QLineEdit(QString::fromStdString(value.dump()));
            field->setObjectName(QString("clearValue%1").arg(i));
            if (depth && i == 0)
                field->setToolTip("1 = depth, 2 = stencil, 3 = both");
            fields.append(field);
            layout->addRow(names[i], field);
        }
        auto error = new QLabel;
        error->setWordWrap(true);
        error->hide();
        layout->addRow(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                nlohmann::json edited;
                if (!depth)
                    edited["values"] = nlohmann::json::array();
                for (int i = 0; i < fields.size(); ++i) {
                    bool ok = false;
                    nlohmann::json value;
                    auto text = fields[i]->text().trimmed();
                    if (integer || (depth && i != 1)) {
                        auto number = text.toULongLong(&ok);
                        if (text.startsWith('-'))
                            ok = false;
                        value = uint64_t(number);
                    } else
                        value = text.toDouble(&ok);
                    if (!ok)
                        throw std::runtime_error("Enter a valid numeric value");
                    if (depth)
                        edited[i == 0 ? "flags" : i == 1 ? "depth" : "stencil"] = value;
                    else
                        edited["values"].push_back(value);
                }
                experiment_->setClear(*frame_, event, edited);
                dialog.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
                error->show();
            }
        });
        if (dialog.exec() == QDialog::Accepted)
            experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::replaceUpdateSource() {
    if (!frame_ || !experiment_ || !selectedEvent_)
        return;
    try {
        auto event = selectedEvent_;
        auto layout = updateSourceLayout(*frame_, event);
        auto path = QFileDialog::getOpenFileName(this, QString("Update Source — %1 bytes").arg(layout.size),
                                                 {}, "Binary data (*.bin);;All files (*)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || uint64_t(file.size()) != layout.size)
            throw std::runtime_error("Update source must contain " + std::to_string(layout.size) +
                                     " tightly packed bytes");
        auto bytes = file.readAll();
        experiment_->setUpdateSource(
            *frame_, event, {reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())});
        experimentChanged();
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::openExperiment() {
    if (busy())
        return;
    if (projectDirty_) {
        auto answer = QMessageBox::question(this, "Unsaved experiment", "Save experiment changes?",
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !saveExperiment()))
            return;
    }
    if (!frame_)
        return;
    auto path = QFileDialog::getOpenFileName(this, "Open experiment", {}, "FloraGPA experiments (*.json)");
    if (path.isEmpty())
        return;
    try {
        auto candidate = std::make_unique<Experiment>(*frame_);
        candidate->load(path, *frame_);
        const auto settings = replayUiState(
            *frame_, candidate->document().value("ui", nlohmann::json::object()), selectedEvent_);
        {
            QSignalBlocker target(outputTarget_), channel(channels_), layer(outputLayer_),
                sample(outputSample_), adapter(adapter_);
            outputTarget_->setCurrentIndex(outputTarget_->findData(QString::fromStdString(settings.target)));
            channels_->setCurrentText(QString::fromStdString(settings.channel).toUpper());
            outputLow_->setText(QString::fromStdString(settings.low));
            outputHigh_->setText(QString::fromStdString(settings.high));
            outputLayer_->setValue(settings.layer ? int(*settings.layer) : -1);
            outputSample_->setValue(settings.sample ? int(*settings.sample) : -1);
            adapter_->setCurrentIndex(settings.warp ? 1 : 0);
        }
        experiment_ = std::move(candidate);
        projectPath_ = path;
        if (settings.event)
            locateEvent(settings.event);
        {
            QSignalBlocker boundary(boundary_);
            boundary_->setCurrentIndex(settings.boundary);
        }
        centerTabs_->setCurrentWidget(image_->parentWidget());
        experimentChanged();
        projectDirty_ = false;
        setWindowModified(false);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
bool MainWindow::saveExperiment() {
    if (!experiment_)
        return false;
    auto path = projectPath_;
    if (path.isEmpty())
        path = QFileDialog::getSaveFileName(this, "Save experiment",
                                            QFileInfo(capturePath_).completeBaseName() + ".flora.json",
                                            "FloraGPA experiments (*.json)");
    if (path.isEmpty())
        return false;
    try {
        ReplayUiState state;
        state.target = outputTarget_->currentData().toString().toStdString();
        state.channel = channels_->currentText().toLower().toStdString();
        state.low = outputLow_->text().toStdString();
        state.high = outputHigh_->text().toStdString();
        if (outputLayer_->value() >= 0)
            state.layer = uint32_t(outputLayer_->value());
        if (outputSample_->value() >= 0)
            state.sample = uint32_t(outputSample_->value());
        state.warp = adapter_->currentIndex() == 1;
        state.event = selectedEvent_;
        state.boundary = boundary_->currentIndex();
        experiment_->save(
            path,
            replayUiDocument(*frame_, state, experiment_->document().value("ui", nlohmann::json::object())));
        projectPath_ = path;
        projectDirty_ = false;
        setWindowModified(false);
        statusBar()->showMessage("Experiment saved", 3000);
        return true;
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
        return false;
    }
}
void MainWindow::exportOutputStorage() {
    if (!outputDir_ || !outputStorageAction_->isEnabled())
        return;
    const auto path =
        QFileDialog::getSaveFileName(this, "Export Output Storage", "output.bin", "Binary data (*.bin)");
    if (path.isEmpty())
        return;
    try {
        QFile source(outputDir_->path() + "/result/output_storage.bin");
        if (!source.open(QIODevice::ReadOnly))
            throw std::runtime_error("Output storage is unavailable");
        writeFile(path, source.readAll());
        QFile metadata(outputDir_->path() + "/result/output_storage.json");
        if (!metadata.open(QIODevice::ReadOnly))
            throw std::runtime_error("Output metadata is unavailable");
        writeFile(path + ".json", metadata.readAll());
        statusBar()->showMessage("Output storage exported", 3000);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::selectOutputPixel(int x, int y, const QColor &color) {
    if (!frame_ || busy() || outputReport_.is_null())
        return;
    if (outputGeneration_ != displayedOutputGeneration_ || replayTimer_.isActive()) {
        statusBar()->showMessage("Replay output before selecting a pixel", 4000);
        return;
    }
    try {
        const auto display = outputReport_.at("output_display");
        const auto selection = outputReport_.at("output_selection");
        const auto resource = display.at("resource").get<Id>();
        const auto eventValue = selection.value("boundary", std::string{}) == "api_event"
                                    ? outputReport_.at("event")
                                    : selection.at("navigation_event");
        Id event = 0;
        if (eventValue.is_string())
            event = std::stoull(eventValue.get<std::string>());
        else if (eventValue.is_number_unsigned())
            event = eventValue.get<Id>();
        if (event && frame_->entries().contains(event) && frame_->entry(event).category == 7)
            locateEvent(event);
        const auto &entry = frame_->entry(resource);
        if (entry.type == 0x83) {
            const auto view = display.at("view_selection");
            if (y != 0 || x < 0 || uint64_t(x) >= view.at("element_count").get<uint64_t>())
                return;
            inspectResource(resource);
            bufferOffset_->setText(QString::number((view.at("first_element").get<uint64_t>() + uint64_t(x)) *
                                                   view.at("element_size").get<uint64_t>()));
            bufferLength_->setText(QString::number(view.at("element_size").get<uint64_t>()));
            bufferBoundary_->setCurrentIndex(event ? 2 : 0);
            replayTimer_.stop();
            bufferTimer_.start();
            return;
        }
        selectedResource_ = resource;
        updateExperimentActions();
        const auto coordinate = entry.type == 0x86 ? display.at("slice") : display.at("layer");
        properties("Output Pixel",
                   {{"Resource", QString::number(resource)},
                    {"Event", event ? QString::number(event) : "—"},
                    {"Pixel", QString("%1, %2").arg(x).arg(y)},
                    {"Mip", QString::fromStdString(display.at("mip").dump())},
                    {entry.type == 0x86 ? "Slice" : "Layer", QString::fromStdString(coordinate.dump())},
                    {"Sample", display.at("sample").is_null()
                                   ? (display.value("source_samples", 1u) > 1 ? "0 (resolved display)" : "0")
                                   : QString::fromStdString(display.at("sample").dump())},
                    {"Display RGBA", QString("%1, %2, %3, %4")
                                         .arg(color.red())
                                         .arg(color.green())
                                         .arg(color.blue())
                                         .arg(color.alpha())}});
        auto root = properties_->topLevelItem(0);
        if (root && root->childCount())
            root->child(0)->setData(
                0, Qt::UserRole,
                QString::fromStdString(
                    nlohmann::json({{"id", resource}, {"exists", true}, {"category", 5}}).dump()));
        findChild<QTabWidget *>("inspectorTabs")->setCurrentIndex(0);
        statusBar()->showMessage(QString("Pixel %1, %2 · T:%3").arg(x).arg(y).arg(resource), 4000);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
void MainWindow::exportImage() {
    auto viewer = centerTabs_->currentWidget() == texturePane_ ? textureImage_ : image_;
    if (viewer->image().isNull())
        return;
    auto path = QFileDialog::getSaveFileName(this, "Export output", {}, "PNG image (*.png)");
    if (path.isEmpty())
        return;
    if (!viewer->displayImage().save(path))
        showError("Cannot save image.");
    else
        statusBar()->showMessage("Image exported", 3000);
}
void MainWindow::exportBytes() {
    if (!frame_ || !selectedResource_)
        return;
    try {
        auto r = frame_->resource(selectedResource_);
        Bytes bytes;
        std::vector<uint8_t> effective;
        if (r.type >= 0x90 && r.type <= 0x95) {
            effective = experiment_->shaderBytes(*frame_, selectedResource_);
            bytes = effective;
        } else if (r.data)
            bytes = frame_->data(r.data);
        else
            bytes = frame_->payload(selectedResource_);
        auto path = QFileDialog::getSaveFileName(this, "Export resource",
                                                 QString("resource-%1.bin").arg(selectedResource_),
                                                 "Binary data (*.bin *.dxbc)");
        if (path.isEmpty())
            return;
        writeFile(path, QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
        statusBar()->showMessage("Resource exported", 3000);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
} // namespace flora
