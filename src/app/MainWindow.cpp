#include "MainWindow.h"
#include "replay/Replay.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
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
} // namespace
MainWindow::MainWindow() {
    buildUi();
    loadSettings();
    replayTimer_.setSingleShot(true);
    replayTimer_.setInterval(180);
    connect(&replayTimer_, &QTimer::timeout, this, [this] { replay(); });
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
            commands_->setFrame(frame_);
            resources_->setFrame(frame_);
            chart_->clear();
            pipeline_->clear();
            metrics_->clear();
            properties_->clear();
            shader_->clear();
            bufferModel_->setBytes({});
            report_ = {};
            image_->setImage({});
            setWindowTitle(QFileInfo(capturePath_).completeBaseName() + " — FloraGPA");
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
    openAction_ = file->addAction(
        "&Open Capture…", this,
        [this] {
            auto path = QFileDialog::getOpenFileName(this, "Open DX11 capture", {},
                                                     "GPA frames (*.gpa_frame *.gpaframe);;All files (*)");
            if (!path.isEmpty())
                openCapture(path);
        },
        QKeySequence::Open);
    openAction_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    auto saveProject = file->addAction("Save Experiment…");
    saveProject->setEnabled(false);
    saveProject->setToolTip("Experiment migration pending");
    file->addSeparator();
    exportAction_ =
        file->addAction("Export Image…", this, &MainWindow::exportImage, QKeySequence("Ctrl+Shift+S"));
    file->addAction("Export Resource…", this, &MainWindow::exportBytes);
    file->addSeparator();
    file->addAction("Exit", this, &QWidget::close, QKeySequence::Quit);
    auto analyze = menuBar()->addMenu("&Analysis");
    replayAction_ = analyze->addAction("Replay", this, [this] { replay(); }, QKeySequence("F5"));
    replayAction_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    collectAction_ =
        analyze->addAction("Collect GPU Metrics", this, [this] { replay(true); }, QKeySequence("F6"));
    cancelAction_ = analyze->addAction("Cancel", this, &MainWindow::cancel, QKeySequence("Escape"));
    cancelAction_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    cancelAction_->setEnabled(false);
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
    apiView_->setObjectName("apiLog");
    apiView_->setColumnWidth(1, 185);
    resources_ = new CaptureModel(CaptureModel::Kind::Resources, this);
    resourceFilter_ = new CaptureFilter(this);
    resourceFilter_->setSourceModel(resources_);
    resourceFilter_->setFilterKeyColumn(-1);
    resourceFilter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    resourceView_ = table(resourceFilter_);
    resourceView_->setObjectName("resources");
    auto apiKinds = new QComboBox;
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
    leftTabs_->addTab(filtered(apiView_, commandFilter_, apiKinds), "API Log");
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
    boundary_ = new QComboBox;
    boundary_->addItems({"Final", "Before event", "After event"});
    imageBar->addWidget(boundary_);
    imageBar->addSeparator();
    channels_ = new QComboBox;
    channels_->addItems({"RGB", "RGBA", "R", "G", "B", "A"});
    imageBar->addWidget(channels_);
    imageBar->addSeparator();
    imageBar->addAction("Fit", this, [this] { image_->fit(); });
    imageBar->addAction("100%", this, [this] { image_->actualSize(); });
    imageBar->addAction("Export", this, &MainWindow::exportImage);
    auto imageSpacer = new QWidget;
    imageSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    imageBar->addWidget(imageSpacer);
    imageLabel_ = new QLabel("—");
    imageBar->addWidget(imageLabel_);
    outputLayout->addWidget(imageBar);
    image_ = new ImageView;
    image_->setObjectName("frameOutput");
    outputLayout->addWidget(image_);
    centerTabs_->addTab(output, "Output");
    pipeline_ = tree({"Stage / Binding", "Resource", "Details"});
    pipeline_->setObjectName("pipeline");
    pipeline_->setColumnWidth(0, 225);
    pipeline_->setColumnWidth(1, 110);
    centerTabs_->addTab(pipeline_, "Pipeline");
    shader_ = new QPlainTextEdit;
    shader_->setReadOnly(true);
    shader_->setLineWrapMode(QPlainTextEdit::NoWrap);
    shader_->setFont(QFont("Cascadia Mono", 10));
    shader_->setObjectName("shader");
    centerTabs_->addTab(shader_, "Shader");
    bufferModel_ = new BufferModel(this);
    bufferView_ = table(bufferModel_);
    bufferView_->setFont(QFont("Cascadia Mono", 10));
    bufferView_->setColumnWidth(0, 105);
    bufferView_->setColumnWidth(1, 420);
    centerTabs_->addTab(bufferView_, "Buffer");
    for (const auto &name : {"Geometry", "Pixel History", "Shader Debug"}) {
        int tab = centerTabs_->addTab(new QWidget, name);
        centerTabs_->setTabEnabled(tab, false);
        centerTabs_->setTabToolTip(tab, "Migration pending");
    }
    auto rightTabs = new QTabWidget;
    rightTabs->setObjectName("inspectorTabs");
    rightTabs->setDocumentMode(true);
    properties_ = tree({"Property", "Value"});
    properties_->setObjectName("properties");
    metrics_ = tree({"Metric", "Value"});
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
    connect(image_, &ImageView::zoomChanged, this,
            [this](int percent) { zoomLabel_->setText(QString("%1%").arg(percent)); });
    connect(channels_, &QComboBox::currentTextChanged, image_, &ImageView::channel);
    connect(boundary_, &QComboBox::currentIndexChanged, this, [this] {
        if (frame_) {
            ++revision_;
            if (process_.state() != QProcess::NotRunning)
                cancel();
            replayTimer_.start();
        }
    });
    connect(adapter_, &QComboBox::currentIndexChanged, this, [this] {
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
        if (id)
            inspectResource(id);
    });
    setBusy(false);
}
void MainWindow::loadSettings() {
    QSettings s;
    restoreGeometry(s.value("window/geometry").toByteArray());
    workspace_->restoreState(s.value("window/docks").toByteArray(), 1);
}
void MainWindow::closeEvent(QCloseEvent *e) {
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/docks", workspace_->saveState(1));
    cancel();
    e->accept();
}
void MainWindow::setBusy(bool busy) {
    openAction_->setEnabled(!busy);
    replayAction_->setEnabled(!busy && bool(frame_));
    collectAction_->setEnabled(!busy && bool(frame_));
    cancelAction_->setEnabled(busy && !loader_.isRunning());
    progress_->setVisible(busy);
    if (busy) {
        progress_->setRange(0, 0);
    } else
        progress_->setValue(0);
    exportAction_->setEnabled(!image_->image().isNull());
}
void MainWindow::openCapture(const QString &path) {
    if (loader_.isRunning())
        return;
    cancel();
    replayTimer_.stop();
    pendingPath_ = path;
    setBusy(true);
    statusBar()->showMessage("Opening capture…");
    loader_.setFuture(QtConcurrent::run([path] {
        return std::shared_ptr<const Frame>(
            std::make_shared<Frame>(std::filesystem::path(path.toStdWString())));
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
    QStringList args{"replay", capturePath_};
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
    jobDir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/FloraGPA-XXXXXX");
    if (!jobDir_->isValid()) {
        showError("Cannot create worker directory.");
        return;
    }
    args << "--out" << jobDir_->path() + "/result";
    if (adapter_->currentIndex() == 1)
        args << "--warp";
    runningRevision_ = revision_;
    runningTimings_ = timings;
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
        report_ = QJsonDocument::fromJson(file.readAll(), &error).object();
        if (error.error != QJsonParseError::NoError || !report_["completed"].toBool())
            throw std::runtime_error("Worker did not complete");
        QImage result(jobDir_->path() + "/result/frame.png");
        if (result.isNull())
            throw std::runtime_error("Worker output image is missing");
        image_->setImage(std::move(result));
        image_->channel(channels_->currentText());
        imageLabel_->setText(QString("T:%1  ·  %2 × %3")
                                 .arg(report_["resource"].toString())
                                 .arg(report_["width"].toInt())
                                 .arg(report_["height"].toInt()));
        if (runningTimings_) {
            findChild<QTabWidget*>("inspectorTabs")->setCurrentWidget(metrics_);
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
        exportAction_->setEnabled(true);
        statusBar()->showMessage(report_["adapter"].toString() + "  ·  Replay complete", 7000);
        log_->appendPlainText(QString("Replay complete · %1 × %2 · %3")
                                  .arg(report_["width"].toInt())
                                  .arg(report_["height"].toInt())
                                  .arg(report_["rgba_sha256"].toString()));
        emit taskFinished(true);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
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
void MainWindow::selectEvent(Id id) {
    if (!frame_ || selectedEvent_ == id)
        return;
    selectedEvent_ = id;
    selectedResource_ = 0;
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
        if (isDraw(frame_->entry(id).type))
            showPipeline(frame_->state(frame_->event(id).state));
        else
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
    auto &e = frame_->entry(id);
    QList<QPair<QString, QString>> values{{"ID", QString::number(id)},
                                          {"Type", QString("0x%1").arg(e.type, 4, 16, QChar('0'))},
                                          {"Wire bytes", QString::number(e.size)}};
    if (isDraw(e.type)) {
        auto event = frame_->event(id);
        values.append(QPair<QString, QString>{"State", QString::number(event.state)});
        values.append(QPair<QString, QString>{"Context", QString::number(event.context)});
        QStringList names;
        switch (e.type) {
        case 0x35:
            names = {"X", "Y", "Z"};
            break;
        case 0x37:
            names = {"Vertex count", "Start vertex"};
            break;
        case 0x39:
            names = {"Index count", "Start index", "Base vertex"};
            break;
        case 0x3a:
            names = {"Index count", "Instance count", "Start index", "Base vertex", "Start instance"};
            break;
        case 0x3c:
            names = {"Vertex count", "Instance count", "Start vertex", "Start instance"};
            break;
        default:
            names = {"Offset"};
        }
        for (size_t i = 0; i < event.args.size(); ++i)
            values.append(
                QPair<QString, QString>{names.value(int(i), "Argument"), QString::number(event.args[i])});
    } else
        values.append(QPair<QString, QString>{"Wire", hex(frame_->payload(id), 96)});
    properties(QString::fromStdString(commandName(e.type)), values);
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
        for (int i = 0; i < 14; ++i)
            if (stage.cb[i])
                link(group, QString("CB %1").arg(i), stage.cb[i]);
        for (int i = 0; i < 128; ++i)
            if (stage.srv[i])
                link(group, QString("SRV %1").arg(i), stage.srv[i]);
        for (int i = 0; i < 16; ++i)
            if (stage.samplers[i])
                link(group, QString("Sampler %1").arg(i), stage.samplers[i]);
    }
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
}
void MainWindow::inspectResource(Id id) {
    if (!frame_)
        return;
    try {
        auto &e = frame_->entry(id);
        if (e.category != 5)
            return;
        selectedResource_ = id;
        auto resource = frame_->resource(id);
        QList<QPair<QString, QString>> values{{"ID", QString::number(id)},
                                              {"Type", QString("0x%1").arg(e.type, 4, 16, QChar('0'))},
                                              {"Device", QString::number(resource.device)}};
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
            auto bytes = frame_->shader(resource.data);
            shader_->setPlainText(QString::fromStdString(disassemble(bytes)));
            centerTabs_->setCurrentWidget(shader_);
            values.append(QPair<QString, QString>{"DXBC bytes", QString::number(bytes.size())});
        }
        if (e.type == 0x83) {
            const QStringList names{"Byte width", "Usage",      "Bind flags",
                                    "CPU access", "Misc flags", "Structure stride"};
            for (size_t i = 0; i < resource.desc.size(); ++i)
                values.append(QPair<QString, QString>{names[int(i)], QString::number(resource.desc[i])});
            auto bytes = resource.data ? frame_->data(resource.data) : Bytes{};
            bufferModel_->setBytes(
                QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
            centerTabs_->setCurrentWidget(bufferView_);
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
void MainWindow::exportImage() {
    if (image_->image().isNull())
        return;
    auto path = QFileDialog::getSaveFileName(this, "Export output", {}, "PNG image (*.png)");
    if (path.isEmpty())
        return;
    if (!image_->image().save(path))
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
        if (r.type >= 0x90 && r.type <= 0x95)
            bytes = frame_->shader(r.data);
        else if (r.data)
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
