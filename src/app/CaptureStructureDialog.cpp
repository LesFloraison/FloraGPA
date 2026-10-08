#include "CaptureStructureDialog.h"
#include "StructureModel.h"
#include "StructureExport.h"
#include "application/ContextInspector.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace flora {
namespace {
struct Result {
    std::shared_ptr<const nlohmann::json> documents[2];
    QString error;
    bool cancelled = false;
};
}
CaptureStructureDialog::CaptureStructureDialog(std::shared_ptr<const Frame> frame, QWidget *parent)
    : QDialog(parent), frame_(std::move(frame)) {
    setObjectName("captureStructureDialog"); setWindowTitle("Contexts and Command Lists"); resize(840, 600);
    auto layout = new QVBoxLayout(this); tabs_ = new QTabWidget; layout->addWidget(tabs_);
    for (int i = 0; i < 2; ++i) {
        auto view = views_[i] = new QTreeView;
        view->setObjectName(i ? "commandListInventory" : "contextInventory");
        view->setUniformRowHeights(true); view->setAlternatingRowColors(true);
        tabs_->addTab(view, i ? "Command Lists" : "Contexts");
        connect(view, &QTreeView::doubleClicked, this, [this, view](const QModelIndex &index) {
            if (busy()) return;
            const auto id = index.siblingAtColumn(1).data(Qt::UserRole).toULongLong();
            if (id) { event_ = id; accept(); }
        });
    }
    status_ = new QLabel("Reading capture structure…"); status_->setObjectName("structureStatus"); layout->addWidget(status_);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    export_ = buttons->addButton("Export JSON…", QDialogButtonBox::ActionRole); export_->setObjectName("exportCaptureStructure");
    cancel_ = buttons->addButton("Cancel", QDialogButtonBox::ActionRole); cancel_->setObjectName("cancelCaptureStructure");
    retry_ = buttons->addButton("Retry", QDialogButtonBox::ActionRole); retry_->setObjectName("retryCaptureStructure"); retry_->hide();
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &CaptureStructureDialog::reject);
    connect(cancel_, &QPushButton::clicked, this, &CaptureStructureDialog::cancel);
    connect(retry_, &QPushButton::clicked, this, &CaptureStructureDialog::load);
    connect(export_, &QPushButton::clicked, this, &CaptureStructureDialog::exportDocument);
    export_->setEnabled(false); cancel_->setEnabled(false);
    QTimer::singleShot(0, this, &CaptureStructureDialog::load);
}
CaptureStructureDialog::~CaptureStructureDialog() { cancel(); }
void CaptureStructureDialog::cancel() {
    if (loadCancel_) loadCancel_->store(true);
    if (exportCancel_) exportCancel_->store(true);
}
void CaptureStructureDialog::reject() { closed_ = true; cancel(); QDialog::reject(); }
void CaptureStructureDialog::controls() {
    cancel_->setEnabled(busy()); export_->setEnabled(!busy() && bool(documents_[0]));
    retry_->setVisible(!busy() && !documents_[0]);
}
void CaptureStructureDialog::load() {
    if (busy() || closed_) return;
    const auto token = loadCancel_ = std::make_shared<std::atomic_bool>(false);
    controls(); status_->setText("Reading capture structure…"); status_->setToolTip({});
    auto watcher = new QFutureWatcher<Result>(this); watcher->setObjectName("structureLoader");
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, token] {
        auto result = watcher->future().takeResult(); watcher->deleteLater();
        if (loadCancel_ != token) return;
        loadCancel_.reset();
        if (!token->load() && result.error.isEmpty() && !result.cancelled) {
            for (int i = 0; i < 2; ++i) {
                documents_[i] = std::move(result.documents[i]);
                auto model = new StructureModel(documents_[i], frame_, views_[i]);
                auto previous = views_[i]->model(); views_[i]->setModel(model); delete previous;
                views_[i]->setToolTip(model->rootToolTip()); views_[i]->setColumnWidth(0, 290);
                views_[i]->expandToDepth(0);
            }
            status_->setText("Ready");
        } else {
            status_->setText(result.error.isEmpty() ? "Inspection cancelled" : "Inspection failed");
            status_->setToolTip(result.error);
        }
        controls(); emit inspectionFinished(bool(documents_[0]));
    });
    watcher->setFuture(QtConcurrent::run([frame = frame_, token] {
        Result result; const CancelCheck cancelled = [token] { return token->load(); };
        try {
            result.documents[0] = std::make_shared<const nlohmann::json>(inspectContexts(*frame, cancelled));
            try {
                result.documents[1] = std::make_shared<const nlohmann::json>(inspectCommandLists(*frame, cancelled));
            } catch (const OperationCancelled &) { throw; }
            catch (const std::exception &error) {
                result.documents[1] = std::make_shared<const nlohmann::json>(nlohmann::json{{"error", error.what()}});
            }
            checkCancellation(cancelled);
        } catch (const OperationCancelled &) { result.cancelled = true; }
        catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
        return result;
    }));
}
void CaptureStructureDialog::exportDocument() {
    if (busy() || !documents_[0]) return;
    const auto index = tabs_->currentIndex(); const auto document = documents_[index];
    const QPointer<CaptureStructureDialog> alive(this);
    const auto path = QFileDialog::getSaveFileName(this, "Export Capture Structure",
        index ? "command-lists.json" : "contexts.json", "JSON (*.json)");
    if (!alive || path.isEmpty() || !isVisible() || busy()) return;
    const auto token = exportCancel_ = std::make_shared<std::atomic_bool>(false);
    controls(); status_->setText("Exporting capture structure…"); status_->setToolTip({});
    auto watcher = new QFutureWatcher<Result>(this); watcher->setObjectName("structureExporter");
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, token] {
        auto result = watcher->future().takeResult(); watcher->deleteLater();
        if (exportCancel_ != token) return;
        exportCancel_.reset(); controls();
        status_->setText(!result.error.isEmpty() ? "Export failed" : result.cancelled ? "Export cancelled" : "Structure exported");
        status_->setToolTip(result.error); emit exportFinished(result.error.isEmpty() && !result.cancelled);
    });
    watcher->setFuture(QtConcurrent::run([document, path, token] {
        Result result;
        try { exportStructureFile(path, *document, [token] { return token->load(); }); }
        catch (const OperationCancelled &) { result.cancelled = true; }
        catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
        return result;
    }));
}
}
