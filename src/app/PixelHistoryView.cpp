#include "PixelHistoryView.h"
#include "application/RenderDocLibrary.h"
#include "application/RdcEvents.h"
#include <QAction>
#include <QCheckBox>
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
#include <charconv>

namespace flora {
using Json = nlohmann::json;
bool historyBackendCompatible(const QString &path) {
    const auto name = path.toStdWString();
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(name.c_str(), &ignored);
    if (!size)
        return false;
    std::vector<uint8_t> bytes(size);
    if (!GetFileVersionInfoW(name.c_str(), 0, size, bytes.data()))
        return false;
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void **>(&info), &length) ||
        length < sizeof(*info))
        return false;
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
    if (row.value("record_kind", "") == "observed_event_depth_boundary") {
        const auto &value = row.at(std::string(key) == "preMod" ? "pre" : "post");
        return scalar(value.at("depth")) + " (raw " + scalar(value.at("raw_unorm16")) + ')';
    }
    if (row.value("depth_value_status", "") == "unavailable_backend_d16_format")
        return "Fragment depth unavailable";
    if (row.value("pixel_value_kind", "") == "depth_stencil") {
        auto value = "Depth " + scalar(row.at(key).at("depth"));
        if (row.contains("depth_value_sources") &&
            row.at("depth_value_sources").value(key, "") == "backend_d32_intermediate")
            value += " (D32 intermediate)";
        const auto &stencil = row.at(key).at("stencil");
        if (row.value("stencil_present", false) && stencil.is_number_integer() && stencil.get<int>() >= 0)
            value += " / Stencil " + scalar(stencil);
        return value;
    }
    const auto kind = row.value("color_interpretation", std::string("float"));
    const auto field = kind == "uint" ? "uintValue" : kind == "sint" ? "intValue" : "floatValue";
    QStringList parts;
    for (const auto &v : row.at(key).at("col").at(field))
        parts << scalar(v);
    return parts.join(", ");
}
QString outcome(const Json &r) {
    if (r.value("record_kind", "") == "resource_operation") {
        const auto coverage = r.value("coverage", Json::object());
        if (coverage.value("status", "") == "unavailable") return "Resource operation · coverage unavailable";
        return r.value("pixel_value_changed", false) ? "Resource operation · selected value changed"
                                                    : "Resource operation · selected value unchanged";
    }
    if (r.value("record_kind", "") == "observed_event_depth_boundary")
        return "Observed event boundary (not a fragment)";
    if (r.value("depth_test_reason_status", "") == "unverified_backend_d16_format")
        return "Depth-test result unverified";
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
uint64_t executionId(const Json &value) {
    if (!value.is_string())
        throw std::runtime_error("DX12 execution identity must be a decimal string");
    const auto &text = value.get_ref<const std::string &>();
    uint64_t id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (!id || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error("Invalid DX12 execution identity");
    return id;
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
    debug_ = tools->addAction("Debug PS");
    debug_->setObjectName("debugHistoryPixel");
    debug_->setToolTip("Select this draw, pixel, sample and primitive for PS debugging. "
                       "Fragment index and instance selection are unavailable.");
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
    depthRoundtrip_ = new QCheckBox("D32/D32S8 depth refresh (experimental)");
    depthRoundtrip_->setObjectName("historyDepthRoundtrip");
    depthRoundtrip_->setToolTip("Requires the patched RenderDoc library. Refreshes depth for the selected isolated "
                                "Draw only; other history events and intermediate fragments remain unverified. "
                                "Requires read-only D32 direct Draw (patch v2) or depth-writing D32S8; stencil disabled, one mip/layer/sample/node and legacy depth states.");
    layout->addWidget(depthRoundtrip_);
    connect(depthRoundtrip_, &QCheckBox::toggled, this, &PixelHistoryView::invalidate);
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
    eventInput_ = idInput(" Event ", "historyEvent", "Last draw");
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
            emit eventRequested(item->data(0, Qt::UserRole).toULongLong(),
                dx12_ ? indirectActionOrdinal(rows_.at(item->data(0, Qt::UserRole + 1).toULongLong())) : -1);
    });
    connect(cancel_, &QAction::triggered, this, &PixelHistoryView::cancelRequested);
    connect(debug_, &QAction::triggered, this, [this] {
        if (!debug_->isEnabled())
            return;
        const auto item = records_->currentItem();
        const auto &row = rows_.at(item->data(0, Qt::UserRole + 1).toULongLong());
        emit pixelDebugRequested(item->data(0, Qt::UserRole).toULongLong(), x_->value(), y_->value(),
                                 sample_->value(), row.at("primitiveID").get<uint32_t>(), indirectActionOrdinal(row),
                                 dx12_ && depthRoundtrip_->isChecked() &&
                                     item->data(0, Qt::UserRole).toULongLong() == event_);
    });
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
    setBackendPath(configuredRenderDocLibrary(), false);
    updateActions();
}
void PixelHistoryView::setContext(const QString &key, qulonglong event, bool dx12) {
    if (dx12_ != dx12 || key_ != key || event_ != event)
        depthRoundtrip_->setChecked(false);
    if (dx12_ != dx12) {
        dx12_ = dx12;
        creation_ = object_ = 0;
        resource_->clear();
        invalidate();
    }
    records_->headerItem()->setText(0, dx12 ? "Execution" : "GPA");
    locate_->setText(dx12 ? "Locate execution" : "Locate API");
    records_->headerItem()->setText(2, dx12 ? "Fragment / primitive" : "Fragment");
    records_->setColumnWidth(2, dx12 ? 145 : 65);
    for (int column : {2, 4, 5})
        records_->setColumnHidden(column, !dx12);
    for (int column : {4, 5}) records_->setColumnWidth(column, dx12 ? 245 : 150);
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
void PixelHistoryView::setDx12Resource(qulonglong creation, qulonglong object) {
    if (!dx12_ || (creation_ == creation && object_ == object))
        return;
    creation_ = creation;
    object_ = object;
    resource_->setText(object ? QString::number(object) : QString());
    invalidate();
}
void PixelHistoryView::setBackendPath(const QString &path, bool persist) {
    if (backendPath_ == path)
        return;
    backendPath_ = path;
    if (persist)
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
    Json job{{"action", "history"},
             {"gpa_event", optionalId(eventInput_)},
             {"resource", optionalId(resource_)},
             {"x", x_->value()},
             {"y", y_->value()},
             {"mip", mip_->value()},
             {"layer", layer_->value()},
             {"sample", sample_->value()}};
    if (dx12_) {
        if (key_.isEmpty() || !event_ || !object_ || !creation_)
            throw std::runtime_error("Select a submitted execution and a resource creation");
        job.erase("gpa_event");
        job["execution"] = std::to_string(event_);
        job["resource"] = std::to_string(object_);
        job["resource_creation"] = std::to_string(creation_);
        if (depthRoundtrip_->isChecked()) job["depth_roundtrip"] = true;
    }
    return job;
}
void PixelHistoryView::setNotice(const QString &notice) {
    summary_->setText(notice);
    summary_->setToolTip(notice);
}
void PixelHistoryView::queryPixel(qulonglong event, qulonglong resource, int x, int y, int mip, int layer,
                                  int sample) {
    if (busy_)
        return;
    eventInput_->setText(event ? QString::number(event) : QString());
    selectPixel(resource, x, y, mip, layer, sample);
    read_->trigger();
}
void PixelHistoryView::invalidate() {
    ++requestId_;
    result_ = nullptr;
    rows_ = nullptr;
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
    depthRoundtrip_->setVisible(dx12_);
    depthRoundtrip_->setEnabled(!busy_);
    for (auto widget :
         std::initializer_list<QWidget *>{eventInput_, resource_, x_, y_, mip_, layer_, sample_})
        widget->setEnabled(!busy_);
    if (dx12_) {
        eventInput_->setEnabled(false);
        resource_->setEnabled(false);
    }
    const auto item = records_->currentItem();
    locate_->setEnabled(!busy_ && item && item->data(0, Qt::UserRole).isValid());
    bool debuggable = false;
    if (locate_->isEnabled() && rows_.is_array() &&
        item->data(0, Qt::UserRole + 1).toULongLong() < rows_.size()) {
        const auto &row = rows_.at(item->data(0, Qt::UserRole + 1).toULongLong());
        const auto primitive = row.value("primitiveID", Json());
        // Resource operations and synthetic boundaries are not shader invocations.
        debuggable = dx12_ && row.value("record_kind", "") == "backend_modification" &&
                     (row.value("name", std::string()).find("Draw") != std::string::npos ||
                      row.value("indirect_action_kind", "") == "draw") &&
                     !row.value("directShaderWrite", false) && !row.value("unboundPS", false) &&
                     primitive.is_number_unsigned() && primitive.get<uint64_t>() < UINT32_MAX;
    }
    debug_->setEnabled(debuggable);
}
void PixelHistoryView::selected() {
    const auto item = records_->currentItem();
    if (!item || result_.is_null()) {
        details_->clear();
        updateActions();
        return;
    }
    const auto index = item->data(0, Qt::UserRole + 1).toULongLong();
    details_->setPlainText(QString::fromStdString(rows_.at(size_t(index)).dump(2)));
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
    if (dx12_ && (result.value("backend", std::string()) != "dx12" ||
                  result.value("execution", Json()) != Json(std::to_string(event_))))
        throw std::runtime_error("History result does not match the DX12 execution selection");
    if (dx12_ && result.value("depth_roundtrip", Json::object()).value("requested", false) != depthRoundtrip_->isChecked())
        throw std::runtime_error("History result does not match the D32/D32S8 depth refresh mode");
    auto rows = result.at("history");
    if (dx12_)
        for (const auto &boundary : result.value("depth_event_boundaries", Json::array()))
            rows.push_back(boundary);
    // Validate every displayed field before mutating the current table.
    for (const auto &r : rows) {
        color(r, "preMod");
        color(r, "postMod");
        outcome(r);
        if (dx12_ && r.value("selectable", false)) {
            (void)executionId(r.at("execution"));
            (void)indirectActionOrdinal(r);
        }
    }
    result_ = result;
    rows_ = std::move(rows);
    records_->clear();
    size_t index = 0;
    for (const auto &r : rows_) {
        auto command =
            dx12_ ? scalar(r.value("name", Json("Unmapped")))
                  : (r.at("gpa_command").is_null() ? QString("Unmapped") : scalar(r.at("gpa_command")));
        if (dx12_ && r.contains("indirect_action")) command += " · action " + scalar(r.at("indirect_action"));
        auto item = new QTreeWidgetItem(
            records_, {scalar(r.value(dx12_ ? "execution" : "gpa_event", Json())), scalar(r.at("eventId")),
                       scalar(r.value("fragIndex", Json())) + (dx12_ ? " / " + scalar(r.value("primitiveID", Json())) : QString()), command + " · " + outcome(r), color(r, "preMod"),
                       color(r, "postMod")});
        if (dx12_ && r.value("selectable", false))
            item->setData(0, Qt::UserRole, qulonglong(executionId(r.at("execution"))));
        else if (!dx12_ && !r.at("gpa_event").is_null())
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
    if (dx12_)
        summary_->setText(
            QString("%1 fragment/operation records · %2 observed depth boundaries · %3 unmapped · (%4, %5)")
                .arg(result.at("history").size())
                .arg(rows_.size() - result.at("history").size())
                .arg(scope.at("unmapped_records").get<size_t>())
                .arg(result.at("x").get<uint32_t>())
                .arg(result.at("y").get<uint32_t>()));
    summary_->setText(summary_->text() +
                      QString(dx12_ ? "\nObject %1 · through execution %2 · mip %3 / layer %4 / sample %5"
                                    : "\nT:%1 · through API %2 · mip %3 / layer %4 / sample %5")
                          .arg(resource_->text(), eventInput_->text())
                          .arg(mip_->value())
                          .arg(layer_->value())
                          .arg(sample_->value()));
    if (result.value("partial", false))
        summary_->setText("Partial history · " + summary_->text());
    const auto excluded = scope.value("excluded_resource_operations", Json::array()).size();
    const auto coverageGaps = scope.value("resource_operation_coverage_gaps", Json::array()).size();
    if (excluded || coverageGaps)
        summary_->setText(summary_->text() + QString("\n%1 resource operations excluded by captured coverage · %2 coverage gaps")
            .arg(excluded).arg(coverageGaps));
    if (const auto refresh = result.value("depth_roundtrip", Json()); refresh.is_object())
        summary_->setText(summary_->text() +
            QString("\nExperimental D32/D32S8 depth refresh: %1 completed depth refreshes for the selected event. "
                    "Other events and intermediate fragments are not validated.").arg(scalar(refresh.value("copies", Json()))));
    const auto consistency = scope.value("replay_consistency", Json::object());
    const auto consistencyStatus = consistency.value("status", "not_measured");
    if (consistencyStatus == "selected_texel_changed")
        summary_->setText(summary_->text() +
            "\nSelected texel changed during history replays. These records do not explain one consistent replay state.");
    else if (consistencyStatus == "selected_texel_unchanged")
        summary_->setText(summary_->text() +
            "\nSelected texel unchanged before/after history; intermediate fragments and history completeness are not validated.");
    else
        summary_->setText(summary_->text() + "\nReplay consistency not measured or unavailable.");
    const auto boundary = consistency.value("history_boundary", Json::object());
    if (boundary.value("status", "") == "differs_from_selected_texel")
        summary_->setText(summary_->text() +
            "\nHistory post-event color differs from the selected texel. The reported history boundary is not validated.");
    if (scope.value("depth_value_status", "") == "event_boundaries_only")
        summary_->setText(
            summary_->text() +
            "\nD16: event-boundary depths only; fragment depths and depth-test reasons unavailable.");
    else if (scope.value("depth_value_status", "") == "patched_backend_values")
        summary_->setText(summary_->text() +
                          "\nD16: patched RenderDoc 1.45 decoding; backend test-reason limitations apply.");
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
