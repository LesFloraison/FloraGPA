#include "NativeCountersView.h"
#include "application/NativeCounters.h"
#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>
namespace flora {
namespace {
using Json = nlohmann::json;
QString text(const Json &v) {
    return v.is_null() ? QString("—")
                       : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
QString unit(const Json &v) {
    auto s = text(v);
    return s == "seconds" ? "s" : s;
}
QTreeWidget *table(const char *name, const QStringList &columns) {
    auto tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setHeaderLabels(columns);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    return tree;
}
} // namespace
NativeCountersView::NativeCountersView(QWidget *parent) : QWidget(parent) {
    setObjectName("nativeCountersView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    read_ = bar->addAction("Measure");
    read_->setObjectName("readNativeCounters");
    cancel_ = bar->addAction("Cancel");
    cancel_->setObjectName("cancelNativeCounters");
    cancel_->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    qobject_cast<QToolButton *>(bar->widgetForAction(cancel_))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportNativeCounters");
    auto more = new QToolButton;
    more->setText("More");
    more->setPopupMode(QToolButton::InstantPopup);
    auto menu = new QMenu(more);
    more->setMenu(menu);
    bar->addWidget(more);
    auto detail = menu->addAction("Details");
    detail->setObjectName("nativeCountersDetailsToggle");
    detail->setCheckable(true);
    auto row = new QToolBar;
    layout->addWidget(row);
    scope_ = new QComboBox;
    scope_->setObjectName("nativeCountersScope");
    scope_->addItems({"Selection", "Frame"});
    row->addWidget(scope_);
    filter_ = new QLineEdit;
    filter_->setObjectName("nativeCountersFilter");
    filter_->setPlaceholderText("Filter metrics…");
    row->addWidget(filter_);
    locate_ = row->addAction("Locate");
    locate_->setObjectName("locateNativeCounter");
    summary_ = new QLabel("No sample");
    summary_->setObjectName("nativeCountersSummary");
    summary_->setMargin(4);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    summary_->setToolTip("Measured on the replay GPU; GPA vendor-specific metrics are not implied.");
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);
    auto tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setObjectName("nativeCounterTabs");
    split->addWidget(tabs);
    values_ = table("nativeCounterValues", {"Metric", "Value", "Unit"});
    values_->setColumnWidth(1, 95);
    tabs->addTab(values_, "Values");
    catalog_ = table("nativeCounterCatalog", {"Metric", "Type", "Bytes"});
    catalog_->setColumnWidth(1, 55);
    catalog_->setRootIsDecorated(false);
    tabs->addTab(catalog_, "Available");
    details_ = new QPlainTextEdit;
    details_->setObjectName("nativeCounterDetails");
    details_->setReadOnly(true);
    split->addWidget(details_);
    details_->hide();
    connect(detail, &QAction::toggled, this, [this, split](bool visible) {
        details_->setVisible(visible);
        if (visible) {
            if (!result_.is_null() && details_->document()->isEmpty())
                details_->setPlainText(QString::fromStdString(result_.dump(2)));
            split->setSizes({split->height() * 3 / 4, split->height() / 4});
        }
    });
    connect(read_, &QAction::triggered, this, [this] {
        if (busy_ || key_.isEmpty())
            return;
        invalidate();
        summary_->setText("Measuring…");
        emit readRequested();
    });
    connect(cancel_, &QAction::triggered, this, &NativeCountersView::cancelRequested);
    connect(export_, &QAction::triggered, this, [this] {
        try {
            auto path =
                QFileDialog::getSaveFileName(this, "Export replay metrics", "counters.json", "JSON (*.json)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    });
    connect(scope_, &QComboBox::currentIndexChanged, this, &NativeCountersView::render);
    connect(filter_, &QLineEdit::textChanged, this, &NativeCountersView::render);
    connect(values_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActions();
        auto item = values_->currentItem();
        if (item && item->data(0, Qt::UserRole + 1).isValid())
            details_->setPlainText(
                QString::fromStdString(rows_.at(item->data(0, Qt::UserRole + 1).toULongLong()).dump(2)));
    });
    connect(catalog_, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto item = catalog_->currentItem();
        if (item)
            details_->setPlainText(QString::fromStdString(
                result_.at("data").at("catalog").at(item->data(0, Qt::UserRole).toULongLong()).dump(2)));
    });
    connect(locate_, &QAction::triggered, this, [this] {
        auto item = values_->currentItem();
        if (item && !busy_ && item->data(0, Qt::UserRole).toULongLong())
            emit eventRequested(item->data(0, Qt::UserRole).toULongLong());
    });
    connect(values_, &QTreeWidget::itemDoubleClicked, this, [this] { locate_->trigger(); });
    updateActions();
}
void NativeCountersView::setContext(const QString &key, qulonglong event) {
    const bool changed = key != key_;
    const bool selectionChanged = event != event_;
    key_ = key;
    event_ = event;
    if (changed)
        invalidate();
    else if (selectionChanged) {
        details_->clear();
        render();
    }
    updateActions();
}
void NativeCountersView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void NativeCountersView::invalidate() {
    const QSignalBlocker blockValues(values_), blockCatalog(catalog_);
    ++requestId_;
    result_ = nullptr;
    rows_ = nullptr;
    values_->clear();
    catalog_->clear();
    details_->clear();
    summary_->setText("No sample");
    updateActions();
}
void NativeCountersView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty());
    cancel_->setEnabled(busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    locate_->setEnabled(!busy_ && values_->currentItem() &&
                        values_->currentItem()->data(0, Qt::UserRole).toULongLong());
}
NativeCountersView::Json NativeCountersView::request() const {
    if (key_.isEmpty())
        throw std::runtime_error("Open a frame first");
    return {{"action", "counters"}};
}
bool NativeCountersView::finish(uint64_t request, const Json &result) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(text(result.value("error", Json("Measurement failed"))));
        emit inspectionFinished(false);
        return false;
    }
    auto rows = nativeCounterRows(result.at("data"));
    rows_ = std::move(rows);
    result_ = result;
    render();
    if (!details_->isHidden())
        details_->setPlainText(QString::fromStdString(result_.dump(2)));
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void NativeCountersView::render() {
    if (result_.is_null())
        return;
    const QSignalBlocker blockValues(values_), blockCatalog(catalog_);
    values_->clear();
    catalog_->clear();
    const auto filter = filter_->text();
    size_t shown = 0;
    std::map<uint64_t, QTreeWidgetItem *> groups;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto &r = rows_[i];
        const auto gpa = r.at("event");
        if (!scope_->currentIndex() && (gpa.is_null() || gpa != event_))
            continue;
        if (!filter.isEmpty() && !text(r.at("name")).contains(filter, Qt::CaseInsensitive) &&
            !text(gpa).contains(filter))
            continue;
        QTreeWidgetItem *parent = nullptr;
        const auto eid = r.at("event").get<uint64_t>();
        const auto id = gpa.is_null() ? qulonglong(0) : gpa.get<qulonglong>();
        if (scope_->currentIndex()) {
            auto &group = groups[eid];
            if (!group) {
                group = new QTreeWidgetItem(values_, {QString("API %1").arg(id)});
                group->setData(0, Qt::UserRole, id);
                group->setToolTip(0, QString("GPA event %1").arg(eid));
            }
            parent = group;
        }
        const QStringList columns{text(r.at("name")), text(r.at("value")), unit(r.at("unit"))};
        auto item = parent ? new QTreeWidgetItem(parent, columns) : new QTreeWidgetItem(values_, columns);
        item->setData(0, Qt::UserRole, id);
        item->setData(0, Qt::UserRole + 1, qulonglong(i));
        for (int col = 0; col < 3; ++col)
            item->setToolTip(col, item->text(col));
        item->setToolTip(0, item->text(0) +
                                QString("\nAPI %1 · Counter %2").arg(text(gpa)).arg(text(r.at("counter"))));
        ++shown;
    }
    for (size_t i = 0; i < result_.at("data").at("catalog").size(); ++i) {
        const auto &d = result_.at("data").at("catalog")[i];
        if (!filter.isEmpty() && !text(d.at("name")).contains(filter, Qt::CaseInsensitive) &&
            !text(d.at("id")).contains(filter, Qt::CaseInsensitive))
            continue;
        auto item = new QTreeWidgetItem(catalog_, {text(d.at("name")), text(d.at("type")), "8"});
        item->setData(0, Qt::UserRole, qulonglong(i));
        item->setToolTip(0, text(d.at("id")) + "\n" + text(d.at("unit")) + "\n" +
                                text(d.value("description", Json(""))));
    }
    values_->setRootIsDecorated(scope_->currentIndex() != 0);
    if (groups.size() <= 16)
        values_->expandAll();
    summary_->setText(
        QString("%1 values · %2 available").arg(shown).arg(result_.at("data").at("catalog").size()));
    if (result_.contains("context") && result_.at("context").contains("adapter")) {
        const auto &adapter = result_.at("context").at("adapter");
        summary_->setText(summary_->text() + " · " + text(adapter.at("description")));
        summary_->setToolTip(QString::fromStdString(result_.at("context").dump(2)));
    }
    updateActions();
}
void NativeCountersView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No counters to export");
    const auto bytes = result_.dump(2);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot export replay metrics");
}
} // namespace flora
