#include "StatisticsView.h"
#include "application/GpuStatistics.h"
#include <QAction>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace flora {
using Json = nlohmann::json;
namespace {
QString valueText(const Json &value) {
    return value.is_null()
               ? QString("—")
               : QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
}
void details(QTreeWidget *view, QTreeWidgetItem *parent, const QString &key, const Json &value) {
    auto row = parent
                   ? new QTreeWidgetItem(parent, {key, value.is_structured() ? QString{} : valueText(value)})
                   : new QTreeWidgetItem(view, {key, value.is_structured() ? QString{} : valueText(value)});
    row->setToolTip(1, valueText(value));
    if (value.is_object())
        for (auto it = value.begin(); it != value.end(); ++it)
            details(view, row, QString::fromStdString(it.key()), it.value());
    else if (value.is_array())
        for (size_t i = 0; i < value.size(); ++i)
            details(view, row, QString::number(i), value[i]);
}
} // namespace
StatisticsView::StatisticsView(QWidget *parent) : QWidget(parent) {
    setObjectName("gpuStatisticsView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    eventAction_ = bar->addAction("Measure Event");
    eventAction_->setObjectName("measureStatisticsEvent");
    rangeAction_ = bar->addAction("Measure Range");
    rangeAction_->setObjectName("measureStatisticsRange");
    frameAction_ = bar->addAction("Measure Frame");
    frameAction_->setObjectName("measureStatisticsFrame");
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportStatistics");
    layout->addWidget(bar);
    auto range = new QToolBar;
    start_ = new QLineEdit;
    start_->setPlaceholderText("Start API ID");
    start_->setObjectName("statisticsStart");
    start_->setMaximumWidth(190);
    end_ = new QLineEdit;
    end_->setPlaceholderText("End API ID");
    end_->setObjectName("statisticsEnd");
    end_->setMaximumWidth(190);
    range->addWidget(start_);
    range->addWidget(end_);
    setStart_ = range->addAction("Start ← Selection");
    setStart_->setObjectName("statisticsStartSelection");
    setEnd_ = range->addAction("End ← Selection");
    setEnd_->setObjectName("statisticsEndSelection");
    layout->addWidget(range);
    summary_ = new QLabel("No sample");
    summary_->setMargin(6);
    summary_->setObjectName("statisticsSummary");
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    auto tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    values_ = new QTreeWidget;
    values_->setObjectName("statisticsValues");
    values_->setHeaderLabels({"Metric", "Value", "Unit"});
    values_->setColumnWidth(0, 310);
    values_->setColumnWidth(1, 210);
    values_->setRootIsDecorated(false);
    values_->setAlternatingRowColors(true);
    values_->setUniformRowHeights(true);
    details_ = new QTreeWidget;
    details_->setObjectName("statisticsDetails");
    details_->setHeaderLabels({"Field", "Value"});
    details_->setColumnWidth(0, 260);
    details_->setUniformRowHeights(true);
    details_->setAlternatingRowColors(true);
    tabs->addTab(values_, "Metrics");
    tabs->addTab(details_, "Details");
    layout->addWidget(tabs);
    connect(start_, &QLineEdit::textChanged, this, &StatisticsView::invalidate);
    connect(end_, &QLineEdit::textChanged, this, &StatisticsView::invalidate);
    connect(setStart_, &QAction::triggered, this, [this] {
        if (!busy_)
            start_->setText(QString::number(event_));
    });
    connect(setEnd_, &QAction::triggered, this, [this] {
        if (!busy_)
            end_->setText(QString::number(event_));
    });
    connect(eventAction_, &QAction::triggered, this, [this] { read(true); });
    connect(rangeAction_, &QAction::triggered, this, [this] { read(false); });
    connect(frameAction_, &QAction::triggered, this, [this] {
        if (!frame_ || busy_)
            return;
        Id first = 0, last = 0;
        for (const auto &[id, e] : frame_->entries())
            if (e.category == 7) {
                if (!first)
                    first = id;
                last = id;
            }
        if (first) {
            setRange(first, last);
            read(false);
        }
    });
    connect(export_, &QAction::triggered, this, [this] {
        if (result_.is_null() || busy_)
            return;
        auto path = QFileDialog::getExistingDirectory(this, "Export GPU Statistics");
        if (path.isEmpty())
            return;
        try {
            exportGpuStatistics(result_, path.toStdWString());
        } catch (const std::exception &error) {
            summary_->setText("Export failed");
            summary_->setToolTip(QString::fromUtf8(error.what()));
        }
    });
    updateActions();
}
void StatisticsView::invalidate() {
    ++revision_;
    result_ = nullptr;
    values_->clear();
    details_->clear();
    summary_->setText("No sample");
    summary_->setToolTip({});
    updateActions();
}
void StatisticsView::setSelection(std::shared_ptr<const Frame> frame, Id event) {
    if (frame_ == frame && event_ == event)
        return;
    if (frame_ != frame) {
        QSignalBlocker a(start_), b(end_);
        start_->clear();
        end_->clear();
    }
    frame_ = std::move(frame);
    event_ = event;
    invalidate();
}
void StatisticsView::setRange(Id start, Id end) {
    QSignalBlocker a(start_), b(end_);
    start_->setText(QString::number(start));
    end_->setText(QString::number(end));
    invalidate();
}
void StatisticsView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void StatisticsView::updateActions() {
    const bool selected = frame_ && frame_->entries().contains(event_) && frame_->entry(event_).category == 7;
    eventAction_->setEnabled(!busy_ && selected && isDraw(frame_->entry(event_).type));
    rangeAction_->setEnabled(!busy_ && bool(frame_));
    frameAction_->setEnabled(!busy_ && bool(frame_));
    setStart_->setEnabled(!busy_ && selected);
    setEnd_->setEnabled(!busy_ && selected);
    start_->setEnabled(!busy_);
    end_->setEnabled(!busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
}
void StatisticsView::read(bool single) {
    if (!frame_ || busy_)
        return;
    bool a = false, b = false;
    Id start = single ? event_ : start_->text().toULongLong(&a),
       end = single ? event_ : end_->text().toULongLong(&b);
    if (single)
        a = b = true;
    if (!a || !b || !start || start > end || !frame_->entries().contains(start) ||
        !frame_->entries().contains(end) || frame_->entry(start).category != 7 ||
        frame_->entry(end).category != 7 || (single && !isDraw(frame_->entry(start).type))) {
        summary_->setText("Select valid API endpoints");
        return;
    }
    invalidate();
    summary_->setText("Measuring…");
    emit readRequested(start, end, single, revision_);
}
bool StatisticsView::finish(uint64_t request, const Json &result) {
    if (request != revision_) {
        emit inspectionFinished(false);
        return false;
    }
    if (result.is_null() || result.contains("error")) {
        invalidate();
        summary_->setText("Measurement failed");
        summary_->setToolTip(result.is_null() ? QString{} : valueText(result.at("error")));
        emit inspectionFinished(false);
        return false;
    }
    try {
        values_->clear();
        details_->clear();
        auto row = [&](const QString &name, const Json &value, const char *unit) {
            new QTreeWidgetItem(values_, {name, valueText(value), unit});
        };
        for (auto it = result.at("pipeline").begin(); it != result.at("pipeline").end(); ++it)
            row(QString::fromStdString(it.key()), it.value(), "count");
        row("occlusion_samples", result.at("occlusion_samples"), "samples");
        for (const auto &stream : result.at("stream_output").at("streams"))
            for (auto key : {"primitives_written", "primitives_storage_needed"})
                row(QString("SO %1 · %2").arg(stream.at("stream").get<unsigned>()).arg(key), stream.at(key),
                    "primitives");
        row("SO overflow", result.at("stream_output").at("overflow"), "bool");
        row("GPU time", result.at("timing").at("elapsed_ms"), "ms");
        for (auto it = result.begin(); it != result.end(); ++it)
            details(details_, nullptr, QString::fromStdString(it.key()), it.value());
        const auto scope = result.at("scope") == "single_event"
                               ? "Event " + valueText(result.at("event"))
                               : "Range " + valueText(result.at("range").at("start")) + "–" +
                                     valueText(result.at("range").at("end"));
        summary_->setText(scope + " · " + valueText(result.at("execution_device").at("selected")) +
                          " · Single sample");
        summary_->setToolTip(valueText(result.at("execution_device").at("adapter").at("description")) + "\n" +
                             valueText(result.at("notes")));
        result_ = result;
        updateActions();
        emit inspectionFinished(true);
        return true;
    } catch (const std::exception &error) {
        return finish(request, {{"error", error.what()}});
    }
}
} // namespace flora
