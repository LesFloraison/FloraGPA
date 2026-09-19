#include "CapturedStateView.h"
#include "application/CommandState.h"
#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSaveFile>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace flora {
CapturedStateView::CapturedStateView(QWidget *parent) : QWidget(parent) {
    setObjectName("capturedStateView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    boundary_ = new QComboBox;
    boundary_->setObjectName("stateBoundary");
    boundary_->addItems({"Before command", "After command"});
    bar->addWidget(boundary_);
    read_ = bar->addAction("Read");
    read_->setObjectName("readCapturedState");
    read_->setEnabled(false);
    export_ = bar->addAction("Export JSON…");
    export_->setObjectName("exportCapturedState");
    export_->setEnabled(false);
    bar->addSeparator();
    known_ = new QComboBox;
    known_->setObjectName("stateKnowledge");
    known_->addItems({"All fields", "Known", "Unknown"});
    bar->addWidget(known_);
    search_ = new QLineEdit;
    search_->setObjectName("stateSearch");
    search_->setPlaceholderText("Filter fields…");
    search_->setClearButtonEnabled(true);
    bar->addWidget(search_);
    layout->addWidget(bar);
    summary_ = new QLabel("Original capture");
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    summary_->setMargin(6);
    summary_->setObjectName("stateSummary");
    layout->addWidget(summary_);
    fields_ = new QTreeWidget;
    fields_->setObjectName("capturedStateFields");
    fields_->setHeaderLabels({"Field", "Value", "Source", "Event"});
    fields_->setRootIsDecorated(false);
    fields_->setAlternatingRowColors(true);
    fields_->setUniformRowHeights(true);
    fields_->setColumnWidth(0, 205);
    fields_->setColumnWidth(1, 230);
    fields_->setColumnWidth(2, 125);
    fields_->header()->setStretchLastSection(true);
    layout->addWidget(fields_);
    connect(read_, &QAction::triggered, this, &CapturedStateView::read);
    connect(boundary_, &QComboBox::currentIndexChanged, this, [this] {
        ++revision_;
        clear();
    });
    connect(search_, &QLineEdit::textChanged, this, &CapturedStateView::filter);
    connect(known_, &QComboBox::currentIndexChanged, this, &CapturedStateView::filter);
    connect(fields_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *row, int column) {
        auto id = row->data(column, Qt::UserRole).toULongLong();
        if (id && column == 1)
            emit resourceRequested(id);
        if (id && column == 3)
            emit eventRequested(id);
    });
    connect(export_, &QAction::triggered, this, [this] {
        if (result_.is_null())
            return;
        auto path = QFileDialog::getSaveFileName(this, "Export Captured State", "command-state.json",
                                                 "JSON (*.json)");
        if (path.isEmpty())
            return;
        auto bytes = QByteArray::fromStdString(result_.dump(2) + "\n");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            summary_->setText("Export failed");
    });
    connect(&watcher_, &QFutureWatcher<nlohmann::json>::finished, this, [this] {
        read_->setEnabled(bool(frame_) && event_);
        if (runningRevision_ != revision_) {
            emit inspectionFinished(false);
            return;
        }
        result_ = watcher_.result();
        if (result_.contains("error")) {
            summary_->setText("Inspection failed");
            summary_->setToolTip(QString::fromStdString(result_["error"]));
            result_ = nullptr;
            emit inspectionFinished(false);
            return;
        }
        populate();
        export_->setEnabled(true);
        emit inspectionFinished(true);
    });
}
CapturedStateView::~CapturedStateView() { watcher_.waitForFinished(); }
void CapturedStateView::setSelection(std::shared_ptr<const Frame> frame, Id event) {
    frame_ = std::move(frame);
    event_ = event;
    ++revision_;
    clear();
}
void CapturedStateView::clear() {
    result_ = nullptr;
    fields_->clear();
    export_->setEnabled(false);
    read_->setEnabled(bool(frame_) && event_ && !watcher_.isRunning());
    summary_->setText(event_ ? QString("Event %1 · Original capture").arg(event_) : "Original capture");
    summary_->setToolTip({});
}
void CapturedStateView::read() {
    if (!frame_ || !event_ || watcher_.isRunning())
        return;
    runningRevision_ = revision_;
    auto frame = frame_;
    auto event = event_;
    bool after = boundary_->currentIndex() == 1;
    read_->setEnabled(false);
    export_->setEnabled(false);
    summary_->setText("Reconstructing…");
    watcher_.setFuture(QtConcurrent::run([frame, event, after] {
        try {
            return inspectCommandState(*frame, event, after);
        } catch (const std::exception &e) {
            return nlohmann::json{{"error", e.what()}};
        }
    }));
}
void CapturedStateView::populate() {
    fields_->clear();
    if (result_.contains("unavailable")) {
        summary_->setText("State unavailable");
        summary_->setToolTip(QString::fromStdString(result_["unavailable"]));
        return;
    }
    summary_->setText(QString("Event %1 · Context %2 · Original capture · %3 known / %4 unknown")
                          .arg(event_)
                          .arg(result_["context"].get<Id>())
                          .arg(result_["known_fields"].get<uint64_t>())
                          .arg(result_["unknown_fields"].get<uint64_t>()));
    summary_->setToolTip(QString::fromStdString(
        nlohmann::json{{"notes", result_["notes"]}, {"limits", result_["limits"]}}.dump(2)));
    for (const auto &field : result_["fields"]) {
        const auto &value = field["value"], &source = field["source"];
        bool known = field["known"];
        auto text = known
                        ? QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump())
                        : QString("Unknown");
        auto event = source["event"].is_null() ? QString{} : QString::number(source["event"].get<Id>());
        auto row = new QTreeWidgetItem(fields_, {QString::fromStdString(field["field"]), text,
                                                 QString::fromStdString(source["kind"]), event});
        row->setData(0, Qt::UserRole, known);
        row->setToolTip(0, row->text(0));
        row->setToolTip(1, text);
        row->setToolTip(2, QString::fromStdString(source.dump(2)));
        if (!source["event"].is_null()) {
            row->setData(3, Qt::UserRole, QVariant::fromValue<qulonglong>(source["event"].get<Id>()));
            row->setToolTip(3, "Double-click to locate API event");
        }
        if (!field["resource_id"].is_null()) {
            auto id = field["resource_id"].get<Id>();
            auto found = frame_->entries().find(id);
            if (found != frame_->entries().end() && found->second.category == 5) {
                row->setData(1, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                row->setToolTip(1, text + "\nDouble-click to inspect resource");
            }
        }
    }
    filter();
}
void CapturedStateView::filter() {
    auto term = search_->text().trimmed();
    for (int i = 0; i < fields_->topLevelItemCount(); ++i) {
        auto row = fields_->topLevelItem(i);
        bool known = row->data(0, Qt::UserRole).toBool();
        bool match = (known_->currentIndex() == 0 || known == (known_->currentIndex() == 1));
        if (!term.isEmpty()) {
            QString joined;
            for (int c = 0; c < 4; ++c)
                joined += row->text(c) + ' ';
            match &= joined.contains(term, Qt::CaseInsensitive);
        }
        row->setHidden(!match);
    }
}
} // namespace flora
