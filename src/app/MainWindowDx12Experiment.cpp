#include "MainWindow.h"
#include <QAction>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>
#include <charconv>
#include <climits>

namespace flora {
namespace {
struct ExperimentUi {
    Id event{}, resource{};
    bool execution{};
    int boundary{}, subresource{}, budget{};
    bool isolateMissingQueries{};
};
ExperimentUi readUi(const nlohmann::json &ui, const IReplaySession &session) {
    const auto number = [&](const char *key, uint64_t limit) {
        const auto text = ui.at(key).get<std::string>(); uint64_t value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            std::to_string(value) != text || value > limit)
            throw std::runtime_error("Invalid DX12 project UI value");
        return value;
    };
    if (ui.at("schema") != 1) throw std::runtime_error("Unsupported DX12 project UI schema");
    const auto domain = ui.at("domain").get<std::string>();
    if (domain != "execution_instance" && domain != "cpu_record") throw std::runtime_error("Invalid DX12 project event domain");
    ExperimentUi result{number("event", UINT64_MAX), number("resource_creation", UINT64_MAX),
                        domain == "execution_instance", int(number("boundary", 2)),
                        int(number("subresource", INT_MAX)), int(number("allocation_mib", 131072))};
    const auto policy = ui.value("missing_query_policy", "fail");
    if (policy != "fail" && policy != "isolate_readback_outputs")
        throw std::runtime_error("Invalid DX12 project query replay policy");
    result.isolateMissingQueries = policy == "isolate_readback_outputs";
    if (result.budget < 64) throw std::runtime_error("DX12 project allocation budget is too small");
    if (result.event) {
        if (result.execution) (void)session.executionEvent(result.event);
        else (void)session.event(result.event);
    }
    if (result.resource) (void)session.resource(result.resource);
    return result;
}
struct ExperimentResult {
    std::shared_ptr<Experiment> experiment;
    std::optional<ExperimentUi> ui;
    QString error;
    bool cancelled{};
};
}
void MainWindow::prepareDx12Experiment(const QString &path, std::optional<std::pair<Id, bool>> edit,
                                      std::optional<Dx12ShaderImport> shader, std::optional<Dx12ResourceImport> resource,
                                      std::optional<std::pair<Id, bool>> barrier) {
    if (busy() || !session_ || session_->backend() != ReplayBackend::Dx12) return;
    const auto session = session_;
    const auto revision = revision_;
    const auto current = (shader || resource || barrier) && experiment_ ? std::make_shared<Experiment>(*experiment_) : nullptr;
    const auto token = std::make_shared<std::atomic_bool>(false);
    experimentCancel_ = token;
    setBusy(true);
    statusBar()->showMessage("Verifying capture contents for experiment…");
    auto watcher = new QFutureWatcher<ExperimentResult>(this);
    watcher->setObjectName("dx12ExperimentLoader");
    connect(watcher, &QFutureWatcher<ExperimentResult>::finished, this,
            [this, watcher, token, session, revision, path] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (experimentCancel_ != token) return;
        experimentCancel_.reset();
        setBusy(busy());
        if (token->load() || result.cancelled || session_ != session || revision_ != revision) {
            statusBar()->showMessage("Experiment preparation cancelled", 3000);
            emit experimentFinished(false); return;
        }
        if (!result.error.isEmpty()) {
            showError(result.error); emit experimentFinished(false); return;
        }
        experiment_ = std::make_unique<Experiment>(*result.experiment);
        if (result.ui) {
            const auto &ui = *result.ui;
            selection_.isolateMissingQueries = ui.isolateMissingQueries;
            const QSignalBlocker block(dx12QueryIsolation_);
            dx12QueryIsolation_->setChecked(ui.isolateMissingQueries);
            findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(ui.execution ? 1 : 0);
            selectedEvent_ = 0; selection_.event = 0; selection_.execution.reset();
            apiView_->setCurrentIndex({}); properties_->clear();
            selectionLabel_->setText(ui.execution ? "Submitted instances — not replayed" : "Recorded API calls");
            if (ui.event) locateEvent(ui.event);
            selection_.resource = selectedResource_ = 0;
            resourceView_->setCurrentIndex({});
            if (ui.resource) {
                if (auto search = resourceView_->parentWidget()->findChild<QLineEdit *>()) search->clear();
                resourceFilter_->setFilterRegularExpression(QString{});
                const auto index = resourceFilter_->mapFromSource(resources_->index(resources_->rowOf(ui.resource), 0));
                resourceView_->setCurrentIndex(index);
                resourceView_->scrollTo(index);
                inspectResource(ui.resource);
            }
            boundary_->setCurrentIndex(ui.boundary);
            dx12Subresource_->setValue(ui.subresource);
            dx12Budget_->setValue(ui.budget);
        }
        if (!path.isEmpty()) projectPath_ = path;
        experimentChanged();
        if (!path.isEmpty()) { projectDirty_ = false; setWindowModified(false); }
        statusBar()->showMessage(path.isEmpty() ? "Experiment updated" : "Experiment loaded", 3000);
        emit experimentFinished(true);
    });
    watcher->setFuture(QtConcurrent::run([session, token, path, edit, shader, resource, barrier, current] {
        ExperimentResult result;
        try {
            const CancelCheck cancelled = [token] { return token->load(); };
            result.experiment = current ? current : std::make_shared<Experiment>(*session, cancelled);
            if (!path.isEmpty()) result.experiment->load(path, *session);
            checkCancellation(cancelled);
            if (edit) result.experiment->setEnabled(*session, edit->first, edit->second);
            if (barrier) result.experiment->setUavBarrierBefore(*session, barrier->first, barrier->second);
            if (shader) {
                const auto &[pipeline, stage, source] = *shader;
                QFile file(source);
                if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 64 * 1024 * 1024)
                    throw std::runtime_error("Cannot read shader bytecode, or file exceeds 64 MiB");
                const auto bytes = file.readAll();
                if (bytes.size() != file.size()) throw std::runtime_error("Incomplete shader bytecode read");
                result.experiment->setShader(*session, pipeline, stage,
                    {reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())});
            }
            if (resource) {
                QFile file(resource->path);
                if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 256 * 1024 * 1024)
                    throw std::runtime_error("Cannot read resource data, or file exceeds 256 MiB");
                const auto bytes = file.readAll();
                if (bytes.size() != file.size()) throw std::runtime_error("Incomplete resource data read");
                result.experiment->setResourcePatch(*session, resource->submission, resource->resource, resource->offset,
                    resource->subresource, resource->texture, {reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())},
                    resource->afterExecution);
            }
            const auto &document = result.experiment->document();
            if (document.contains("ui") && !document.at("ui").is_object())
                throw std::runtime_error("Invalid DX12 project UI state");
            if (!path.isEmpty() && document.contains("ui") && document.at("ui").contains("dx12"))
                result.ui = readUi(document.at("ui").at("dx12"), *session);
            checkCancellation(cancelled);
        } catch (const OperationCancelled &) { result.cancelled = true; }
        catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
        return result;
    }));
}
void MainWindow::replaceDx12Shader(Id pipeline, const std::string &stage, const QString &bytecodePath) {
    if (!bytecodePath.isEmpty()) prepareDx12Experiment({}, {}, Dx12ShaderImport{pipeline, stage, bytecodePath});
}
void MainWindow::patchDx12Resource(Id submission, Id resource, uint64_t offset, uint32_t subresource,
                                  bool texture, const QString &dataPath, Id afterExecution) {
    if (!dataPath.isEmpty()) prepareDx12Experiment({}, {}, {}, Dx12ResourceImport{submission, resource, offset, subresource, texture, dataPath, afterExecution});
}
void MainWindow::importDx12Resource() {
    if (busy() || !session_ || session_->backend() != ReplayBackend::Dx12 || !selection_.execution || !selection_.resource) return;
    try {
        const auto event = session_->executionEvent(*selection_.execution);
        if (!event.submission) throw std::runtime_error("Select a submitted execution instance");
        const auto desc = nlohmann::json::parse(session_->resource(selection_.resource).summary);
        const bool texture = desc.at("Dimension") != 1;
        bool accepted{};
        Id afterExecution{};
        if (!texture) {
            const auto boundary = QInputDialog::getItem(this, "Buffer Patch Boundary", "Apply the imported bytes:",
                {"Before the selected submission", "After the selected execution"}, 0, false, &accepted);
            if (!accepted) return;
            if (boundary == "After the selected execution") afterExecution = *selection_.execution;
        }
        const auto title = afterExecution
            ? QString("Patch Resource %1 after Execution %2").arg(selection_.resource).arg(afterExecution)
            : QString("Patch Resource %1 before Submission %2").arg(selection_.resource).arg(*event.submission);
        const auto text = QInputDialog::getText(this, title,
            (texture ? "Subresource index (import complete, tightly packed rows):" : "Destination byte offset:") +
            (afterExecution
                ? QString("\nApplies after this call, before later calls in the same submission. Requires a default-heap buffer with resource state established by a transition in this command list.")
                : QString("\nApplies before all commands in this submission. Later copies, clears and draws may overwrite the data.")),
            QLineEdit::Normal, texture ? QString::number(dx12Subresource_->value()) : "0", &accepted);
        if (!accepted) return;
        bool valid{}; const auto value = text.toULongLong(&valid);
        if (!valid || text != QString::number(value) || (texture && value > UINT32_MAX))
            throw std::runtime_error("Enter a valid decimal offset or subresource index");
        const auto path = QFileDialog::getOpenFileName(this, title, {}, "Raw resource data (*.bin);;All files (*)");
        if (!path.isEmpty()) patchDx12Resource(*event.submission, selection_.resource, texture ? 0 : value,
                                              texture ? uint32_t(value) : 0, texture, path, afterExecution);
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::importDx12Shader() {
    if (busy() || !session_ || session_->backend() != ReplayBackend::Dx12) return;
    bool accepted{};
    const auto text = QInputDialog::getText(this, "Replace Pipeline Shader",
        "PSO creation record ID (applies to every use of this pipeline):", QLineEdit::Normal,
        commands_->executionView() ? QString{} : QString::number(selectedEvent_), &accepted);
    if (!accepted) return;
    try {
        bool valid{}; const auto pipeline = text.toULongLong(&valid);
        if (!valid || !pipeline || text != QString::number(pipeline)) throw std::runtime_error("Enter a decimal PSO creation record ID");
        const auto info = session_->eventDetails(pipeline);
        if (!info.contains("pipeline")) throw std::runtime_error("Selected record is not a supported pipeline creation");
        QStringList stages;
        for (const auto &shader : info.at("pipeline").at("shaders")) stages << QString::fromStdString(shader.at("stage").get<std::string>());
        if (stages.empty()) throw std::runtime_error("Pipeline has no captured shader stages");
        const auto stage = QInputDialog::getItem(this, "Replace Pipeline Shader", "Captured stage:", stages, 0, false, &accepted);
        if (!accepted) return;
        const auto file = QFileDialog::getOpenFileName(this, "Import compiled DXBC/DXIL shader", {}, "Shader bytecode (*.cso *.dxbc *.dxil *.bin);;All files (*)");
        if (!file.isEmpty()) replaceDx12Shader(pipeline, stage.toStdString(), file);
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::openDx12Experiment(const QString &path) {
    if (!path.isEmpty()) prepareDx12Experiment(path);
}
bool MainWindow::saveDx12Experiment(const QString &path) {
    if (path.isEmpty() || busy() || !session_ || session_->backend() != ReplayBackend::Dx12 || !experiment_) return false;
    try {
        auto ui = experiment_->document().value("ui", nlohmann::json::object());
        if (!ui.is_object()) throw std::runtime_error("Invalid DX12 project UI state");
        ui["dx12"] = {{"schema", 1}, {"domain", commands_->executionView() ? "execution_instance" : "cpu_record"},
                      {"event", std::to_string(selectedEvent_)}, {"resource_creation", std::to_string(selection_.resource)},
                      {"boundary", std::to_string(boundary_->currentIndex())},
                      {"subresource", std::to_string(dx12Subresource_->value())},
                      {"allocation_mib", std::to_string(dx12Budget_->value())},
                      {"missing_query_policy", selection_.isolateMissingQueries ? "isolate_readback_outputs" : "fail"}};
        (void)readUi(ui.at("dx12"), *session_);
        experiment_->save(path, ui);
        projectPath_ = path; projectDirty_ = false; setWindowModified(false);
        statusBar()->showMessage("Experiment saved", 3000);
        return true;
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); return false; }
}
}
