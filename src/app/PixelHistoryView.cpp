#include "PixelHistoryView.h"
#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <Windows.h>

namespace flora {
using Json = nlohmann::json;


namespace {
QString scalar(const Json &v) {
    if (v.is_null())
        return "—";
    if (v.is_number_float())
        return QString::number(v.get<double>(), 'g', 7);
    return QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}


Json optionalId(const QLineEdit *input) {
    const auto text = input->text().trimmed();
    if (text.isEmpty())
        return nullptr;
    bool ok{};
    const auto value = text.toULongLong(&ok);
    if (!ok || text.startsWith('-') || text.startsWith('+'))
        throw std::runtime_error("Invalid unsigned identifier");
    return uint64_t(value);
}
} // namespace
PixelHistoryView::PixelHistoryView(QWidget *parent) : QWidget(parent) {
    setObjectName("pixelHistoryView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto tools = new QToolBar;
    read_ = tools->addAction("Read");
    read_->setObjectName("readPixelHistory");
    cancel_ = tools->addAction("Cancel");
    cancel_->setObjectName("cancelPixelHistory");
    locate_ = tools->addAction("Locate API");
    locate_->setObjectName("locateHistoryEvent");
    export_ = tools->addAction("Export…");
    export_->setObjectName("exportPixelHistory");
    auto details = tools->addAction("Details");
    details->setCheckable(true);
    details->setObjectName("historyDetailsToggle");
    tools->addSeparator();
    nativeMode_ = tools->addAction("Event storage");
    nativeMode_->setObjectName("historyNativeEvents");
    nativeMode_->setCheckable(true);
    nativeMode_->setChecked(false);
    nativeMode_->setToolTip("Read GPA storage before and after resource operations across the full frame. "
                            "These are event snapshots, not individual fragments.");
    auto advanced = tools->addAction("Coordinates");
    advanced->setCheckable(true);
    advanced->setObjectName("historyCoordinatesToggle");
    layout->addWidget(tools);
    auto inputs = new QToolBar;
    tools->removeAction(read_);
    inputs->addAction(read_);
    auto idInput = [&](const QString &label, const char *name, const QString &placeholder) {
        inputs->addWidget(new QLabel(label));
        auto value = new QLineEdit;
        value->setObjectName(name);
        value->setPlaceholderText(placeholder);
        value->setMaximumWidth(110);
        inputs->addWidget(value);
        connect(value, &QLineEdit::textChanged, this, &PixelHistoryView::invalidate);
        return value;
    };
    eventInput_ = idInput(" API ", "historyEvent", "Last draw");
    resource_ = idInput(" Resource ", "historyResource", "Target 0");
    auto spin = [&](const QString &label, const char *name, int limit) {
        inputs->addWidget(new QLabel(label));
        auto value = new QSpinBox;
        value->setObjectName(name);
        value->setRange(0, limit);
        value->setMaximumWidth(85);
        inputs->addWidget(value);
        connect(value, &QSpinBox::valueChanged, this, &PixelHistoryView::invalidate);
        return value;
    };
    x_ = spin(" X ", "historyX", INT_MAX);
    y_ = spin(" Y ", "historyY", INT_MAX);
    mip_ = spin(" Mip ", "historyMip", 31);
    layer_ = spin(" Layer ", "historyLayer", INT_MAX);
    sample_ = spin(" Sample ", "historySample", 31);
    slice_ = spin(" Slice ", "historySlice", INT_MAX);
    typedFormat_ = idInput(" Format ", "historyTypedFormat", "Automatic");
    typedFormat_->setToolTip("Optional DXGI format number for native storage interpretation");
    sample_->setToolTip("Individual MSAA sample; sample 0 for single-sampled textures");
    layout->addWidget(inputs);
    inputs->hide();
    connect(advanced, &QAction::toggled, inputs, &QWidget::setVisible);
    summary_ = new QLabel("No history");
    summary_->setObjectName("historySummary");
    summary_->setMargin(6);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    values_ = new QLabel;
    values_->setObjectName("historySnapshotValues");
    values_->setMargin(6);
    values_->setWordWrap(true);
    values_->setTextFormat(Qt::PlainText);
    values_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(values_);
    auto split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    records_ = new QTreeWidget;
    records_->setObjectName("historyRecords");
    records_->setHeaderLabels({"GPA", "Status", "Count", "Operation", "Before", "After"});
    records_->setRootIsDecorated(false);
    records_->setUniformRowHeights(true);
    records_->setAlternatingRowColors(true);
    const int widths[] = {65, 65, 65, 185, 150, 150};
    for (int i = 0; i < 6; ++i)
        records_->setColumnWidth(i, widths[i]);
    for (int column : {1, 2, 4, 5})
        records_->setColumnHidden(column, true);
    records_->header()->setStretchLastSection(false);
    records_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setObjectName("historyDetails");
    split->addWidget(records_);
    split->addWidget(details_);
    details_->hide();
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    layout->addWidget(split);
    connect(details, &QAction::toggled, this, [this, split](bool visible) {
        details_->setVisible(visible);
        if (visible)
            split->setSizes({split->height() * 3 / 4, split->height() / 4});
    });
    connect(records_, &QTreeWidget::itemSelectionChanged, this, &PixelHistoryView::selected);
    connect(records_, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (locate_->isEnabled())
            locate_->trigger();
    });
    connect(locate_, &QAction::triggered, this, [this] {
        auto item = records_->currentItem();
        if (item && !busy_ && item->data(0, Qt::UserRole).isValid())
            emit eventRequested(item->data(0, Qt::UserRole).toULongLong());
    });
    connect(cancel_, &QAction::triggered, this, &PixelHistoryView::cancelRequested);
    connect(read_, &QAction::triggered, this, [this] {
        if (busy_ || key_.isEmpty())
            return;
        try {
            request();
            invalidate();
            summary_->setText("Reading…");
            emit readRequested();
        } catch (const std::exception &e) {
            summary_->setText(QString::fromUtf8(e.what()));
        }
    });
    connect(export_, &QAction::triggered, this, [this] {
        const auto path =
            QFileDialog::getSaveFileName(this, "Export Pixel History", "history.json", "JSON (*.json)");
        if (path.isEmpty())
            return;
        try {
            exportResult(path);
        } catch (const std::exception &e) {
            summary_->setText(QString::fromUtf8(e.what()));
        }
    });
    connect(nativeMode_, &QAction::toggled, this, &PixelHistoryView::invalidate);
    updateActions();
}
void PixelHistoryView::setNativeMode(bool enabled) { nativeMode_->setChecked(enabled); }
bool PixelHistoryView::nativeMode() const { return nativeMode_->isChecked(); }
void PixelHistoryView::setContext(const QString &key, qulonglong event) {
    if (key_ != key) {
        key_ = key;
        invalidate();
    }
    if (event_ != event) {
        if (busy_)
            invalidate();
        event_ = event;
        ++requestId_;
        QSignalBlocker blocker(eventInput_);
        eventInput_->setText(event ? QString::number(event) : QString{});
    }
    updateActions();
}

void PixelHistoryView::selectPixel(qulonglong resource, int x, int y, int mip, int layer, int sample,
                                   int slice) {
    if (busy_)
        return;
    resource_->setText(QString::number(resource));
    x_->setValue(x);
    y_->setValue(y);
    mip_->setValue(mip);
    layer_->setValue(layer);
    sample_->setValue(sample);
    slice_->setValue(slice);
}
Json PixelHistoryView::request() const {
    const auto resource = optionalId(resource_);
    if (resource.is_null() || resource == 0)
        throw std::runtime_error("Select a GPA resource for history");
    Json result{{"action", "history"}, {"resource", resource},
                {"x", x_->value()}, {"y", y_->value()}, {"mip", mip_->value()},
                {"layer", layer_->value()}, {"sample", sample_->value()}, {"slice", slice_->value()},
                {"history_scope", nativeMode() ? "events" : "gpa"}};
    const auto format = optionalId(typedFormat_);
    if (!format.is_null()) {
        if (format == 0 || format.get<uint64_t>() > UINT32_MAX)
            throw std::runtime_error("Format must be a nonzero unsigned 32-bit DXGI identifier");
        result["typed_format"] = format;
    }
    return result;
}
void PixelHistoryView::setNotice(const QString &notice) {
    summary_->setText(notice);
    summary_->setToolTip(notice);
}
void PixelHistoryView::queryPixel(qulonglong event, qulonglong resource, int x, int y, int mip, int layer,
                                  int sample, int slice) {
    if (busy_)
        return;
    eventInput_->setText(event ? QString::number(event) : QString());
    selectPixel(resource, x, y, mip, layer, sample, slice);
    read_->trigger();
}
void PixelHistoryView::invalidate() {
    ++requestId_;
    result_ = nullptr;
    records_->clear();
    details_->clear();
    values_->clear();
    summary_->setText("No history");
    summary_->setToolTip({});
    updateActions();
}
void PixelHistoryView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void PixelHistoryView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty());
    cancel_->setEnabled(busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    nativeMode_->setEnabled(!busy_);
    for (auto widget :
         std::initializer_list<QWidget *>{eventInput_, resource_, x_, y_, mip_, layer_, sample_})
        widget->setEnabled(!busy_);
    eventInput_->setEnabled(false);
    eventInput_->setToolTip("History covers the full frame");
    slice_->setEnabled(!busy_ && nativeMode());
    typedFormat_->setEnabled(!busy_ && nativeMode());
    records_->setHeaderLabels(nativeMode()
                                  ? QStringList{"GPA", "Boundary", "Record", "Operation", "Before", "After"}
                                  : QStringList{"GPA", "Status", "Count", "Diagnostic", "Before", "Pixel"});
    records_->setColumnHidden(1, nativeMode());
    records_->setColumnHidden(2, nativeMode());
    records_->setColumnHidden(5, nativeMode());
    values_->setVisible(nativeMode() && records_->currentItem());
    const auto item = records_->currentItem();
    locate_->setEnabled(!busy_ && item && item->data(0, Qt::UserRole).isValid());
}
void PixelHistoryView::selected() {
    const auto item = records_->currentItem();
    if (!item || result_.is_null()) {
        details_->clear();
        values_->clear();
        updateActions();
        return;
    }
    const auto index = item->data(0, Qt::UserRole + 1).toULongLong();
    const auto &rows = result_.at("data").at("records");
    details_->setPlainText(QString::fromStdString(rows.at(size_t(index)).dump(2)));
    if (nativeMode())
        values_->setText(
            QString("API %1\nBefore: %2\nAfter: %3").arg(item->text(0), item->text(4), item->text(5)));
    updateActions();
}

bool PixelHistoryView::finishNative(uint64_t serial, const Json &result) {
    if (serial != requestId_)
        return false;
    if (!result.value("ok", false)) {
        setNotice(QString::fromStdString(result.value("error", "Event storage history failed")));
        emit inspectionFinished(false);
        return false;
    }
    const auto &data = result.at("data");
    if (!nativeMode()) {
        if (result.at("context").at("action") != "history" ||
            result.at("context").at("options").at("history_scope") != "gpa" ||
            data.at("kind") != "gpa_event_history" || data.at("schema") != 1)
            throw std::runtime_error("Unexpected GPA event diagnostic contract");
        const auto options = request();
        for (const auto field : {"resource", "x", "y", "mip", "layer", "slice", "sample"})
            if (data.at("selection").at(field) != options.at(field))
                throw std::runtime_error("GPA history selection changed");
        std::vector<QStringList> rows;
        for (const auto &record : data.at("records")) {
            if (record.at("record_kind") != "gpa_event_diagnostic")
                throw std::runtime_error("Unexpected GPA history record");
            const auto status = record.at("diagnostic_status").get<uint32_t>();
            const auto label = status == 0 ? "Passed diagnostic" : status == 2 ? "Depth diagnostic" :
                               status == 3 ? "Stencil diagnostic" : status == 5 ? "Discard diagnostic" : "Unknown status";
            rows.push_back({scalar(record.at("event")), QString::number(status),
                            scalar(record.at("diagnostic_count")), label, {}, scalar(record.at("pixel").value("bytes", Json(nullptr)))});
        }
        result_ = result;
        records_->clear();
        size_t index = 0;
        for (const auto &row : rows) {
            auto *item = new QTreeWidgetItem(records_, row);
            item->setData(0, Qt::UserRole, row[0].toULongLong());
            item->setData(0, Qt::UserRole + 1, qulonglong(index++));
        }
        setNotice(QString("%1 GPA event diagnostics · counterfactual scissor/sample-mask conditions; counts do not prove actual writes").arg(rows.size()));
        updateActions();
        emit inspectionFinished(true);
        return true;
    }

    if (result.at("context").at("action") != "history" ||
        result.at("context").at("options").at("history_scope") != "events" || data.at("schema") != 2 ||
        data.at("kind") != "pixel_event_history" || data.at("source") != "native_dx11_replay" ||
        data.at("fragment_history_available") != false)
        throw std::runtime_error("Unexpected native event history contract");
    const auto options = request();
    const auto &selection = data.at("selection");
    for (const auto field : {"resource", "x", "y", "mip", "layer", "slice", "sample"})
        if (selection.at(field) != options.at(field))
            throw std::runtime_error("Native history selection changed");
    if (selection.at("typed_format") != options.value("typed_format", Json(nullptr)))
        throw std::runtime_error("Native history format changed");
    auto snapshot = [](const Json &value) {
        if (!value.at("available").get<bool>())
            return QString("Unavailable: ") + scalar(value.at("reason"));
        QStringList parts;
        const auto &color = value.at("decoded_color");
        if (!color.is_null()) {
            QStringList channels;
            for (const auto &channel : color.at("rgba"))
                channels << scalar(channel);
            parts << channels.join(", ");
        }
        if (!value.at("depth").is_null())
            parts << "Depth: " + scalar(value.at("depth"));
        if (!value.at("stencil").is_null())
            parts << "Stencil: " + scalar(value.at("stencil"));
        if (parts.isEmpty())
            parts << "Bytes: " + scalar(value.at("bytes"));
        return parts.join(" · ");
    };
    struct Row {
        uint64_t event;
        QStringList text;
    };
    std::vector<Row> rows;
    if (!data.at("records").is_array())
        throw std::runtime_error("Native history records must be an array");
    for (const auto &record : data.at("records")) {
        if (record.at("record_kind") != "event_snapshot" || !record.at("fragment").is_null() ||
            !record.at("shader_output").is_null() || !record.at("failure_reasons").is_null())
            throw std::runtime_error("Event storage history cannot claim fragment observations");
        const auto &event = record.at("event");
        if ((!event.is_number_unsigned() && !event.is_number_integer()) || event == 0 ||
            (event.is_number_integer() && !event.is_number_unsigned() && event.get<int64_t>() < 0))
            throw std::runtime_error("Invalid native history event identity");
        const auto &footprint = record.at("footprint");
        const auto &executed = record.at("write_executed");
        if (!executed.is_null() && !executed.is_boolean())
            throw std::runtime_error("Invalid native history write validity");
        const auto operation = scalar(record.at("api")) + " · " + scalar(footprint.at("coverage")) +
                               (executed.is_null() ? " · execution unknown"
                                : executed == true ? " · write executed"
                                                   : " · no write");
        rows.push_back({event.get<uint64_t>(),
                        {scalar(event), scalar(record.at("boundary")), "Event snapshot", operation,
                         snapshot(record.at("before")), snapshot(record.at("after"))}});
    }
    const auto summary = QString("%1 event snapshots · full frame · (%2, %3)\n"
                                 "T:%4 · mip %5 / layer %6 / slice %7 / sample %8 · fragment details unknown")
                             .arg(rows.size())
                             .arg(x_->value())
                             .arg(y_->value())
                             .arg(resource_->text())
                             .arg(mip_->value())
                             .arg(layer_->value())
                             .arg(slice_->value())
                             .arg(sample_->value());
    auto overview = data;
    overview.erase("records");
    const auto details = QString::fromStdString(overview.dump(2));
    // Commit only after every row and selection field has been validated.
    result_ = result;
    records_->clear();
    size_t index = 0;
    for (const auto &row : rows) {
        auto *item = new QTreeWidgetItem(records_, row.text);
        item->setData(0, Qt::UserRole, qulonglong(row.event));
        item->setData(0, Qt::UserRole + 1, qulonglong(index++));
        for (int column = 0; column < 6; ++column)
            item->setToolTip(column, item->text(column));
    }
    setNotice(summary);
    summary_->setToolTip(details);
    details_->setPlainText(details);
    if (!rows.empty())
        records_->setCurrentItem(records_->topLevelItem(0));
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void PixelHistoryView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No history to export");
    QSaveFile file(path);
    const auto bytes = result_.dump(2);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot export pixel history");
}
} // namespace flora
