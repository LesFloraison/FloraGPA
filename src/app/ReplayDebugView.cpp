#include "ReplayDebugView.h"
#include "application/NativeDebugConfig.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

namespace flora {
namespace {
using Json = nlohmann::json;
QString text(const Json &v) {
    return v.is_null() ? QString("—")
                       : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
QString valueText(const Json &values, const std::string &kind) {
    QStringList parts;
    for (const auto &v : values) {
        if (v.is_number_float()) {
            const auto n = v.get<double>();
            parts << (n == 0 && std::signbit(n) ? QString("-0")
                                                : QString::number(n, 'g', kind == "double" ? 17 : 9));
        } else
            parts << text(v);
    }
    return parts.join(", ");
}
QTreeWidget *table(const char *name, const QStringList &columns) {
    auto tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setHeaderLabels(columns);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setStretchLastSection(true);
    return tree;
}
void tip(QTreeWidgetItem *item) {
    for (int i = 0; i < item->columnCount(); ++i)
        item->setToolTip(i, item->text(i));
}
Json triple(const QLineEdit *edit) {
    const auto parts = edit->text().trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() != 3)
        throw std::runtime_error("Enter three unsigned coordinates");
    Json result = Json::array();
    for (const auto &part : parts) {
        bool ok = false;
        const auto n = part.toULongLong(&ok);
        if (!ok || part.startsWith('-') || part.startsWith('+') || n > UINT32_MAX)
            throw std::runtime_error("Coordinates must fit uint32");
        result.push_back(n);
    }
    return result;
}
uint32_t invocationIndex(const QLineEdit *edit) {
    const auto text = edit->text().trimmed();
    bool ok = false;
    const auto value = text.toULongLong(&ok);
    if (!ok || text.startsWith('-') || text.startsWith('+') || value > UINT32_MAX)
        throw std::runtime_error("Invocation index must fit uint32");
    return uint32_t(value);
}
} // namespace
ReplayDebugView::ReplayDebugView(QString stage, QWidget *parent) : QWidget(parent), stage_(std::move(stage)) {
    setObjectName("replayDebug-" + stage_);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto toolbar = new QToolBar;
    layout->addWidget(toolbar);
    auto action = [&](QToolBar *bar, const char *name, const QString &label,
                      const std::function<void()> &fn) {
        auto a = bar->addAction(label);
        a->setObjectName(name);
        connect(a, &QAction::triggered, this, [this, fn] { invoke(fn); });
        return a;
    };
    read_ = action(toolbar, "replayDebugRead", "Debug", [this] {
        request();
        invalidate();
        summary_->setText("Reading…");
        emit readRequested();
    });
    cancel_ = action(toolbar, "replayDebugCancel", "Cancel", [this] { emit cancelRequested(); });
    export_ = action(toolbar, "replayDebugExport", "Export…", [this] {
        auto path =
            QFileDialog::getSaveFileName(this, "Export shader debug trace", "debug.json", "JSON (*.json)");
        if (!path.isEmpty())
            exportResult(path);
    });
    auto toggle = toolbar->addAction("Details");
    toggle->setObjectName("replayDebugDetailsToggle");
    toggle->setCheckable(true);
    toolbar->addSeparator();
    backend_ = action(toolbar, "replayDebugBackend", "RenderDoc…", [this] {
        auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", backendPath_,
                                                 "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    inputs_ = new QToolBar;
    layout->addWidget(inputs_);
    auto spin = [&](const char *name, const QString &label, int max) {
        inputs_->addWidget(new QLabel(label));
        auto w = new QSpinBox;
        w->setObjectName(name);
        w->setRange(0, max);
        w->setMaximumWidth(115);
        inputs_->addWidget(w);
        connect(w, &QSpinBox::valueChanged, this, &ReplayDebugView::invalidate);
        return w;
    };
    if (stage_ == "ps") {
        x_ = spin("debugX", " X ", INT_MAX);
        y_ = spin("debugY", " Y ", INT_MAX);
        sample_ = spin("debugSample", " Sample ", 31);
    } else if (stage_ == "vs") {
        auto index = [&](const char *name, const QString &label) {
            inputs_->addWidget(new QLabel(label));
            auto edit = new QLineEdit("0");
            edit->setObjectName(name);
            edit->setMaximumWidth(115);
            inputs_->addWidget(edit);
            connect(edit, &QLineEdit::textChanged, this, &ReplayDebugView::invalidate);
            return edit;
        };
        vertex_ = index("debugVertex", " Vertex ");
        instance_ = index("debugInstance", " Instance ");
    } else {
        auto field = [&](const char *name, const QString &label) {
            inputs_->addWidget(new QLabel(label));
            auto w = new QLineEdit("0 0 0");
            w->setObjectName(name);
            w->setMaximumWidth(210);
            inputs_->addWidget(w);
            connect(w, &QLineEdit::textChanged, this, &ReplayDebugView::invalidate);
            return w;
        };
        group_ = field("debugGroup", " Group ");
        thread_ = field("debugThread", " Thread ");
    }
    steps_ = new QToolBar;
    layout->addWidget(steps_);
    navigation_ = new QComboBox;
    navigation_->setObjectName("debugNavigation");
    navigation_->addItems({"Instruction", "Source"});
    steps_->addWidget(navigation_);
    action(steps_, "debugPrevious", "Previous", [this] {
        auto n = model_->position();
        if (navigation_->currentIndex()) {
            go(model_->nextSource(n, -1));
            code_->setCurrentIndex(1);
        } else {
            if (n <= 0)
                return;
            go(size_t(n - 1));
        }
    });
    action(steps_, "debugStep", "Step", [this] {
        auto n = model_->position();
        go(navigation_->currentIndex() ? model_->nextSource(n, 1)
                                       : (std::min)(size_t(n + 1), model_->size() - 1));
        if (navigation_->currentIndex())
            code_->setCurrentIndex(1);
    });
    const auto functionStep = [this](bool out) {
        const auto [index, reason] = model_->functionStep(model_->position(), out);
        go(index);
        code_->setCurrentIndex(1);
        summary_->setText(summary_->text() + " · " +
                          (reason == "breakpoint" ? "Breakpoint"
                           : out                  ? "Returned"
                                                  : "Step over"));
    };
    action(steps_, "debugOver", "Over", [functionStep] { functionStep(false); });
    action(steps_, "debugOut", "Out", [functionStep] { functionStep(true); });
    action(steps_, "debugContinue", "Continue", [this] {
        go(model_->seek(model_->position()));
        code_->setCurrentIndex(1);
        summary_->setText(summary_->text() + " · Breakpoint");
    });
    step_ = new QSpinBox;
    step_->setObjectName("debugStepIndex");
    step_->setMaximumWidth(105);
    steps_->addWidget(step_);
    connect(step_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!rendering_ && model_)
            invoke([&] { go(size_t(value)); });
    });
    instruction_ = new QLineEdit;
    instruction_->setObjectName("debugInstruction");
    instruction_->setPlaceholderText("Instruction");
    instruction_->setMaximumWidth(105);
    steps_->addWidget(instruction_);
    action(steps_, "debugRunTo", "Run to", [this] {
        bool ok = false;
        const auto target = instruction_->text().trimmed().toULongLong(&ok);
        if (!ok)
            throw std::runtime_error("Enter an instruction index");
        for (size_t i = size_t(model_->position() + 1); i < model_->size(); ++i)
            if (model_->result()["steps"][i]["nextInstruction"] == target) {
                go(i);
                return;
            }
        throw std::runtime_error("Instruction is not encountered later in this trace");
    });
    summary_ = new QLabel(stage_.toUpper() + " · Select an event");
    summary_->setObjectName("replayDebugSummary");
    summary_->setMargin(4);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    auto vertical = new QSplitter(Qt::Vertical);
    vertical->setChildrenCollapsible(false);
    layout->addWidget(vertical, 1);
    auto split = new QSplitter;
    vertical->addWidget(split);
    code_ = new QTabWidget;
    code_->setObjectName("debugCodeTabs");
    code_->setDocumentMode(true);
    split->addWidget(code_);
    assembly_ = new QPlainTextEdit;
    assembly_->setReadOnly(true);
    assembly_->setFont(QFont("Cascadia Mono", 10));
    assembly_->setObjectName("debugAssembly");
    assembly_->setLineWrapMode(QPlainTextEdit::NoWrap);
    code_->addTab(assembly_, "Assembly");
    auto sourcePage = new QWidget;
    auto sourceLayout = new QVBoxLayout(sourcePage);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    auto sourceBar = new QToolBar;
    sourceLayout->addWidget(sourceBar);
    files_ = new QComboBox;
    files_->setObjectName("debugSourceFile");
    files_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    files_->setMinimumContentsLength(18);
    sourceBar->addWidget(files_);
    line_ = new QSpinBox;
    line_->setObjectName("debugSourceLine");
    line_->setRange(1, INT_MAX);
    line_->setMaximumWidth(85);
    sourceBar->addWidget(line_);
    action(sourceBar, "debugToggleBreakpoint", "Breakpoint", [this] {
        if (!model_)
            return;
        model_->toggle(point());
        refreshRules();
        showSource(false);
    });
    source_ = new QPlainTextEdit;
    source_->setReadOnly(true);
    source_->setFont(QFont("Cascadia Mono", 10));
    source_->setLineWrapMode(QPlainTextEdit::NoWrap);
    source_->setObjectName("debugSource");
    source_->setPlaceholderText("No source mapping");
    sourceLayout->addWidget(source_, 1);
    code_->addTab(sourcePage, "Source");
    connect(files_, &QComboBox::currentIndexChanged, this, [this] {
        if (!rendering_)
            showSource(false);
    });
    connect(source_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!rendering_)
            line_->setValue(source_->textCursor().blockNumber() + 1);
    });
    connect(line_, &QSpinBox::valueChanged, this, [this] {
        if (!rendering_)
            readRule();
    });
    values_ = new QTabWidget;
    values_->setObjectName("debugValueTabs");
    values_->setDocumentMode(true);
    split->addWidget(values_);
    auto registerPage = new QWidget;
    auto registerLayout = new QVBoxLayout(registerPage);
    registerLayout->setContentsMargins(0, 0, 0, 0);
    interpretation_ = new QComboBox;
    interpretation_->setObjectName("debugInterpretation");
    interpretation_->addItems({"float", "int", "uint", "hex"});
    registerLayout->addWidget(interpretation_);
    registers_ = table("debugRegisters", {"Register", "Value", "Type"});
    registers_->setColumnWidth(0, 100);
    registerLayout->addWidget(registers_, 1);
    values_->addTab(registerPage, "Registers");
    connect(interpretation_, &QComboBox::currentIndexChanged, this, [this] {
        if (model_ && !rendering_)
            invoke([this] { render(); });
    });
    locals_ = table("debugSourceVariables", {"Variable", "Value", "Type", "Scope / Status"});
    locals_->setColumnWidth(0, 130);
    locals_->setColumnWidth(1, 160);
    values_->addTab(locals_, "Variables");
    stack_ = table("debugCallstack", {"Frame", "Function"});
    stack_->setColumnWidth(0, 55);
    values_->addTab(stack_, "Callstack");
    auto rulePage = new QWidget;
    auto ruleLayout = new QVBoxLayout(rulePage);
    ruleLayout->setContentsMargins(0, 0, 0, 0);
    auto ruleBar = new QToolBar;
    ruleLayout->addWidget(ruleBar);
    condition_ = new QLineEdit;
    condition_->setObjectName("debugCondition");
    condition_->setPlaceholderText("Condition");
    condition_->setMaximumWidth(180);
    ruleBar->addWidget(condition_);
    hitMode_ = new QComboBox;
    hitMode_->setObjectName("debugHitMode");
    for (const auto &[label, key] : {std::pair{"Always", "always"},
                                     {"Equal", "equal"},
                                     {"At least", "at_least"},
                                     {"Multiple", "multiple"}})
        hitMode_->addItem(label, key);
    ruleBar->addWidget(hitMode_);
    count_ = new QLineEdit("1");
    count_->setObjectName("debugHitCount");
    count_->setMaximumWidth(90);
    ruleBar->addWidget(count_);
    action(ruleBar, "debugApplyRule", "Set", [this] {
        if (!model_)
            return;
        bool ok = false;
        const auto count = count_->text().trimmed().toLongLong(&ok);
        if (!ok || count < 1 || qulonglong(count) > UINT32_MAX)
            throw std::runtime_error("Invalid hit count");
        model_->setCondition(point(), condition_->text().toStdString());
        model_->setHitCount(point(), hitMode_->currentData().toString().toStdString(), count);
        refreshRules();
        showSource(false);
    });
    action(ruleBar, "debugRemoveRule", "Remove", [this] {
        auto item = rules_->currentItem();
        if (!model_ || !item)
            return;
        const auto &p = points_.at(item->data(0, Qt::UserRole).toULongLong());
        model_->toggle({p.at("file"), p.at("line")});
        refreshRules();
        showSource(false);
    });
    auto configBar = new QToolBar;
    ruleLayout->addWidget(configBar);
    action(configBar, "debugImport", "Import…", [this] {
        if (!model_)
            return;
        auto path = QFileDialog::getOpenFileName(this, "Import debug configuration", {}, "JSON (*.json)");
        if (!path.isEmpty()) {
            model_->importConfiguration(readNativeDebugConfig(path.toStdWString()));
            refreshRules();
            refreshWatches();
            showSource(false);
        }
    });
    action(configBar, "debugSave", "Save…", [this] {
        if (!model_)
            return;
        auto path = QFileDialog::getSaveFileName(this, "Save debug configuration", "debug-config.json",
                                                 "JSON (*.json)");
        if (!path.isEmpty())
            writeNativeDebugConfig(path.toStdWString(), model_->configuration());
    });
    rules_ = table("debugRules", {"Source", "Condition", "Hit count", "Visits"});
    ruleLayout->addWidget(rules_, 1);
    values_->addTab(rulePage, "Breakpoints");
    connect(rules_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rendering_ || !model_ || !rules_->currentItem())
            return;
        const auto &p = points_.at(rules_->currentItem()->data(0, Qt::UserRole).toULongLong());
        files_->setCurrentIndex(p.at("file"));
        line_->setValue(p.at("line"));
        condition_->setText(text(p.value("condition", Json(""))));
        const auto rule = p.value("hit_count", Json{{"mode", "always"}, {"count", 1}});
        hitMode_->setCurrentIndex(hitMode_->findData(text(rule.at("mode"))));
        count_->setText(text(rule.at("count")));
        code_->setCurrentIndex(1);
    });
    auto watchPage = new QWidget;
    auto watchLayout = new QVBoxLayout(watchPage);
    watchLayout->setContentsMargins(0, 0, 0, 0);
    auto watchBar = new QToolBar;
    watchLayout->addWidget(watchBar);
    watch_ = new QLineEdit;
    watch_->setObjectName("debugWatchExpression");
    watch_->setPlaceholderText("Watch expression");
    watchBar->addWidget(watch_);
    auto add = action(watchBar, "debugAddWatch", "Add", [this] {
        if (model_) {
            model_->addWatch(watch_->text().toStdString());
            watch_->clear();
            refreshWatches();
        }
    });
    connect(watch_, &QLineEdit::returnPressed, add, &QAction::trigger);
    action(watchBar, "debugRemoveWatch", "Remove", [this] {
        if (model_ && watches_->currentItem()) {
            model_->removeWatch(watches_->currentItem()->data(0, Qt::UserRole).toULongLong());
            refreshWatches();
        }
    });
    watches_ = table("debugWatchValues", {"Expression", "Type", "Value / Status"});
    watchLayout->addWidget(watches_, 1);
    values_->addTab(watchPage, "Watches");
    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setObjectName("replayDebugDetails");
    vertical->addWidget(details_);
    details_->hide();
    connect(toggle, &QAction::toggled, this, [this, vertical](bool visible) {
        details_->setVisible(visible);
        if (visible) {
            if (model_ && details_->document()->isEmpty())
                details_->setPlainText(QString::fromStdString(model_->result().dump(2)));
            vertical->setSizes({vertical->height() * 3 / 4, vertical->height() / 4});
        }
    });
    connect(locals_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!rendering_ && locals_->currentItem())
            details_->setPlainText(
                text(sourceValues_.at(locals_->currentItem()->data(0, Qt::UserRole).toULongLong()).dump(2)));
    });
    connect(stack_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!rendering_ && model_ && stack_->currentItem()) {
            const auto frames = model_->stack(model_->position());
            const auto i = stack_->currentItem()->data(0, Qt::UserRole).toULongLong();
            details_->setPlainText(QString::fromStdString(Json{
                {"step", model_->position()}, {"frame", i}, {"function", frames.at(i)}, {"callstack", frames}}
                                                              .dump(2)));
        }
    });
    split->setSizes({540, 480});
    setBackendPath(
        QSettings()
            .value("analysis/renderdoc", QDir(qEnvironmentVariable("ProgramFiles", "C:/Program Files"))
                                             .filePath("RenderDoc/renderdoc.dll"))
            .toString());
    updateActions();
}
void ReplayDebugView::invoke(const std::function<void()> &fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        summary_->setText(QString::fromUtf8(e.what()));
        emit error(QString::fromUtf8(e.what()));
    }
}
void ReplayDebugView::setContext(const QString &key, qulonglong event) {
    if (key_ != key || event_ != event) {
        key_ = key;
        event_ = event;
        invalidate();
    }
    updateActions();
}
void ReplayDebugView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void ReplayDebugView::setBackendPath(const QString &path) {
    if (path == backendPath_)
        return;
    backendPath_ = path;
    backend_->setToolTip(path);
    QSettings().setValue("analysis/renderdoc", path);
    invalidate();
}
void ReplayDebugView::selectPixel(int x, int y, int sample) {
    if (busy_ || stage_ != "ps")
        return;
    x_->setValue(x);
    y_->setValue(y);
    sample_->setValue(sample);
}
Json ReplayDebugView::request() const {
    if (key_.isEmpty() || !event_)
        throw std::runtime_error("Select an executed event first");
    Json job{{"action", stage_ == "ps"   ? "debug-pixel"
                        : stage_ == "vs" ? "debug-vertex"
                                         : "debug-thread"},
             {"gpa_event", event_}};
    if (stage_ == "ps")
        job.update({{"x", x_->value()}, {"y", y_->value()}, {"sample", sample_->value()}});
    else if (stage_ == "vs")
        job.update({{"vertex", invocationIndex(vertex_)}, {"instance", invocationIndex(instance_)}});
    else
        job.update({{"group", triple(group_)}, {"thread", triple(thread_)}});
    return job;
}
void ReplayDebugView::invalidate() {
    ++requestId_;
    model_.reset();
    rendering_ = true;
    for (auto tree : {registers_, locals_, stack_, rules_, watches_})
        tree->clear();
    assembly_->clear();
    source_->clear();
    details_->clear();
    files_->clear();
    step_->setRange(0, 0);
    sourceValues_ = points_ = Json::array();
    condition_->clear();
    count_->setText("1");
    hitMode_->setCurrentIndex(0);
    summary_->setText(stage_.toUpper() + " · " +
                      (event_ ? QString("API %1").arg(event_) : "Select an event"));
    rendering_ = false;
    updateActions();
}
void ReplayDebugView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && event_);
    cancel_->setEnabled(busy_);
    backend_->setEnabled(!busy_);
    inputs_->setEnabled(!busy_);
    export_->setEnabled(!busy_ && bool(model_));
    steps_->setEnabled(!busy_ && bool(model_));
    values_->setEnabled(!busy_ && bool(model_));
    code_->setEnabled(!busy_ && bool(model_));
    for (auto action : findChildren<QAction *>())
        if (action->objectName().startsWith("debug"))
            action->setEnabled(!busy_ && bool(model_));
}
bool ReplayDebugView::finish(uint64_t request, const Json &result) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(text(result.value("error", Json("Debugging failed"))));
        emit inspectionFinished(false);
        return false;
    }
    auto replacement = std::make_unique<ReplayDebugModel>(result);
    if (!replacement->size())
        throw std::runtime_error("Shader debugger returned no steps");
    replacement->move(0);
    model_ = std::move(replacement);
    rendering_ = true;
    assembly_->setPlainText(text(result.at("disassembly")));
    files_->clear();
    for (const auto &file : model_->files()) {
        const auto path = text(file.at("filename"));
        files_->addItem(QFileInfo(path).fileName());
        files_->setItemData(files_->count() - 1, path, Qt::ToolTipRole);
    }
    step_->setRange(0, int(model_->size() - 1));
    rendering_ = false;
    refreshRules();
    render();
    if (!details_->isHidden())
        details_->setPlainText(QString::fromStdString(result.dump(2)));
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void ReplayDebugView::go(size_t index) {
    if (!model_ || busy_)
        return;
    model_->move(index);
    render();
}
void ReplayDebugView::render() {
    if (!model_)
        return;
    rendering_ = true;
    auto guard = qScopeGuard([&] { rendering_ = false; });
    const auto position = model_->position();
    const auto &step = model_->result().at("steps").at(size_t(position));
    step_->setValue(int(position));
    summary_->setText(QString("%1 · API %2 · Step %3/%4 · Instruction %5 · Flags %6")
                          .arg(stage_.toUpper())
                          .arg(event_)
                          .arg(position)
                          .arg(model_->size() - 1)
                          .arg(text(step.at("nextInstruction")))
                          .arg(text(step.value("flags", Json(0)))));
    registers_->clear();
    for (const auto &row : model_->registerRows(interpretation_->currentText().toStdString()))
        tip(new QTreeWidgetItem(registers_, {text(row[0]), text(row[1]), text(row[2])}));
    sourceValues_ = model_->sourceValues();
    locals_->clear();
    for (size_t i = 0; i < sourceValues_.size(); ++i) {
        const auto &v = sourceValues_[i];
        const auto shape = QString(" %1×%2")
                               .arg((std::max)(1, v.at("rows").get<int>()))
                               .arg((std::max)(1, v.at("columns").get<int>()));
        auto item = new QTreeWidgetItem(locals_, {text(v.at("name")), valueText(v.at("values"), v.at("type")),
                                                  text(v.at("type")) + shape,
                                                  text(v.at("scope")) + " · " + text(v.at("status"))});
        item->setData(0, Qt::UserRole, qulonglong(i));
        tip(item);
    }
    stack_->clear();
    const auto frames = model_->stack(position);
    for (size_t i = 0; i < frames.size(); ++i) {
        auto item = new QTreeWidgetItem(stack_, {QString::number(i), text(frames[i])});
        item->setData(0, Qt::UserRole, qulonglong(i));
        tip(item);
    }
    QList<QTextEdit::ExtraSelection> highlight;
    const auto info = model_->instructionInfo(step.at("nextInstruction"));
    if (!info.is_null() && info.at("instruction") == step.at("nextInstruction")) {
        const auto line = info.value("lineInfo", Json::object()).value("disassemblyLine", 0);
        const auto block = assembly_->document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(block);
            s.format.setBackground(QColor(27, 87, 109));
            s.format.setProperty(QTextFormat::FullWidthSelection, true);
            highlight << s;
            assembly_->setTextCursor(s.cursor);
            assembly_->ensureCursorVisible();
        }
    }
    assembly_->setExtraSelections(highlight);
    refreshRules();
    refreshWatches();
    showSource(true);
}
ReplayDebugModel::Point ReplayDebugView::point() const { return {files_->currentIndex(), line_->value()}; }
void ReplayDebugView::showSource(bool follow) {
    if (!model_)
        return;
    const bool old = rendering_;
    rendering_ = true;
    auto guard = qScopeGuard([&] { rendering_ = old; });
    const auto location = model_->location(model_->position());
    source_->setToolTip(location.is_null() ? "No source location for this instruction"
                                           : QString("Next instruction · File %1 · %2:%3–%4:%5")
                                                 .arg(text(location[0]), text(location[1]), text(location[3]),
                                                      text(location[2]), text(location[4])));
    if (follow && !location.is_null()) {
        files_->setCurrentIndex(location[0]);
        line_->setValue(location[1]);
    }
    const auto file = files_->currentIndex();
    if (file < 0 || size_t(file) >= model_->files().size()) {
        source_->clear();
        return;
    }
    const auto contents = text(model_->files()[size_t(file)].at("contents"));
    if (source_->toPlainText() != contents)
        source_->setPlainText(contents);
    QList<QTextEdit::ExtraSelection> selections;
    for (const auto &p : model_->breakpoints())
        if (p.at("file") == file) {
            const auto block = source_->document()->findBlockByNumber(p.at("line").get<int>() - 1);
            if (block.isValid()) {
                QTextEdit::ExtraSelection s;
                s.cursor = QTextCursor(block);
                s.format.setBackground(QColor(103, 49, 53));
                s.format.setProperty(QTextFormat::FullWidthSelection, true);
                selections << s;
            }
        }
    if (!location.is_null() && location[0] == file) {
        auto start = source_->document()->findBlockByNumber(location[1].get<int>() - 1),
             end = source_->document()->findBlockByNumber(location[2].get<int>() - 1);
        if (start.isValid() && end.isValid()) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(start);
            s.cursor.setPosition(start.position() +
                                 qBound(0, location[3].get<int>() - 1, start.length() - 1));
            const int stop = location[4].get<int>();
            s.cursor.setPosition(end.position() +
                                     (stop ? qBound(0, stop, end.length() - 1) : end.length() - 1),
                                 QTextCursor::KeepAnchor);
            s.format.setBackground(QColor(27, 87, 109));
            selections << s;
            if (follow) {
                source_->setTextCursor(s.cursor);
                source_->ensureCursorVisible();
            }
        }
    }
    source_->setExtraSelections(selections);
    readRule();
}
void ReplayDebugView::readRule() {
    if (!model_)
        return;
    Json rule{{"condition", ""}, {"hit_count", {{"mode", "always"}, {"count", 1}}}};
    for (const auto &entry : model_->breakpoints())
        if (entry.at("file") == point().first && entry.at("line") == point().second) {
            rule = entry;
            break;
        }
    condition_->setText(text(rule.value("condition", Json(""))));
    const auto count = rule.value("hit_count", Json{{"mode", "always"}, {"count", 1}});
    hitMode_->setCurrentIndex(hitMode_->findData(text(count.at("mode"))));
    count_->setText(text(count.at("count")));
}
void ReplayDebugView::refreshRules() {
    if (!model_)
        return;
    const QSignalBlocker blocker(rules_);
    rules_->clear();
    points_ = model_->breakpoints();
    for (size_t i = 0; i < points_.size(); ++i) {
        const auto &p = points_[i];
        const auto f = p.at("file").get<size_t>();
        const auto name =
            f < model_->files().size() ? text(model_->files()[f].at("filename")) : text(p.at("file"));
        const auto rule = p.value("hit_count", Json{{"mode", "always"}, {"count", 1}});
        auto item = new QTreeWidgetItem(
            rules_,
            {QFileInfo(name).fileName() + ":" + text(p.at("line")), text(p.value("condition", Json(""))),
             text(rule.at("mode")) + " " + text(rule.at("count")),
             QString::number(model_->encounterCount(model_->position(), {p.at("file"), p.at("line")}))});
        item->setData(0, Qt::UserRole, qulonglong(i));
        tip(item);
        item->setToolTip(0, name + ":" + text(p.at("line")));
    }
}
void ReplayDebugView::refreshWatches() {
    watches_->clear();
    if (!model_)
        return;
    const auto rows = model_->watchResults();
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        const auto type = r.value("type", std::string{});
        auto item =
            new QTreeWidgetItem(watches_, {text(r.at("expression")), QString::fromStdString(type),
                                           r.at("status") == "available" ? valueText(r.at("values"), type)
                                                                         : text(r.at("error"))});
        item->setData(0, Qt::UserRole, qulonglong(i));
        tip(item);
    }
}
Json ReplayDebugView::exportReport() const {
    if (!model_)
        throw std::runtime_error("No shader trace to export");
    return model_->exportReport();
}
void ReplayDebugView::exportResult(const QString &path) const {
    const auto bytes = exportReport().dump(2);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !f.commit())
        throw std::runtime_error("Cannot export shader debug trace");
}
} // namespace flora
