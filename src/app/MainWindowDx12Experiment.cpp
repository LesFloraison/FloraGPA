#include "MainWindow.h"
#include <QAction>
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
void MainWindow::prepareDx12Experiment(const QString &path, std::optional<std::pair<Id, bool>> edit) {
    if (busy() || !session_ || session_->backend() != ReplayBackend::Dx12) return;
    const auto session = session_;
    const auto revision = revision_;
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
        emit experimentFinished(true);
    });
    watcher->setFuture(QtConcurrent::run([session, token, path, edit] {
        ExperimentResult result;
        try {
            const CancelCheck cancelled = [token] { return token->load(); };
            result.experiment = std::make_shared<Experiment>(*session, cancelled);
            if (!path.isEmpty()) result.experiment->load(path, *session);
            checkCancellation(cancelled);
            if (edit) result.experiment->setEnabled(*session, edit->first, edit->second);
            const auto &document = result.experiment->document();
            if (document.contains("ui") && !document.at("ui").is_object())
                throw std::runtime_error("Invalid DX12 project UI state");
            if (document.contains("ui") && document.at("ui").contains("dx12"))
                result.ui = readUi(document.at("ui").at("dx12"), *session);
            checkCancellation(cancelled);
        } catch (const OperationCancelled &) { result.cancelled = true; }
        catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
        return result;
    }));
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
                      {"allocation_mib", std::to_string(dx12Budget_->value())}};
        (void)readUi(ui.at("dx12"), *session_);
        experiment_->save(path, ui);
        projectPath_ = path; projectDirty_ = false; setWindowModified(false);
        statusBar()->showMessage("Experiment saved", 3000);
        return true;
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); return false; }
}
}
