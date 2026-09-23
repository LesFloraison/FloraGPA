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
bool historyBackendCompatible(const QString &path) {
    const auto name = path.toStdWString();
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(name.c_str(), &ignored);
    if (!size) return false;
    std::vector<uint8_t> bytes(size);
    if (!GetFileVersionInfoW(name.c_str(), 0, size, bytes.data())) return false;
    VS_FIXEDFILEINFO *info = nullptr; UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void **>(&info), &length) || length < sizeof(*info)) return false;
    return HIWORD(info->dwFileVersionMS) == 1 && LOWORD(info->dwFileVersionMS) == 45;
}

namespace {
QString scalar(const Json &v) {
    if (v.is_null())
        return "—";
    if (v.is_number_float())
        return QString::number(v.get<double>(), 'g', 7);
    return QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
QString color(const Json &row, const char *key) {
    const auto kind = row.value("color_interpretation", std::string("float"));
    const auto field = kind == "uint" ? "uintValue" : kind == "sint" ? "intValue" : "floatValue";
    QStringList parts;
    for (const auto &v : row.at(key).at("col").at(field))
        parts << scalar(v);
    return parts.join(", ");
}
QString outcome(const Json &r) {
    if (r.value("record_kind", "") == "cpu_write_snapshot")
        return "CPU snapshot";
    if (r.value("gpa_command_kind", Json(nullptr)) == "api")
        return "Resource operation";
    QStringList failed;
    for (const auto &[key, label] :
         std::initializer_list<std::pair<const char *, const char *>>{{"backfaceCulled", "Backface"},
                                                                      {"depthClipped", "Depth clip"},
                                                                      {"depthTestFailed", "Depth test"},
                                                                      {"stencilTestFailed", "Stencil test"},
                                                                      {"scissorClipped", "Scissor"},
                                                                      {"shaderDiscarded", "Discard"},
                                                                      {"sampleMasked", "Sample mask"},
                                                                      {"predicationSkipped", "Predication"},
                                                                      {"viewClipped", "Viewport"},
                                                                      {"depthBoundsFailed", "Depth bounds"}})
        if (r.value(key, false))
            failed << label;
    return failed.isEmpty() ? "Passed / write" : failed.join(", ");
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
    backend_ = tools->addAction("RenderDoc…");
    backend_->setObjectName("historyBackend");
    auto advanced = tools->addAction("Coordinates");
    advanced->setCheckable(true);
    advanced->setObjectName("historyCoordinatesToggle");
    layout->addWidget(tools);
    auto inputs = new QToolBar;
    tools->removeAction(read_);
    inputs->addAction(read_);
    tools->insertAction(cancel_, backend_);
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
    auto split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    records_ = new QTreeWidget;
    records_->setObjectName("historyRecords");
    records_->setHeaderLabels({"GPA", "Replay", "Fragment", "Operation", "Before", "After"});
    records_->setRootIsDecorated(false);
    records_->setUniformRowHeights(true);
    records_->setAlternatingRowColors(true);
    const int widths[] = {65, 65, 65, 185, 150, 150};
    for (int i = 0; i < 6; ++i)
        records_->setColumnWidth(i, widths[i]);
    for (int column : {1, 2, 4, 5}) records_->setColumnHidden(column, true);
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
    connect(backend_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", backendPath_,
                                                       "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    setBackendPath(
        QSettings()
            .value("analysis/renderdoc", QDir(qEnvironmentVariable("ProgramFiles", "C:/Program Files"))
                                             .filePath("RenderDoc/renderdoc.dll"))
            .toString());
    updateActions();
}
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
void PixelHistoryView::setBackendPath(const QString &path) {
    if (backendPath_ == path)
        return;
    backendPath_ = path;
    QSettings().setValue("analysis/renderdoc", path);
    backend_->setToolTip(path);
    invalidate();
}
void PixelHistoryView::selectPixel(qulonglong resource, int x, int y, int mip, int layer, int sample) {
    if (busy_)
        return;
    resource_->setText(QString::number(resource));
    x_->setValue(x);
    y_->setValue(y);
    mip_->setValue(mip);
    layer_->setValue(layer);
    sample_->setValue(sample);
}
Json PixelHistoryView::request() const {
    return {{"action", "history"},
            {"gpa_event", optionalId(eventInput_)},
            {"resource", optionalId(resource_)},
            {"x", x_->value()},
            {"y", y_->value()},
            {"mip", mip_->value()},
            {"layer", layer_->value()},
            {"sample", sample_->value()}};
}
void PixelHistoryView::setNotice(const QString &notice) {
    summary_->setText(notice);
    summary_->setToolTip(notice);
}
void PixelHistoryView::queryPixel(qulonglong event, qulonglong resource, int x, int y, int mip, int layer, int sample) {
    if (busy_) return;
    eventInput_->setText(event ? QString::number(event) : QString());
    selectPixel(resource, x, y, mip, layer, sample);
    read_->trigger();
}
void PixelHistoryView::invalidate() {
    ++requestId_;
    result_ = nullptr;
    records_->clear();
    details_->clear();
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
    backend_->setEnabled(!busy_);
    for (auto widget :
         std::initializer_list<QWidget *>{eventInput_, resource_, x_, y_, mip_, layer_, sample_})
        widget->setEnabled(!busy_);
    const auto item = records_->currentItem();
    locate_->setEnabled(!busy_ && item && item->data(0, Qt::UserRole).isValid());
}
void PixelHistoryView::selected() {
    const auto item = records_->currentItem();
    if (!item || result_.is_null()) {
        details_->clear();
        updateActions();
        return;
    }
    const auto index = item->data(0, Qt::UserRole + 1).toULongLong();
    details_->setPlainText(QString::fromStdString(result_.at("history").at(size_t(index)).dump(2)));
    updateActions();
}
bool PixelHistoryView::finish(uint64_t request, const Json &result) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(QString::fromStdString(result.value("error", "History failed")));
        emit inspectionFinished(false);
        return false;
    }
    // Validate every displayed field before mutating the current table.
    for (const auto &r : result.at("history")) {
        color(r, "preMod");
        color(r, "postMod");
        outcome(r);
    }
    result_ = result;
    records_->clear();
    size_t index = 0;
    for (const auto &r : result.at("history")) {
        const auto command =
            r.at("gpa_command").is_null() ? QString("Unmapped") : scalar(r.at("gpa_command"));
        auto item = new QTreeWidgetItem(records_, {scalar(r.at("gpa_event")), scalar(r.at("eventId")),
                                                   scalar(r.at("fragIndex")), command + " · " + outcome(r),
                                                   color(r, "preMod"), color(r, "postMod")});
        if (!r.at("gpa_event").is_null())
            item->setData(0, Qt::UserRole, qulonglong(r.at("gpa_event").get<uint64_t>()));
        item->setData(0, Qt::UserRole + 1, qulonglong(index++));
        for (int c = 0; c < 6; ++c)
            item->setToolTip(c, item->text(c));
    }
    const auto &scope = result.at("history_scope");
    summary_->setText(QString("%1 records · %2 CPU · %3 gaps · (%4, %5)")
                          .arg(index)
                          .arg(scope.at("cpu_write_snapshots").get<size_t>())
                          .arg(scope.at("cpu_write_gaps").size())
                          .arg(result.at("x").get<uint32_t>())
                          .arg(result.at("y").get<uint32_t>()));
    summary_->setText(summary_->text() + QString("\nT:%1 · through API %2 · mip %3 / layer %4 / sample %5")
        .arg(resource_->text(), eventInput_->text()).arg(mip_->value()).arg(layer_->value()).arg(sample_->value()));
    summary_->setToolTip(QString::fromStdString(scope.dump(2)));
    details_->setPlainText(QString::fromStdString(scope.dump(2)));
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
