#include "PredicateView.h"
#include "application/PredicateInspector.h"
#include "core/NormalizedPredication.h"
#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace flora {
PredicateView::PredicateView(QWidget *parent) : QWidget(parent) {
    setObjectName("predicatePane");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    resources_ = new QComboBox;
    resources_->setObjectName("predicateResource");
    resources_->setMinimumContentsLength(18);
    bar->addWidget(resources_);
    boundary_ = new QComboBox;
    boundary_->setObjectName("predicateBoundary");
    boundary_->addItems({"Before event", "After event"});
    bar->addWidget(boundary_);
    read_ = bar->addAction("Read");
    read_->setObjectName("readPredicate");
    export_ = bar->addAction("Export");
    eventLabel_ = new QLabel;
    bar->addWidget(eventLabel_);
    layout->addWidget(bar);
    fields_ = new QTreeWidget;
    fields_->setObjectName("predicateFields");
    fields_->setHeaderLabels({"Property", "Value"});
    fields_->setRootIsDecorated(false);
    fields_->setAlternatingRowColors(true);
    fields_->setColumnWidth(0, 240);
    fields_->header()->setStretchLastSection(true);
    layout->addWidget(fields_);
    connect(resources_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(boundary_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(read_, &QAction::triggered, this, [this] {
        if (!read_->isEnabled())
            return;
        invalidate();
        emit readRequested(resources_->currentData().toULongLong(), event_, boundary_->currentIndex() == 1,
                           revision_);
    });
    connect(export_, &QAction::triggered, this, [this] {
        if (result_.is_null())
            return;
        auto path = QFileDialog::getSaveFileName(this, "Export Predicate", "predicate.json", "JSON (*.json)");
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        auto bytes = QByteArray::fromStdString(result_.dump(2) + "\n");
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            eventLabel_->setText("  Export failed");
    });
    updateActions();
}
void PredicateView::setSelection(std::shared_ptr<const Frame> frame, Id event) {
    if (frame_ != frame) {
        frame_ = std::move(frame);
        QSignalBlocker block(resources_);
        resources_->clear();
        if (frame_)
            for (const auto &[id, e] : frame_->entries())
                if (e.category == 5 && e.type == 0x96)
                    resources_->addItem(QString("Predicate %1").arg(id), QVariant::fromValue(qulonglong(id)));
        if (frame_)
            for (const auto &[bindingEvent, proof] : auditNormalizedPredication(*frame_))
                if (resources_->findData(QVariant::fromValue(qulonglong(proof.resource))) < 0)
                    resources_->addItem(QString("Condition %1").arg(proof.resource),
                                        QVariant::fromValue(qulonglong(proof.resource)));
    }
    event_ = event;
    invalidate();
}
void PredicateView::selectResource(Id id) {
    resources_->setCurrentIndex(resources_->findData(QVariant::fromValue(qulonglong(id))));
}
void PredicateView::invalidate() {
    ++revision_;
    result_ = nullptr;
    fields_->clear();
    eventLabel_->setText(event_ ? QString("  Event %1").arg(event_) : QString{});
    updateActions();
}
void PredicateView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void PredicateView::updateActions() {
    read_->setEnabled(!busy_ && frame_ && event_ && resources_->currentIndex() >= 0);
    export_->setEnabled(!result_.is_null());
}
bool PredicateView::finish(uint64_t request, const nlohmann::json &result) {
    if (request != revision_)
        return false;
    fields_->clear();
    result_ = result;
    auto add = [&](const QString &name, const nlohmann::json &value) {
        auto text = value.is_null()
                        ? QString("—")
                        : QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
        return new QTreeWidgetItem(fields_, {name, text});
    };
    if (result.contains("error")) {
        add("Error", result["error"]);
        updateActions();
        emit inspectionFinished(false);
        return true;
    }
    add("Status", result["status"] == "captured_condition" ? nlohmann::json("Captured condition")
                  : result["status"] == "replay_baseline" ? nlohmann::json("Replay baseline") : result["status"]);
    add("Result", result["value"]);
    add("Bound", result["bound"]);
    add("Predicate value", result["predicate_value"]);
    add("Query", result["resource"]["query_name"]);
    add("Flags", result["resource"]["query_flags"]);
    add("Source", result["source"])
        ->setToolTip(1,
                     result["status"] == "captured_condition"
                         ? "Execution follows a proven saved condition. The captured Predicate descriptor and query result are unavailable."
                     : result["status"] == "replay_baseline"
                         ? "Empty query initialized for replay. The application's earlier query result is unknown."
                         : "Actual GPU query at the selected boundary; captured return values are not restored.");
    if (result.contains("condition_allows_execution"))
        add("Allows execution", result["condition_allows_execution"]);
    updateActions();
    emit inspectionFinished(true);
    return true;
}
} // namespace flora
