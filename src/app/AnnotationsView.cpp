#include "AnnotationsView.h"
#include "application/Annotations.h"
#include <QAction>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <map>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
QString text(const Json &value) {
    return value.is_null()
               ? QString{}
               : QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
}
void detail(QTreeWidget *view, QTreeWidgetItem *parent, const QString &key, const Json &value) {
    auto row = parent ? new QTreeWidgetItem(parent, {key, value.is_structured() ? QString{} : text(value)})
                      : new QTreeWidgetItem(view, {key, value.is_structured() ? QString{} : text(value)});
    row->setToolTip(1, text(value));
    if (value.is_object())
        for (auto it = value.begin(); it != value.end(); ++it)
            detail(view, row, QString::fromStdString(it.key()), it.value());
    else if (value.is_array())
        for (size_t i = 0; i < value.size(); ++i)
            detail(view, row, QString::number(i), value[i]);
}
} // namespace
AnnotationsView::AnnotationsView(QWidget *parent) : QWidget(parent) {
    setObjectName("annotationsView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    read_ = bar->addAction("Read");
    read_->setObjectName("readAnnotations");
    filter_ = new QLineEdit;
    filter_->setObjectName("annotationFilter");
    filter_->setPlaceholderText("Name / ID…");
    filter_->setClearButtonEnabled(true);
    bar->addWidget(filter_);
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportAnnotations");
    layout->addWidget(bar);
    auto navigation = new QToolBar;
    begin_ = navigation->addAction("Locate Begin");
    end_ = navigation->addAction("Locate End");
    draw_ = navigation->addAction("Locate Draw");
    begin_->setObjectName("locateAnnotationBegin");
    end_->setObjectName("locateAnnotationEnd");
    draw_->setObjectName("locateAnnotationDraw");
    auto range = navigation->addAction("Range Metrics");
    range->setEnabled(false);
    range->setToolTip("Range statistics migration is pending");
    layout->addWidget(navigation);
    summary_ = new QLabel("No capture");
    summary_->setMargin(6);
    summary_->setObjectName("annotationSummary");
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    auto makeTree = [](const QStringList &labels, const char *name) {
        auto view = new QTreeWidget;
        view->setObjectName(name);
        view->setHeaderLabels(labels);
        view->setUniformRowHeights(true);
        view->setAlternatingRowColors(true);
        view->header()->setStretchLastSection(true);
        return view;
    };
    nodes_ = makeTree({"Annotation", "Begin / API", "End", "Status"}, "annotationNodes");
    nodes_->setColumnWidth(0, 280);
    nodes_->setColumnWidth(1, 125);
    nodes_->setColumnWidth(2, 125);
    split->addWidget(nodes_);
    auto tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    auto memberPane = new QWidget;
    auto memberLayout = new QVBoxLayout(memberPane);
    memberLayout->setContentsMargins(0, 0, 0, 0);
    membership_ = new QLabel;
    membership_->setObjectName("annotationMembership");
    membership_->setMargin(6);
    membership_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    memberLayout->addWidget(membership_);
    members_ = makeTree({"Draw / Dispatch", "API call", "Context"}, "annotationMembers");
    members_->setRootIsDecorated(false);
    members_->setColumnWidth(0, 150);
    members_->setColumnWidth(1, 210);
    memberLayout->addWidget(members_);
    details_ = makeTree({"Field", "Value"}, "annotationDetails");
    details_->setColumnWidth(0, 255);
    tabs->addTab(memberPane, "Members");
    tabs->addTab(details_, "Details");
    split->addWidget(tabs);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    layout->addWidget(split);
    connect(read_, &QAction::triggered, this, &AnnotationsView::read);
    connect(filter_, &QLineEdit::textChanged, this, &AnnotationsView::populate);
    connect(nodes_, &QTreeWidget::itemSelectionChanged, this, &AnnotationsView::selected);
    connect(members_, &QTreeWidget::itemSelectionChanged, this, &AnnotationsView::updateActions);
    connect(nodes_, &QTreeWidget::itemDoubleClicked, this, [this] { navigate(false); });
    connect(members_, &QTreeWidget::itemDoubleClicked, this, [this] { navigateDraw(); });
    connect(begin_, &QAction::triggered, this, [this] { navigate(false); });
    connect(end_, &QAction::triggered, this, [this] { navigate(true); });
    connect(draw_, &QAction::triggered, this, &AnnotationsView::navigateDraw);
    connect(export_, &QAction::triggered, this, [this] {
        if (report_.is_null() || workerBusy_)
            return;
        const auto path = QFileDialog::getExistingDirectory(this, "Export Annotations");
        if (path.isEmpty())
            return;
        try {
            exportAnnotations(report_, std::filesystem::path(path.toStdWString()));
            summary_->setText("Annotations exported");
            summary_->setToolTip(path);
        } catch (const std::exception &error) {
            summary_->setText("Export failed");
            summary_->setToolTip(QString::fromUtf8(error.what()));
        }
    });
    connect(&watcher_, &QFutureWatcher<Json>::finished, this, [this] {
        auto result = watcher_.result();
        if (runningRevision_ != revision_) {
            updateActions();
            emit inspectionFinished(false);
            return;
        }
        const bool success = !result.contains("error");
        report_ = success ? result : Json(nullptr);
        populate();
        if (!success) {
            summary_->setText("Inspection failed");
            summary_->setToolTip(text(result.at("error")));
        }
        updateActions();
        emit inspectionFinished(success);
    });
    updateActions();
}
AnnotationsView::~AnnotationsView() { watcher_.waitForFinished(); }
void AnnotationsView::setFrame(std::shared_ptr<const Frame> frame) {
    ++revision_;
    frame_ = std::move(frame);
    report_ = nullptr;
    filter_->clear();
    populate();
}
void AnnotationsView::setWorkerBusy(bool busy) {
    workerBusy_ = busy;
    updateActions();
}
void AnnotationsView::read() {
    if (!frame_ || watcher_.isRunning() || workerBusy_)
        return;
    runningRevision_ = revision_;
    report_ = nullptr;
    populate();
    summary_->setText("Reading annotations…");
    watcher_.setFuture(QtConcurrent::run([frame = frame_] {
        try {
            return inspectAnnotations(*frame);
        } catch (const std::exception &error) {
            return Json{{"error", error.what()}};
        }
    }));
    updateActions();
}
const Json *AnnotationsView::selection() const {
    if (report_.is_null() || nodes_->selectedItems().empty())
        return nullptr;
    auto item = nodes_->selectedItems().front();
    if (!item->data(0, Qt::UserRole).isValid())
        return nullptr;
    const auto id = item->data(0, Qt::UserRole).toULongLong();
    for (const auto &node : report_.at("nodes"))
        if (node.at("id") == id)
            return &node;
    return nullptr;
}
void AnnotationsView::populate() {
    nodes_->clear();
    members_->clear();
    details_->clear();
    membership_->clear();
    membership_->setToolTip({});
    summary_->setToolTip({});
    if (report_.is_null()) {
        summary_->setText(frame_ ? "Original capture" : "No capture");
        updateActions();
        return;
    }
    std::map<Id, const Json *> byId;
    std::set<Id> visible;
    const auto terms = filter_->text().toCaseFolded().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const auto &node : report_.at("nodes"))
        byId[node.at("id").get<Id>()] = &node;
    for (const auto &[id, node] : byId) {
        const auto owner = node->at("object").is_null() ? QString("None") : text(node->at("object"));
        const auto value =
            (text(node->at("id")) + ' ' + owner + ' ' + text(node->at("display_name"))).toCaseFolded();
        if (std::all_of(terms.begin(), terms.end(),
                        [&](const QString &term) { return value.contains(term); })) {
            const Json *parent = node;
            while (parent && visible.insert(parent->at("id").get<Id>()).second) {
                const auto next = parent->at("parent");
                parent = next.is_null() ? nullptr : byId.at(next.get<Id>());
            }
        }
    }
    std::map<QString, QTreeWidgetItem *> owners;
    std::map<Id, QTreeWidgetItem *> rows;
    for (const auto &node : report_.at("nodes")) {
        const auto id = node.at("id").get<Id>();
        if (!visible.contains(id))
            continue;
        const auto owner = node.at("object").is_null() ? QString("Unknown") : text(node.at("object"));
        if (!owners.contains(owner))
            owners[owner] = new QTreeWidgetItem(nodes_, {"Object " + owner});
        auto parent = node.at("parent").is_null() ? owners.at(owner) : rows.at(node.at("parent").get<Id>());
        auto name = text(node.at("display_name"));
        auto status = text(node.at("status"));
        status.replace('_', ' ');
        auto row = new QTreeWidgetItem(
            parent, {name.isEmpty() ? "(empty)" : name, QString::number(id), text(node.at("end")), status});
        row->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        row->setToolTip(0, name);
        row->setToolTip(3, text(node.at("status")));
        rows[id] = row;
    }
    nodes_->expandAll();
    summary_->setText(QString("%1 records · %2 nodes · %3 issues")
                          .arg(report_.at("record_count").get<qulonglong>())
                          .arg(visible.size())
                          .arg(report_.at("issues").size()));
    summary_->setToolTip(text(report_.at("issues")));
    updateActions();
}
void AnnotationsView::selected() {
    members_->clear();
    details_->clear();
    membership_->clear();
    membership_->setToolTip({});
    const auto node = selection();
    if (node) {
        for (const auto &draw : node->at("draws")) {
            auto row = new QTreeWidgetItem(
                members_, {text(draw.at("id")), text(draw.at("name")), text(draw.at("context"))});
            row->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(draw.at("id").get<Id>()));
        }
        for (auto it = node->begin(); it != node->end(); ++it)
            detail(details_, nullptr, QString::fromStdString(it.key()), it.value());
        if (node->contains("membership_range"))
            membership_->setText(QString("%1 linked · %2 unmatched%3")
                                     .arg(node->at("draws").size())
                                     .arg(node->at("unmatched_draw_ids").size())
                                     .arg(node->at("membership_range").at("complete_group").get<bool>()
                                              ? ""
                                              : " · Capture suffix"));
        else
            membership_->setText("Membership unresolved");
        membership_->setToolTip(text(node->at("context_association")) +
                                "\nContexts: " + text(node->at("context_ids")) +
                                "\nProof events: " + text(node->at("context_proof_events")));
    }
    updateActions();
}
void AnnotationsView::updateActions() {
    const bool ready = !workerBusy_ && !watcher_.isRunning();
    const auto node = selection();
    read_->setEnabled(ready && bool(frame_));
    export_->setEnabled(ready && !report_.is_null());
    begin_->setEnabled(ready && node);
    end_->setEnabled(ready && node && !node->at("end").is_null());
    draw_->setEnabled(ready && node && !members_->selectedItems().empty());
}
void AnnotationsView::navigate(bool end) {
    const auto node = selection();
    if (!node || workerBusy_)
        return;
    const auto id = node->at(end ? "end" : "id");
    if (!id.is_null())
        emit eventRequested(id.get<Id>());
}
void AnnotationsView::navigateDraw() {
    if (!selection() || workerBusy_ || members_->selectedItems().empty())
        return;
    emit eventRequested(members_->selectedItems().front()->data(0, Qt::UserRole).toULongLong());
}
} // namespace flora
