#include "FrameRangeDialog.h"
#include "application/ApiCommands.h"
#include "application/MetricIterations.h"
#include <QAction>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <set>
namespace flora {
FrameRangeDialog::FrameRangeDialog(const Frame &frame, const Experiment &experiment, Id current, int scope,
                                   const QString &indices, QWidget *parent)
    : QDialog(parent) {
    setObjectName("frameRangeDialog");
    setWindowTitle("Frame metric ranges");
    setWindowModality(Qt::WindowModal);
    resize(780, 520);
    setMinimumSize(560, 340);
    auto layout = new QVBoxLayout(this);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    auto all = bar->addAction("All"), clear = bar->addAction("Clear"), here = bar->addAction("Current API");
    all->setObjectName("rangeAll");
    clear->setObjectName("rangeClear");
    here->setObjectName("rangeCurrent");
    ranges_ = new QTreeWidget;
    ranges_->setObjectName("frameRanges");
    ranges_->setHeaderLabels({"Range", "API", "Command", "State"});
    ranges_->setRootIsDecorated(false);
    ranges_->setUniformRowHeights(true);
    ranges_->setAlternatingRowColors(true);
    ranges_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ranges_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ranges_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    ranges_->setToolTip("Ctrl / Shift to select ranges. Disabled commands retain their range boundaries.");
    layout->addWidget(ranges_, 1);
    status_ = new QLabel;
    status_->setObjectName("rangeStatus");
    layout->addWidget(status_);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Apply)->setObjectName("rangeApply");
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
        if (ranges_->selectedItems().empty()) {
            status_->setText("Select at least one range");
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(all, &QAction::triggered, ranges_, &QTreeWidget::selectAll);
    connect(clear, &QAction::triggered, ranges_, &QTreeWidget::clearSelection);
    connect(here, &QAction::triggered, this, [this, current] {
        QList<QTreeWidgetItem *> found;
        for (int i = 0; i < ranges_->topLevelItemCount(); ++i) {
            auto row = ranges_->topLevelItem(i);
            if (row->data(1, Qt::UserRole).toULongLong() == current)
                found.push_back(row);
        }
        if (found.empty()) {
            status_->setText(QString("API %1 has no range endpoint").arg(current));
            return;
        }
        ranges_->clearSelection();
        for (auto row : found)
            row->setSelected(true);
        ranges_->scrollToItem(found.front());
    });
    const auto index = buildFrameMetricIndex(frame);
    std::set<uint64_t> selected;
    if (scope == 5) {
        auto input = indices;
        input.replace(QChar(0xff0c), ',');
        for (const auto &part : input.split(',')) {
            bool ok{};
            auto n = part.trimmed().toULongLong(&ok);
            if (!ok || part.trimmed().startsWith('-')) {
                selected.clear();
                break;
            }
            selected.insert(n);
        }
    }
    size_t n{};
    for (const auto &range : index.at("ranges").at("2")) {
        const auto event = index.at("ergs").at(range.at(2).get<size_t>()).get<Id>();
        const auto &entry = frame.entries().at(event);
        auto row = new QTreeWidgetItem(ranges_, {QString::number(n), QString::number(event),
                                                 QString::fromStdString(commandName(entry.type)),
                                                 experiment.enabled(event) ? "Enabled" : "Disabled"});
        row->setData(0, Qt::UserRole, qulonglong(n));
        row->setData(1, Qt::UserRole, qulonglong(event));
        row->setSelected(scope == 6 || selected.contains(n));
        ++n;
    }
    connect(ranges_, &QTreeWidget::itemSelectionChanged, this, &FrameRangeDialog::refresh);
    refresh();
    if (!ranges_->selectedItems().empty())
        ranges_->scrollToItem(ranges_->selectedItems().front());
}
QString FrameRangeDialog::selectedIndices() const {
    std::set<qulonglong> selected;
    for (auto row : ranges_->selectedItems())
        selected.insert(row->data(0, Qt::UserRole).toULongLong());
    QStringList result;
    for (auto n : selected)
        result << QString::number(n);
    return result.join(", ");
}
void FrameRangeDialog::refresh() {
    status_->setText(QString("%1 ranges · %2 selected")
                         .arg(ranges_->topLevelItemCount())
                         .arg(ranges_->selectedItems().size()));
}
} // namespace flora
