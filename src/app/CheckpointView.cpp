#include "CheckpointView.h"
#include "NativeDebugControls.h"
#include "application/CheckpointInspection.h"
#include "application/InvocationSelector.h"
#include "application/SdbgVariables.h"
#include "application/SourceStack.h"
#include "application/SourceVariables.h"
#include "application/ZipArchive.h"
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace flora {
namespace {
using Json = nlohmann::json;
QString text(const Json &value) {
    return value.is_null()
               ? QString()
               : QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
}
QByteArray readFile(const QString &path, qint64 limit = 256ll * 1024 * 1024) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        throw std::runtime_error("Cannot read bounded checkpoint artifact");
    const auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read checkpoint artifact");
    return bytes;
}
Json readJson(const QString &path) {
    const auto bytes = readFile(path);
    return Json::parse(bytes.constData(), bytes.constData() + bytes.size());
}
QTreeWidget *table(QWidget *parent, const char *name, const QStringList &columns) {
    auto tree = new QTreeWidget(parent);
    tree->setObjectName(name);
    tree->setHeaderLabels(columns);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(true);
    return tree;
}
} // namespace
struct CheckpointView::Data {
    Json result, rows;
    std::map<uint64_t, Json> entries;
    QString directory;
    QByteArray snapshots;
    std::unique_ptr<SdbgTraceValues> history;
    Json registers(size_t index) const {
        const auto &meta = result.at("register_capture"), &row = rows.at(index);
        return checkpointRegisters(
            Bytes(reinterpret_cast<const uint8_t *>(snapshots.constData()), size_t(snapshots.size())), meta,
            row.at("record"));
    }
    const Json &entry(size_t index) const {
        const auto &row = rows.at(index);
        const auto number = row.contains("instruction")
                                ? row.at("instruction")
                                : result.at("checkpoint_instruction").at("instruction");
        if (number.is_null())
            throw std::runtime_error("Snapshot has no original instruction");
        return entries.at(number.get<uint64_t>());
    }
    Json values(size_t index) {
        const auto &model = result.at("source_variables");
        if (model.value("format", "") == "SDBG assignments" && model.value("status", "") == "available" &&
            result.value("trace", false)) {
            if (!history)
                history = std::make_unique<SdbgTraceValues>(result, rows,
                                                            [this](size_t i) { return registers(i); });
            return history->at(index);
        }
        return resolveSourceVariables(model, registers(index), result.at("register_capture"), rows.at(index),
                                      entry(index).at("word_offset").get<uint64_t>() * 4);
    }
};
CheckpointView::~CheckpointView() = default;
CheckpointView::CheckpointView(QString stage, QWidget *parent) : QWidget(parent), stage_(std::move(stage)) {
    setObjectName("checkpoint-" + stage_);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    captureBar_ = new QToolBar;
    captureBar_->setObjectName("checkpointCaptureBar");
    layout->addWidget(captureBar_);
    auto action = [&](QToolBar *bar, const char *name, const char *label, const char *tip,
                      const std::function<void()> &fn) {
        auto a = bar->addAction(label);
        a->setObjectName(name);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, [this, fn] { invoke(fn); });
        return a;
    };
    action(captureBar_, "checkpointRead", "Read", "Read the selected draw's original shader",
           [this] { request(true, false); });
    action(captureBar_, "checkpointCapture", "Capture",
           "Capture values before the selected original instruction", [this] { request(false, false); });
    action(captureBar_, "checkpointTrace", "Trace", "Capture this invocation or selected HS phase",
           [this] { request(false, true); });
    action(captureBar_, "checkpointSelected", "Trace Input",
           "Re-execute using the selected snapshot's original input bits",
           [this] { request(false, true, true); });
    match_ = new QComboBox;
    match_->setObjectName("checkpointMatch");
    match_->addItem("Unique", "unique");
    match_->addItem("All matches", "all");
    captureBar_->addWidget(match_);
    captureBar_->addSeparator();
    instruction_ = new QLineEdit;
    instruction_->setObjectName("checkpointInstruction");
    instruction_->setPlaceholderText("Instruction");
    instruction_->setMaximumWidth(100);
    captureBar_->addWidget(instruction_);
    action(captureBar_, "checkpointExport", "Export…",
           "Save the complete checkpoint package with current debugger settings", [this] {
               if (!data_ || !data_->result.contains("register_capture"))
                   throw std::runtime_error("Capture checkpoints first");
               const auto path = QFileDialog::getSaveFileName(this, "Export checkpoint package", {},
                                                              "Checkpoint package (*.zip)");
               if (!path.isEmpty())
                   exportArchive(path);
           });
    stepBar_ = new QToolBar;
    stepBar_->setObjectName("checkpointStepBar");
    layout->addWidget(stepBar_);
    navigation_ = new QComboBox;
    navigation_->setObjectName("checkpointNavigation");
    navigation_->addItems({"Instruction", "Source"});
    stepBar_->addWidget(navigation_);
    action(stepBar_, "checkpointPrevious", "Previous", "Previous instruction or source position",
           [this] { move(-1); });
    action(stepBar_, "checkpointStep", "Step", "Step into the next instruction or source position",
           [this] { move(1); });
    action(stepBar_, "checkpointOver", "Over", "Step over the current call or source function",
           [this] { step(false); });
    action(stepBar_, "checkpointOut", "Out", "Step out of the current call or source function",
           [this] { step(true); });
    action(stepBar_, "checkpointRunTo", "Run to", "Continue to the selected original instruction",
           [this] { seek(true); });
    action(stepBar_, "checkpointBreakpoint", "Breakpoint", "Toggle an instruction or source-line breakpoint",
           [this] { toggleBreakpoint(); });
    action(stepBar_, "checkpointContinue", "Continue", "Continue to the next breakpoint in this invocation",
           [this] { seek(false); });
    summary_ = new QLabel(stage_.toUpper() + " · Select a draw");
    summary_->setObjectName("checkpointSummary");
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Horizontal);
    layout->addWidget(split, 1);
    codeTabs_ = new QTabWidget(split);
    codeTabs_->setObjectName("checkpointCodeTabs");
    codeTabs_->setDocumentMode(true);
    instructions_ = table(codeTabs_, "checkpointInstructions", {"#", "Stop", "Original assembly"});
    instructions_->setColumnWidth(0, 48);
    instructions_->setColumnWidth(1, 46);
    codeTabs_->addTab(instructions_, "Assembly");
    auto code = new QWidget;
    auto codeLayout = new QVBoxLayout(code);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    auto sourceBar = new QHBoxLayout;
    codeLayout->addLayout(sourceBar);
    files_ = new QComboBox;
    files_->setObjectName("checkpointSourceFile");
    files_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    sourceBar->addWidget(files_, 1);
    sourceLine_ = new QSpinBox;
    sourceLine_->setObjectName("checkpointSourceLine");
    sourceLine_->setRange(1, INT_MAX);
    sourceLine_->setPrefix("Line ");
    sourceLine_->setMaximumWidth(110);
    sourceBar->addWidget(sourceLine_);
    source_ = new QPlainTextEdit;
    source_->setObjectName("checkpointSource");
    source_->setReadOnly(true);
    source_->setLineWrapMode(QPlainTextEdit::NoWrap);
    source_->setFont(QFont("Cascadia Mono", 10));
    codeLayout->addWidget(source_, 1);
    codeTabs_->addTab(code, "Source");
    auto right = new QSplitter(Qt::Vertical, split);
    hits_ = table(right, "checkpointHits", {"Record", "Invocation", "Hit", "Instruction", "Identity"});
    for (int i = 0; i < 4; ++i)
        hits_->setColumnWidth(i, i == 1 ? 76 : i == 3 ? 86 : 65);
    valueTabs_ = new QTabWidget(right);
    valueTabs_->setObjectName("checkpointValueTabs");
    valueTabs_->setDocumentMode(true);
    auto registerPane = new QWidget;
    auto regLayout = new QVBoxLayout(registerPane);
    regLayout->setContentsMargins(0, 0, 0, 0);
    mode_ = new QComboBox;
    mode_->setObjectName("checkpointRegisterFormat");
    mode_->addItems({"Hex", "UInt", "Int", "Float"});
    regLayout->addWidget(mode_);
    registers_ = table(registerPane, "checkpointRegisters", {"Register", "x", "y", "z", "w", "Valid"});
    registers_->setColumnWidth(0, 95);
    for (int i = 1; i < 5; ++i)
        registers_->setColumnWidth(i, 95);
    regLayout->addWidget(registers_, 1);
    valueTabs_->addTab(registerPane, "Registers");
    auto variablePane = new QWidget;
    auto varLayout = new QVBoxLayout(variablePane);
    varLayout->setContentsMargins(0, 0, 0, 0);
    frames_ = new QComboBox;
    frames_->setObjectName("checkpointSourceFrame");
    varLayout->addWidget(frames_);
    variables_ = table(variablePane, "checkpointVariables", {"Variable", "Scope", "Type", "Value", "Status"});
    variables_->setColumnWidth(0, 170);
    variables_->setColumnWidth(1, 100);
    variables_->setColumnWidth(2, 65);
    variables_->setColumnWidth(3, 100);
    varLayout->addWidget(variables_, 1);
    valueTabs_->addTab(variablePane, "Variables");
    controls_ = new NativeDebugControls;
    valueTabs_->addTab(controls_, "Breakpoints / Watches");
    stack_ = new QLabel;
    stack_->setObjectName("checkpointStack");
    stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(stack_);
    split->setSizes({440, 620});
    right->setSizes({180, 340});
    connect(controls_, &NativeDebugControls::error, this, &CheckpointView::error);
    connect(controls_, &NativeDebugControls::rulesChanged, this, [this] {
        invoke([&] {
            updateBreakpoints();
            showSource();
        });
    });
    connect(controls_, &NativeDebugControls::sourcePointSelected, this,
            [this](qlonglong file, qlonglong line) {
                files_->setCurrentIndex(int(file));
                sourceLine_->setValue(int(line));
                codeTabs_->setCurrentIndex(1);
                showSource(true);
            });
    connect(instructions_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!rendering_ && instructions_->currentItem())
            instruction_->setText(instructions_->currentItem()->text(0));
    });
    connect(hits_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!rendering_)
            invoke([&] { showHit(); });
    });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        if (data_ && hits_->currentItem())
            invoke([&] { showHit(); });
    });
    connect(frames_, &QComboBox::currentIndexChanged, this, [this] {
        if (rendering_ || !data_ || !hits_->currentItem())
            return;
        const auto id = frames_->currentData().toString();
        frame_ = id.isEmpty() ? std::nullopt : std::optional(id.toStdString());
        invoke([&] { renderFrame(); });
        codeTabs_->setCurrentIndex(1);
    });
    connect(files_, &QComboBox::currentIndexChanged, this, [this] {
        if (!rendering_)
            invoke([&] { showSource(); });
    });
    connect(sourceLine_, &QSpinBox::valueChanged, this, [this](int line) {
        if (!rendering_) {
            controls_->setSourcePoint(files_->currentIndex(), line);
            showSource(true);
        }
    });
    connect(source_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (rendering_)
            return;
        const QSignalBlocker block(sourceLine_);
        sourceLine_->setValue(source_->textCursor().blockNumber() + 1);
        controls_->setSourcePoint(files_->currentIndex(), sourceLine_->value());
    });
    setWorkerBusy(false);
}
void CheckpointView::invoke(const std::function<void()> &fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        emit error(QString::fromUtf8(e.what()));
    }
}
void CheckpointView::setContext(QString key, bool available) {
    context_ = std::move(key);
    available_ = available;
    if (data_)
        summary_->setText(loadedContext_ == context_
                              ? summaryText_
                              : stage_.toUpper() + " · Context changed · Read to refresh");
    setWorkerBusy(busy_);
}
void CheckpointView::setWorkerBusy(bool busy) {
    busy_ = busy;
    captureBar_->setEnabled(!busy);
    for (auto action : captureBar_->actions())
        action->setEnabled(action->objectName() == "checkpointExport"
                               ? !busy && data_ && data_->result.contains("register_capture")
                               : !busy && available_);
    stepBar_->setEnabled(!busy && bool(data_));
    controls_->setEnabled(!busy && bool(data_));
}
void CheckpointView::request(bool catalog, bool trace, bool selected) {
    if (busy_ || !available_)
        throw std::runtime_error("Select an idle draw event first");
    if (!catalog && (!data_ || loadedContext_ != context_))
        throw std::runtime_error("Read the current shader after changing event, experiment or driver");
    QStringList args{"--shader-stage", stage_};
    if (!catalog) {
        bool valid = false;
        const auto number = instruction_->text().toULongLong(&valid);
        if ((!trace || (stage_ == "hs" && !selected)) &&
            (!valid || !data_->entries.contains(number) ||
             data_->entries.at(number).at("checkpoint_allowed") != true))
            throw std::runtime_error("Select an executable original instruction");
        if (trace)
            args << "--trace";
        else
            args << "--instruction" << QString::number(number);
        if (stage_ == "hs" && trace && !selected)
            args << "--hs-phase" << text(data_->entries.at(number).at("hs_phase"));
        if (selected) {
            const auto index = position(false);
            const auto selector = checkpoint::selectorFromSnapshot(
                data_->result, data_->registers(index), data_->rows.at(index),
                match_->currentData().toString().toStdString());
            selector_ = std::make_unique<QTemporaryDir>();
            if (!selector_->isValid())
                throw std::runtime_error("Cannot create input selector directory");
            const auto path = selector_->path() + "/input-selector.json";
            checkpoint::writeSelector(path.toStdWString(), selector);
            args << "--input-selector" << path;
            if (stage_ == "hs") {
                args << "--hs-phase" << text(selector.at("hs_phase").at("id"));
            }
        }
    }
    emit captureRequested(args, catalog);
}
void CheckpointView::loadOutput(const QString &directory, bool catalog) {
    auto loaded = std::make_shared<Data>();
    loaded->directory = directory;
    loaded->result = readJson(directory + "/checkpoint.json");
    if (loaded->result.at("action") != (stage_ + "-checkpoint").toStdString())
        throw std::runtime_error("Checkpoint stage does not match this view");
    const auto shader = readFile(directory + "/shader.dxbc");
    if (sha256(Bytes(reinterpret_cast<const uint8_t *>(shader.constData()), size_t(shader.size()))) !=
        loaded->result.at("shader_sha256").get<std::string>())
        throw std::runtime_error("Checkpoint shader identity mismatch");
    loaded->rows = loaded->result.value("hits_preview", Json::array());
    if (!loaded->rows.is_array() || loaded->rows.size() > 10000)
        throw std::runtime_error("Checkpoint preview exceeds display limit");
    for (const auto &entry : loaded->result.at("catalog"))
        if (!entry.at("instruction").is_null())
            loaded->entries[entry.at("instruction").get<uint64_t>()] = entry;
    if (loaded->result.contains("register_capture")) {
        loaded->snapshots = readFile(directory + "/snapshots.bin");
        const auto &meta = loaded->result.at("register_capture");
        const auto stride = meta.at("record_stride").get<uint64_t>(),
                   records = meta.at("records").get<uint64_t>();
        if (!stride || records > uint64_t(loaded->snapshots.size()) / stride ||
            records * stride != uint64_t(loaded->snapshots.size()))
            throw std::runtime_error("Checkpoint snapshot size mismatch");
        for (const auto &row : loaded->rows)
            if (row.at("record").get<uint64_t>() >= records)
                throw std::runtime_error("Checkpoint preview record out of bounds");
    }
    controls_->reset(
        loaded->result, loaded->rows, [loaded](size_t i) { return loaded->values(i); },
        !catalog && bool(data_));
    data_ = std::move(loaded);
    loadedContext_ = context_;
    frame_.reset();
    values_.clear();
    visible_.clear();
    viewLocation_ = nullptr;
    rendering_ = true;
    auto restore = qScopeGuard([&] { rendering_ = false; });
    instructions_->clear();
    hits_->clear();
    registers_->clear();
    variables_->clear();
    frames_->clear();
    frames_->addItem("Execution position", "");
    files_->clear();
    stack_->clear();
    for (const auto &[number, entry] : data_->entries) {
        auto assembly = text(entry.at("assembly"));
        if (entry.contains("hs_phase"))
            assembly =
                "[" + text(entry.at("hs_phase")) + " " + text(entry.at("phase_kind")) + "] " + assembly;
        auto item =
            new QTreeWidgetItem(instructions_, {QString::number(number),
                                                entry.at("checkpoint_allowed") == true ? "" : "—", assembly});
        item->setData(0, Qt::UserRole, qulonglong(number));
        item->setToolTip(2, assembly);
    }
    const auto first = std::find_if(data_->entries.begin(), data_->entries.end(), [](const auto &item) {
        return item.second.at("checkpoint_allowed") == true;
    });
    instruction_->setText(first == data_->entries.end() ? "" : QString::number(first->first));
    for (const auto &file : data_->result.value("source_lines", Json::object()).value("files", Json::array()))
        files_->addItem(text(file.at("name")));
    for (size_t i = 0; i < data_->rows.size(); ++i) {
        const auto &row = data_->rows.at(i);
        auto identity = "Primitive " + text(row.value("primitive_id", Json(nullptr)));
        if (stage_ == "gs")
            identity += " · GS " + text(row.value("gs_instance", Json(nullptr)));
        else if (stage_ == "hs")
            identity += " · Phase " + text(row.at("hs_phase")) + " · " +
                        text(row.value("phase_instance", Json(nullptr)));
        else
            identity += " · " + text(row.value("domain_location", Json(nullptr)));
        auto item = new QTreeWidgetItem(hits_, {text(row.at("record")), text(row.at("invocation")),
                                                text(row.at("hit")),
                                                text(row.value("instruction", Json(nullptr))), identity});
        item->setData(0, Qt::UserRole, qulonglong(i));
        item->setToolTip(4, identity);
    }
    auto summary = stage_.toUpper() + " · Shader " + text(data_->result.at("shader"));
    if (data_->result.contains("record_count"))
        summary += " · " + text(data_->result.at("record_count")) + " records";
    if (data_->result.value("preview_truncated", false))
        summary += " · First 10,000 shown";
    summaryText_ = summary;
    summary_->setText(summaryText_);
    summary_->setToolTip(text(data_->result.value("limits", Json::array())));
    const bool history = data_->result.at("source_variables").value("format", "") == "SDBG assignments";
    valueTabs_->setTabText(2, history ? "Breakpoints / History" : "Breakpoints / Watches");
    valueTabs_->setTabToolTip(2, history ? "Watches use the latest observed assignment in the active scope"
                                         : "Watches use the selected source frame");
    updateBreakpoints();
    rendering_ = false;
    restore.dismiss();
    showSource();
    if (!data_->rows.empty())
        go(0);
    setWorkerBusy(busy_);
}
void CheckpointView::keepOutput(std::unique_ptr<QTemporaryDir> directory) { output_ = std::move(directory); }
size_t CheckpointView::position(bool trace) const {
    if (!data_ || !hits_->currentItem() || (trace && !data_->result.value("trace", false)))
        throw std::runtime_error("Capture a trace and select a snapshot first");
    return hits_->currentItem()->data(0, Qt::UserRole).toULongLong();
}
void CheckpointView::go(size_t index) {
    if (!data_ || index >= data_->rows.size())
        throw std::runtime_error("Snapshot is outside the visible trace");
    const QSignalBlocker block(hits_);
    hits_->setCurrentItem(hits_->topLevelItem(int(index)));
    hits_->scrollToItem(hits_->currentItem());
    showHit();
}
void CheckpointView::showHit() {
    const auto index = position(false);
    const auto &row = data_->rows.at(index), &entry = data_->entry(index);
    const QSignalBlocker frameBlock(frames_), instructionBlock(instructions_);
    registers_->clear();
    for (const auto &reg : data_->registers(index)) {
        QStringList cells{text(reg.at("name"))};
        QString mask;
        for (size_t c = 0; c < 4; ++c) {
            QString value;
            if (reg.at("written").at(c) == true) {
                const auto bits = reg.at("bits").at(c).get<uint32_t>();
                mask += QChar("xyzw"[c]);
                value = mode_->currentIndex() == 0 ? QString("0x%1").arg(bits, 8, 16, QChar('0'))
                                                   : QString::fromStdString(source_detail::scalarText(
                                                         mode_->currentIndex() == 1   ? "uint"
                                                         : mode_->currentIndex() == 2 ? "int"
                                                                                      : "float",
                                                         bits));
            }
            cells << value;
        }
        cells << mask;
        new QTreeWidgetItem(registers_, cells);
    }
    instruction_->setText(text(entry.at("instruction")));
    for (int i = 0; i < instructions_->topLevelItemCount(); ++i)
        if (instructions_->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() ==
            entry.at("instruction").get<uint64_t>()) {
            instructions_->setCurrentItem(instructions_->topLevelItem(i));
            instructions_->scrollToItem(instructions_->currentItem());
            break;
        }
    auto stack = controls_->settings().source.stack(index);
    frames_->clear();
    frames_->addItem("Execution position", "");
    frame_.reset();
    QStringList names;
    for (const auto &f : stack.at("frames")) {
        const auto label = text(f.at("name")) + " · " + text(f.at("id"));
        frames_->addItem(label, text(f.at("id")));
        names << text(f.at("name"));
    }
    QString detail = "Invocation " + text(row.at("invocation")) + " · Hit " + text(row.at("hit"));
    if (row.contains("call_stack"))
        for (const auto &call : row.at("call_stack"))
            detail += " → l" + text(call.at("label"));
    detail += " · " + (names.isEmpty() ? text(stack.at("status")) : names.join(" → "));
    stack_->setText(detail);
    stack_->setToolTip(text(row));
    values_ = data_->values(index);
    renderFrame();
}
void CheckpointView::renderFrame() {
    const auto index = position(false);
    auto &source = controls_->settings().source;
    visible_ = values_;
    viewLocation_ = source.location(index);
    if (frame_) {
        const auto active = source.path(index);
        if (!active || std::find(active->begin(), active->end(), *frame_) == active->end())
            throw std::runtime_error("Source frame is not active");
        viewLocation_ = sourceFrameLocation(data_->result.at("source_stack"), *frame_,
                                            data_->entry(index).at("word_offset").get<uint64_t>() * 4);
        visible_ = sourceFrameLocals(data_->result.at("source_variables"), values_, *frame_);
    }
    variables_->clear();
    for (const auto &value : visible_) {
        auto scope = text(value.at("scope_label"));
        if (data_->result.at("source_variables").value("format", "") == "SDBG assignments" &&
            scope.startsWith(QString::fromUtf8("行 ")))
            scope.replace(0, 2, "Line ");
        auto item = new QTreeWidgetItem(variables_, {text(value.at("name")), scope, text(value.at("type")),
                                                     text(value.at("value")), text(value.at("status"))});
        item->setToolTip(3, text(value));
    }
    if (!viewLocation_.is_null()) {
        const QSignalBlocker fileBlock(files_), lineBlock(sourceLine_);
        files_->setCurrentIndex(viewLocation_.at("file").get<int>());
        sourceLine_->setValue(viewLocation_.at("line_start").get<int>());
    }
    controls_->setSourcePoint(files_->currentIndex(), sourceLine_->value());
    showSource(true);
    controls_->setRecord(index, frame_);
    controls_->setToolTip("Watch scope: " + frames_->currentText());
}
void CheckpointView::showSource(bool follow) {
    if (!data_)
        return;
    const bool previous = rendering_;
    rendering_ = true;
    auto restore = qScopeGuard([&] { rendering_ = previous; });
    const auto files = data_->result.value("source_lines", Json::object()).value("files", Json::array());
    const auto file = files_->currentIndex();
    const auto content =
        file >= 0 && size_t(file) < files.size() ? text(files.at(size_t(file)).at("text")) : QString();
    if (source_->toPlainText() != content)
        source_->setPlainText(content);
    QList<QTextEdit::ExtraSelection> selections;
    auto highlight = [&](int first, int last, QColor color) {
        const auto begin = source_->document()->findBlockByNumber(first - 1),
                   end = source_->document()->findBlockByNumber(last - 1);
        if (!begin.isValid() || !end.isValid())
            return;
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(begin);
        selection.cursor.setPosition(end.position() + end.length() - 1, QTextCursor::KeepAnchor);
        selection.format.setBackground(color);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selections.push_back(selection);
    };
    for (const auto &point : controls_->settings().source.exportBreakpoints())
        if (point.at("file") == file)
            highlight(point.at("line"), point.at("line"), QColor("#71424b"));
    if (!viewLocation_.is_null() && viewLocation_.at("file") == file)
        highlight(viewLocation_.at("line_start"), viewLocation_.at("line_end"), QColor("#23556c"));
    source_->setExtraSelections(selections);
    if (follow) {
        const auto block = source_->document()->findBlockByNumber(sourceLine_->value() - 1);
        if (block.isValid()) {
            source_->setTextCursor(QTextCursor(block));
            source_->ensureCursorVisible();
        }
    }
    controls_->setSourcePoint(file, sourceLine_->value());
}
void CheckpointView::move(int direction) {
    const auto index = position();
    if (navigation_->currentIndex() == 1) {
        go(controls_->settings().source.next(index, direction));
        codeTabs_->setCurrentIndex(1);
        return;
    }
    if ((direction < 0 && !index) || (direction > 0 && index + 1 >= data_->rows.size()))
        throw std::runtime_error("Visible invocation boundary reached");
    const auto target = direction < 0 ? index - 1 : index + 1;
    if (!controls_->settings().source.same(index, target))
        throw std::runtime_error("Visible invocation boundary reached");
    go(target);
}
void CheckpointView::step(bool out) {
    const auto index = position();
    auto &source = controls_->settings().source;
    if (navigation_->currentIndex() == 1) {
        go(source.functionStep(index, out));
        codeTabs_->setCurrentIndex(1);
        return;
    }
    const auto depth = data_->rows.at(index).value("call_depth", 0u);
    if (out && !depth)
        throw std::runtime_error("Already at the original entry function");
    for (size_t i = index + 1; i < data_->rows.size() && source.same(index, i); ++i) {
        const auto next = data_->rows.at(i).value("call_depth", 0u);
        if (next < depth || (!out && next == depth)) {
            go(i);
            return;
        }
    }
    throw std::runtime_error("No return target in this invocation's visible trace");
}
void CheckpointView::seek(bool runTo) {
    const auto index = position();
    auto &settings = controls_->settings();
    if (!runTo && navigation_->currentIndex() == 1) {
        go(settings.source.seek(index));
        codeTabs_->setCurrentIndex(1);
        return;
    }
    auto wanted = settings.instructions;
    if (runTo) {
        bool valid;
        const auto n = instruction_->text().toULongLong(&valid);
        if (!valid)
            throw std::runtime_error("Invalid instruction");
        wanted = {n};
    }
    if (wanted.empty())
        throw std::runtime_error("Set an instruction breakpoint first");
    for (size_t i = index + 1; i < data_->rows.size() && settings.source.same(index, i); ++i)
        if (wanted.contains(data_->rows.at(i).at("instruction").get<uint64_t>())) {
            go(i);
            return;
        }
    throw std::runtime_error("No instruction breakpoint hit in this invocation's visible trace");
}
void CheckpointView::toggleBreakpoint() {
    if (!data_)
        throw std::runtime_error("Read the original shader first");
    auto &settings = controls_->settings();
    if (navigation_->currentIndex() == 1) {
        settings.source.toggle(files_->currentIndex(), sourceLine_->value());
        controls_->refresh();
        showSource();
        return;
    }
    bool valid;
    const auto n = instruction_->text().toULongLong(&valid);
    if (!valid || !data_->entries.contains(n) || data_->entries.at(n).at("checkpoint_allowed") != true)
        throw std::runtime_error("Instruction is not a supported breakpoint");
    if (settings.instructions.contains(n))
        settings.instructions.erase(n);
    else {
        if (settings.instructions.size() >= 4096)
            throw std::runtime_error("At most 4096 instruction breakpoints are supported");
        settings.instructions.insert(n);
    }
    updateBreakpoints();
}
void CheckpointView::updateBreakpoints() {
    const auto &breakpoints = controls_->settings().instructions;
    for (int i = 0; i < instructions_->topLevelItemCount(); ++i) {
        auto item = instructions_->topLevelItem(i);
        const auto id = item->data(0, Qt::UserRole).toULongLong();
        item->setText(1, breakpoints.contains(id)                                 ? "●"
                         : data_->entries.at(id).at("checkpoint_allowed") == true ? ""
                                                                                  : "—");
    }
}
CheckpointView::Json CheckpointView::exportReport() const {
    if (!data_ || !data_->result.contains("register_capture"))
        throw std::runtime_error("Capture checkpoints first");
    auto result = data_->result;
    result["source_breakpoints"] = controls_->settings().source.exportBreakpoints();
    result["native_debug_config"] = controls_->configuration();
    if (hits_->currentItem()) {
        const auto record = data_->rows.at(position(false)).at("record");
        if (!values_.empty())
            result["selected_source_variables"] = {{"record", record}, {"values", values_}};
        if (frame_)
            result["selected_source_frame"] = {{"record", record},
                                               {"frame_id", *frame_},
                                               {"location", viewLocation_},
                                               {"variables", visible_}};
        if (!controls_->settings().watches.empty())
            result["selected_native_watches"] = {{"record", record},
                                                 {"frame_id", frame_ ? Json(*frame_) : Json(nullptr)},
                                                 {"values", controls_->watchResults()}};
    }
    return result;
}
void CheckpointView::exportArchive(const QString &path) const {
    const auto report = exportReport();
    auto bytes = [](const Json &value) {
        const auto raw = value.dump(2);
        return QByteArray(raw.data(), qsizetype(raw.size()));
    };
    std::vector<ZipEntry> entries{{"checkpoint.json", {}, bytes(report)}};
    for (const auto *name : {"shader.asm", "shader.dxbc", "snapshots.bin", "hits.csv", "registers.csv"})
        entries.push_back({name, data_->directory + '/' + name, {}});
    const auto runtime = readJson(data_->directory + "/report.json");
    entries.push_back({"result.json", {}, bytes(runtime)});
    entries.push_back({"loaded_modules.json", {}, bytes(runtime.at("loaded_modules"))});
    const auto &meta = report.at("register_capture");
    if (meta.contains("input_selector"))
        entries.push_back({"input-selector.json", {}, bytes(meta.at("input_selector"))});
    writeZipArchive(path, entries);
}
} // namespace flora
