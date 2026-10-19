#include "ReplayDebugView.h"
#include "application/NativeDebugConfig.h"
#include "application/RenderDocLibrary.h"
#include "application/RdcEvents.h"
#include <QAction>
#include <QCheckBox>
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
        inputs_->addWidget(new QLabel(" Primitive "));
        primitive_ = new QLineEdit;
        primitive_->setObjectName("debugPrimitive");
        primitive_->setPlaceholderText("Automatic");
        primitive_->setMaximumWidth(115);
        primitive_->setToolTip("Optional primitive ID. RenderDoc chooses among matching fragments; "
                               "this does not select an instance or a fragment index.");
        inputs_->addWidget(primitive_);
        connect(primitive_, &QLineEdit::textChanged, this, &ReplayDebugView::invalidate);
        pixelHistoryReference_ = new QCheckBox("GPU fragment reference");
        pixelHistoryReference_->setObjectName("debugPixelHistoryReference");
        pixelHistoryReference_->setToolTip("Requires an explicit primitive and a single draw instance. "
                                           "Runs additional Pixel History replays to read shader output before blending.");
        inputs_->addWidget(pixelHistoryReference_);
        connect(pixelHistoryReference_, &QCheckBox::toggled, this, &ReplayDebugView::invalidate);
        depthOptions_ = new QToolBar;
        layout->addWidget(depthOptions_);
        readOnlyDepthSnapshot_ = new QCheckBox("D32 depth snapshot (experimental)");
        readOnlyDepthSnapshot_->setObjectName("debugReadOnlyDepthSnapshot");
        readOnlyDepthSnapshot_->setToolTip("Requires the patched RenderDoc library. Copies read-only, single-sample "
                                            "D32 depth for isolated draws; preserves depth comparison and adds GPU work.");
        depthOptions_->addWidget(readOnlyDepthSnapshot_);
        connect(readOnlyDepthSnapshot_, &QCheckBox::toggled, this, &ReplayDebugView::invalidate);
        depthRoundtrip_ = new QCheckBox("D32S8 depth refresh (experimental)");
        depthRoundtrip_->setObjectName("debugDepthRoundtrip");
        depthRoundtrip_->setToolTip("Requires the patched RenderDoc library. Copies depth out and back before "
                                   "the selected isolated Draw, preserving the original attachment and depth writes. "
                                   "D32S8 only, stencil disabled, one mip/layer/sample/node and legacy depth states. "
                                   "Adds GPU work; does not establish full replay correctness.");
        depthOptions_->addWidget(depthRoundtrip_);
        connect(depthRoundtrip_, &QCheckBox::toggled, this, [this](bool enabled) {
            if (enabled) readOnlyDepthSnapshot_->setChecked(false);
            invalidate();
        });
        connect(readOnlyDepthSnapshot_, &QCheckBox::toggled, this, [this](bool enabled) {
            if (enabled) depthRoundtrip_->setChecked(false);
        });
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
        referenceSelection_ = new QLabel;
        referenceSelection_->setObjectName("computeReferenceSelection");
        inputs_->addWidget(referenceSelection_);
        clearReference_ = action(inputs_, "clearComputeReference", "Clear reference", [this] {
            textureReference_ = Json();
            referenceSelection_->clear();
            invalidate();
        });
    }
    indirectActionLabel_ = new QLabel(" Indirect action ");
    inputs_->addWidget(indirectActionLabel_);
    indirectAction_ = new QLineEdit;
    indirectAction_->setObjectName("debugIndirectAction");
    indirectAction_->setPlaceholderText("Automatic");
    indirectAction_->setMaximumWidth(160);
    indirectAction_->setToolTip("Optional zero-based Draw/Dispatch within ExecuteIndirect. Empty selects only an unambiguous single action.");
    inputs_->addWidget(indirectAction_);
    connect(indirectAction_, &QLineEdit::textChanged, this, &ReplayDebugView::invalidate);
    fastContraction_ = new QCheckBox("Fast DXIL contraction (experimental)");
    fastContraction_->setObjectName("debugFastContraction");
    fastContraction_->setToolTip("GPU multiply-add for adjacent single-use fast float32 pairs. "
        "Precise and shared products stay separate. This models permitted contraction; original driver intermediates are not observed.");
    inputs_->addWidget(fastContraction_);
    connect(fastContraction_, &QCheckBox::toggled, this, &ReplayDebugView::invalidate);
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
        code_->setCurrentIndex(model_->location(model_->position()).is_null() ? 0 : 1);
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
    action(steps_, "debugToggleInstructionBreakpoint", "Instruction breakpoint", [this] {
        const auto input = instruction_->text().trimmed();
        bool ok = false;
        const auto target = input.toULongLong(&ok);
        if (!ok || input.startsWith('-') || input.startsWith('+'))
            throw std::runtime_error("Enter an unsigned instruction index");
        model_->toggleInstruction(target);
        render();
    });
    summary_ = new QLabel(stage_.toUpper() + " · Select an event");
    summary_->setObjectName("replayDebugSummary");
    summary_->setMargin(4);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    backendNotice_ = new QLabel;
    backendNotice_->setObjectName("debugBackendNotice");
    backendNotice_->setWordWrap(true);
    backendNotice_->setMargin(4);
    backendNotice_->hide();
    layout->addWidget(backendNotice_);
    gpuReference_ = new QLabel;
    gpuReference_->setObjectName("debugGpuReference");
    gpuReference_->setWordWrap(true);
    gpuReference_->setMargin(4);
    gpuReference_->hide();
    layout->addWidget(gpuReference_);
    invocationReference_ = new QLabel;
    invocationReference_->setObjectName("debugGpuInvocationReference");
    invocationReference_->setWordWrap(true);
    invocationReference_->setMargin(4);
    invocationReference_->hide();
    layout->addWidget(invocationReference_);
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
    registers_->header()->setStretchLastSection(false);
    registers_->setColumnWidth(0, 150);
    registers_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    registers_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
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
    if (stage_ == "cs") {
        textureStores_ = table("debugTextureStores", {"Instruction", "Creation / RDC", "Coordinates", "Store operands", "GPU after dispatch", "Status"});
        for (int column = 0; column < 6; ++column)
            textureStores_->setColumnWidth(column, column == 0 ? 80 : column == 3 || column == 4 ? 160 : 120);
        textureStores_->setToolTip("Selected lane's typed texture stores; buffer stores and atomics are excluded. Double-click an instruction to step there, a mapped resource to select it, or coordinates to select the texture pixel for Replay. GPU values include all dispatch threads and later writes.");
        values_->addTab(textureStores_, "Texture writes");
        connect(textureStores_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
            if (busy_ || !model_ || !item) return;
            invoke([&] {
                const auto ordinal = item->data(0, Qt::UserRole).toUInt();
                const auto &row = model_->result().at("texture_stores").at("rows").at(ordinal);
                if (column == 2) {
                    if (!row.contains("location") || !row.at("resource_creation").is_string() || !row.at("resource_object").is_string())
                        throw std::runtime_error("This store has no mapped in-bounds texture pixel");
                    const auto &p = row.at("location");
                    emit texturePixelRequested(key_, event_, std::stoull(row.at("resource_creation").get<std::string>()),
                        std::stoull(row.at("resource_object").get<std::string>()), p.at("x").get<uint>(), p.at("y").get<uint>(),
                        p.at("mip").get<uint>(), p.at("layer").get<uint>(), p.at("sample").get<uint>());
                    return;
                }
                if (column == 1 && row.at("resource_creation").is_string()) {
                    emit resourceRequested(std::stoull(row.at("resource_creation").get<std::string>()));
                    return;
                }
                const auto &steps = model_->result().at("steps");
                const auto position = row.at("trace_position").get<size_t>();
                if (position >= steps.size() || steps[position].at("nextInstruction") != row.at("instruction"))
                    throw std::runtime_error("Texture store instruction occurrence is missing from the trace");
                go(position);
            });
        });
    }
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
        if (p.contains("instruction")) model_->toggleInstruction(p.at("instruction"));
        else model_->toggle({p.at("file"), p.at("line")});
        render();
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
            render();
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
    rules_ = table("debugRules", {"Location", "Condition", "Hit count", "Visits"});
    ruleLayout->addWidget(rules_, 1);
    values_->addTab(rulePage, "Breakpoints");
    connect(rules_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateRuleActions();
        if (rendering_ || !model_ || !rules_->currentItem())
            return;
        const auto &p = points_.at(rules_->currentItem()->data(0, Qt::UserRole).toULongLong());
        if (p.contains("instruction")) {
            instruction_->setText(text(p.at("instruction")));
            code_->setCurrentIndex(0);
            showAssembly(p.at("instruction"), true);
            return;
        }
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
    setBackendPath(configuredRenderDocLibrary(), false);
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
void ReplayDebugView::setContext(const QString &key, qulonglong event, bool dx12) {
    if (key_ != key || event_ != event || dx12_ != dx12) {
        dx12_ = dx12;
        key_ = key;
        event_ = event;
        textureReference_ = Json();
        indirectAction_->clear();
        fastContraction_->setChecked(false);
        if (referenceSelection_)
            referenceSelection_->clear();
        if (primitive_)
            primitive_->clear();
        if (pixelHistoryReference_)
            pixelHistoryReference_->setChecked(false);
        if (readOnlyDepthSnapshot_)
            readOnlyDepthSnapshot_->setChecked(false);
        if (depthRoundtrip_)
            depthRoundtrip_->setChecked(false);
        invalidate();
    }
    updateActions();
}
void ReplayDebugView::selectDepthRoundtrip(bool enabled) {
    if (depthRoundtrip_ && !busy_) depthRoundtrip_->setChecked(dx12_ && enabled);
}
void ReplayDebugView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void ReplayDebugView::setBackendPath(const QString &path, bool persist) {
    if (path == backendPath_)
        return;
    backendPath_ = path;
    backend_->setToolTip(path);
    if (persist)
        QSettings().setValue("analysis/renderdoc", path);
    invalidate();
}
void ReplayDebugView::selectPixel(int x, int y, int sample) {
    if (busy_ || stage_ != "ps")
        return;
    x_->setValue(x);
    y_->setValue(y);
    sample_->setValue(sample);
    primitive_->clear();
}
void ReplayDebugView::selectPrimitive(uint32_t primitive) {
    if (!busy_ && primitive_)
        primitive_->setText(QString::number(primitive));
}
void ReplayDebugView::selectVertex(uint32_t ordinal, uint32_t instance) {
    if (busy_ || stage_ != "vs") return;
    vertex_->setText(QString::number(ordinal));
    instance_->setText(QString::number(instance));
}
void ReplayDebugView::selectIndirectAction(qint64 ordinal) {
    if (busy_ || !dx12_) return;
    if (ordinal < -1 || ordinal > UINT32_MAX) throw std::runtime_error("Invalid indirect action ordinal");
    indirectAction_->setText(ordinal < 0 ? QString() : QString::number(ordinal));
}
void ReplayDebugView::selectTextureReference(qulonglong object, qulonglong creation, int x, int y, int mip,
                                             int layer, int sample) {
    if (busy_ || !dx12_ || stage_ != "cs" || !object || !creation || x < 0 || y < 0 || mip < 0 || layer < 0 ||
        sample < 0)
        return;
    textureReference_ = {{"resource", std::to_string(object)},
                         {"resource_creation", std::to_string(creation)},
                         {"x", x},
                         {"y", y},
                         {"mip", mip},
                         {"layer", layer},
                         {"sample", sample}};
    referenceSelection_->setText(QString(" Reference: creation %1 · (%2, %3) · mip %4 layer %5 sample %6 ")
                                     .arg(creation)
                                     .arg(x)
                                     .arg(y)
                                     .arg(mip)
                                     .arg(layer)
                                     .arg(sample));
    referenceSelection_->setToolTip("Reads this selected texel after the dispatch and before debugging. "
                                    "Thread coordinates are independent; a matching value does not prove "
                                    "this invocation wrote the texel.");
    invalidate();
}
Json ReplayDebugView::request() const {
    if (key_.isEmpty() || !event_)
        throw std::runtime_error("Select an executed event first");
    Json job{{"action", stage_ == "ps"   ? "debug-pixel"
                        : stage_ == "vs" ? "debug-vertex"
                                         : "debug-thread"},
             {"gpa_event", event_}};
    if (dx12_) {
        job.erase("gpa_event");
        job["execution"] = std::to_string(event_);
        if (fastContraction_->isChecked()) job["dxil_fast_contraction"] = true;
        if (!indirectAction_->text().trimmed().isEmpty()) job["indirect_action"] = invocationIndex(indirectAction_);
    }
    if (stage_ == "ps") {
        job.update({{"x", x_->value()}, {"y", y_->value()}, {"sample", sample_->value()}});
        if (!primitive_->text().trimmed().isEmpty())
            job["primitive"] = invocationIndex(primitive_);
        if (dx12_ && pixelHistoryReference_->isChecked()) {
            if (!job.contains("primitive") || job.at("primitive") == UINT32_MAX)
                throw std::runtime_error("GPU fragment reference requires an explicit primitive ID");
            job["pixel_history_reference"] = true;
        }
        if (dx12_ && readOnlyDepthSnapshot_->isChecked())
            job["readonly_depth_snapshot"] = true;
        if (dx12_ && depthRoundtrip_->isChecked())
            job["depth_roundtrip"] = true;
    } else if (stage_ == "vs")
        job.update({{"vertex", invocationIndex(vertex_)}, {"instance", invocationIndex(instance_)}});
    else {
        job.update({{"group", triple(group_)}, {"thread", triple(thread_)}});
        if (dx12_ && textureReference_.is_object())
            job.update(textureReference_);
    }
    return job;
}
void ReplayDebugView::invalidate() {
    ++requestId_;
    model_.reset();
    gpuReference_->clear();
    gpuReference_->hide();
    invocationReference_->clear();
    invocationReference_->hide();
    rendering_ = true;
    backendNotice_->clear();
    backendNotice_->hide();
    for (auto tree : {registers_, locals_, stack_, rules_, watches_})
        tree->clear();
    if (textureStores_) textureStores_->clear();
    assembly_->clear();
    source_->clear();
    details_->clear();
    files_->clear();
    step_->setRange(0, 0);
    sourceValues_ = points_ = Json::array();
    condition_->clear();
    count_->setText("1");
    hitMode_->setCurrentIndex(0);
    summary_->setText(
        stage_.toUpper() + " · " +
        (event_ ? QString(dx12_ ? "DX12 execution %1" : "API %1").arg(event_) : "Select an event"));
    rendering_ = false;
    updateActions();
}
void ReplayDebugView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && event_);
    cancel_->setEnabled(busy_);
    backend_->setEnabled(!busy_);
    inputs_->setEnabled(!busy_);
    if (depthOptions_) {
        depthOptions_->setVisible(dx12_);
        depthOptions_->setEnabled(!busy_);
    }
    indirectAction_->setVisible(dx12_);
    indirectActionLabel_->setVisible(dx12_);
    fastContraction_->setVisible(dx12_);
    if (pixelHistoryReference_)
        pixelHistoryReference_->setVisible(dx12_);
    if (readOnlyDepthSnapshot_)
        readOnlyDepthSnapshot_->setVisible(dx12_);
    if (depthRoundtrip_)
        depthRoundtrip_->setVisible(dx12_);
    if (clearReference_) {
        clearReference_->setVisible(dx12_);
        clearReference_->setEnabled(!busy_ && textureReference_.is_object());
        referenceSelection_->setVisible(dx12_);
    }
    export_->setEnabled(!busy_ && bool(model_));
    steps_->setEnabled(!busy_ && bool(model_));
    values_->setEnabled(!busy_ && bool(model_));
    code_->setEnabled(!busy_ && bool(model_));
    for (auto action : findChildren<QAction *>())
        if (action->objectName().startsWith("debug"))
            action->setEnabled(!busy_ && bool(model_));
    updateRuleActions();
}
bool ReplayDebugView::finish(uint64_t request, const Json &result) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(text(result.value("error", Json("Debugging failed"))));
        emit inspectionFinished(false);
        return false;
    }
    if (dx12_ && (result.value("backend", std::string()) != "dx12" ||
                  result.value("execution", Json()) != Json(std::to_string(event_))))
        throw std::runtime_error("Shader trace does not match the DX12 execution selection");
    if (dx12_ && result.value("dxil_fast_contraction", Json::object()).value("enabled", false) != fastContraction_->isChecked())
        throw std::runtime_error("Shader trace does not match the DXIL contraction mode");
    if (dx12_ && depthRoundtrip_ &&
        result.value("depth_roundtrip", Json::object()).value("requested", false) != depthRoundtrip_->isChecked())
        throw std::runtime_error("Shader trace does not match the D32S8 depth refresh mode");
    if (dx12_ && !indirectAction_->text().trimmed().isEmpty() &&
        result.value("indirect_selection", Json::object()).value("indirect_action", Json()) != invocationIndex(indirectAction_))
        throw std::runtime_error("Shader trace does not match the selected indirect action");
    if (dx12_ && result.contains("indirect_selection")) {
        const auto &selection = result.at("indirect_selection");
        if (indirectActionOrdinal(selection) < 0 || selection.at("execution") != std::to_string(event_) ||
            (indirectAction_->text().trimmed().isEmpty() && selection.at("indirect_action_count") != 1))
            throw std::runtime_error("Shader trace has ambiguous or mismatched indirect provenance");
    }
    auto replacement = std::make_unique<ReplayDebugModel>(result);
    if (!replacement->size())
        throw std::runtime_error("Shader debugger returned no steps");
    replacement->move(0);
    model_ = std::move(replacement);
    QStringList warnings;
    for (const auto &warning : result.value("debug_scope", Json::object()).value("warnings", Json::array()))
        warnings.append(text(warning));
    if (dx12_ && result.value("replay_policy", Json::object()).value("dx12_discard_policy", "") == "diagnostic_pattern")
        warnings.append("Diagnostic discard patterns are enabled. Shader reads of discarded data can differ from native replay.");
    if (const auto refresh = result.value("depth_roundtrip", Json()); refresh.is_object())
        warnings.prepend(QString("Experimental D32S8 depth refresh: %1 completed copies. "
                                 "Original depth writes preserved; full replay correctness is not established.")
                             .arg(text(refresh.value("copies", Json("unknown")))));
    if (result.contains("texture_stores"))
        warnings.append("Texture writes show this lane's operands before storage conversion. GPU values follow the whole dispatch and may include later writes; they do not validate individual stores.");
    const auto lifecycle = result.value("compute_input_lifecycle", Json());
    if (lifecycle.is_object() && lifecycle.value("patch_version", 0) < 2) {
        warnings.prepend("Obsolete DXIL input-retention report: the controller replayed the dispatch after trace creation. Its input-phase claims are invalid; regenerate this trace with the current analysis library.");
    } else if (lifecycle.is_object()) {
        const auto status = result.value("compute_buffer_inputs", Json::object()).value("status", "");
        const auto buffers = status == "observed_bytes_unchanged" ? "Read-only buffer observations match."
                           : status == "observed_bytes_changed" ? "Read-only buffer observations differ."
                           : status == "no_buffer_views" ? "No read-only buffer views to check."
                           : "Read-only buffer observations are incomplete.";
        const bool prepared = result.value("compute_preparation", Json::object()).value("status", "") == "single_pre_dispatch_preparation";
        warnings.prepend(QString("DXIL compute inputs retained during stepping; dispatch restored after trace release. %1 Texture inputs are not checked. %2")
            .arg(buffers, prepared ? "Inputs prepared once before dispatch; GPU references read after release."
                                  : "Earlier GPU references belong to a separate replay."));
    }
    if (const auto selection = result.value("indirect_selection", Json()); selection.is_object())
        warnings.prepend(QString("ExecuteIndirect action %1 of %2 (zero-based) · replay event %3")
            .arg(text(selection.at("indirect_action")), text(selection.at("indirect_action_count")), text(selection.at("eid"))));
    const auto instructionFlags = result.value("debug_scope", Json::object()).value("dxil_instruction_flags", Json::object());
    if (instructionFlags.value("status", std::string()) == "included" && instructionFlags.value("annotated_lines", uint64_t(0)))
        warnings.prepend("DXIL fast-math comments show allowed compiler optimizations, not observed driver instructions. Stepping values follow the interpreter.");
    backendNotice_->setText(warnings.join('\n'));
    backendNotice_->setVisible(!warnings.isEmpty());
    const auto reference = result.value("gpu_reference", Json());
    gpuReference_->clear();
    gpuReference_->setVisible(dx12_ && (stage_ == "ps" || stage_ == "cs") && !reference.is_null());
    if (dx12_ && (stage_ == "ps" || stage_ == "cs") && !reference.is_null()) {
        gpuReference_->setToolTip(QString::fromStdString(reference.dump(2)));
        if (reference.value("status", "") == "read") {
            const auto kind = reference.value("interpretation", "float");
            const auto key = kind == "uint" ? "uintValue" : kind == "sint" ? "intValue" : "floatValue";
            if (stage_ == "cs") {
                gpuReference_->setText(
                    QString("GPU selected texel after dispatch · (%1, %2) mip %3 layer %4 sample %5 · %6 %7\n"
                            "Read before debugger replay; this does not prove the selected invocation wrote "
                            "this location.")
                        .arg(text(reference.at("x")), text(reference.at("y")), text(reference.at("mip")),
                             text(reference.at("layer")), text(reference.at("sample")),
                             QString::fromStdString(kind), text(reference.at("value").at(key))));
                if (reference.value("scope", "") == "post_dispatch_after_trace_release")
                    gpuReference_->setText(gpuReference_->text().replace("Read before debugger replay;",
                        "Read after trace release executes the prepared dispatch;"));
            } else
                gpuReference_->setText(
                    QString("GPU target after event · RT0 mip %1 layer %2 sample %3 · %4 %5\n"
                            "Read before debugger replay; target conversion, blending and "
                            "other fragments may differ from shader output.")
                        .arg(text(reference.at("mip")), text(reference.at("layer")),
                             text(reference.at("sample")), QString::fromStdString(kind),
                             text(reference.at("value").at(key))));
            if (reference.contains("after_debug_value")) {
                const bool changed = reference.value("consistency", "") == "selected_texel_changed";
                if (reference.contains("after_trace_creation_value"))
                    gpuReference_->setText(
                        gpuReference_->text() +
                        QString("\nAfter trace creation: %1 %2")
                            .arg(QString::fromStdString(kind),
                                 text(reference.at("after_trace_creation_value").at(key))));
                gpuReference_->setText(
                    gpuReference_->text() +
                    QString("\nAfter debug replays: %1 %2 · %3")
                        .arg(QString::fromStdString(kind), text(reference.at("after_debug_value").at(key)),
                             changed ? "changed; reference cannot validate this trace"
                                     : "selected texel unchanged; input stability is not established"));
            }
        } else {
            gpuReference_->setText("GPU target reference unavailable: " +
                                   text(reference.value("reason", Json("Unknown"))));
        }
    }
    const auto invocationReference = result.value("gpu_invocation_reference", Json());
    invocationReference_->clear();
    invocationReference_->setToolTip({});
    invocationReference_->setVisible(dx12_ && stage_ == "ps" && invocationReference.is_object());
    if (dx12_ && stage_ == "ps" && invocationReference.is_object()) {
        invocationReference_->setToolTip(QString::fromStdString(invocationReference.dump(2)));
        if (invocationReference.value("status", "") == "read") {
            const auto kind = invocationReference.value("interpretation", "float");
            const auto field = kind == "uint" ? "uintValue" : kind == "sint" ? "intValue" : "floatValue";
            invocationReference_->setText(
                QString("GPU fragment output before blending · primitive %1 · target 0 · %2 %3\n"
                        "Separate Pixel History replay; compare with the selected shader output, not the final texture.")
                    .arg(text(invocationReference.at("primitive")), QString::fromStdString(kind),
                         text(invocationReference.at("value").at(field))));
            const auto comparison = invocationReference.value("trace_output_comparison", Json::object());
            const auto status = comparison.value("status", "");
            if (status == "output_mismatch")
                invocationReference_->setText(invocationReference_->text() +
                    "\nFinal trace output differs from the GPU fragment. This trace is not validated.");
            else if (status == "exact_output_match")
                invocationReference_->setText(invocationReference_->text() +
                    "\nFinal mapped output components match exactly; intermediate instructions remain unverified.");
            else if (comparison.contains("reason"))
                invocationReference_->setText(invocationReference_->text() +
                    "\nOutput comparison unavailable: " + text(comparison.at("reason")));
        } else {
            invocationReference_->setText("GPU fragment reference unavailable: " +
                text(invocationReference.value("reason", Json("Unknown"))));
        }
    }
    if (const auto invocation = result.value("vertex_invocation", Json());
        dx12_ && stage_ == "vs" && invocation.is_object()) {
        invocationReference_->setToolTip(QString::fromStdString(invocation.dump(2)));
        invocationReference_->setText(
            QString("Vertex invocation · ordinal %1 · instance %2 · actual vertex %3\n"
                    "Draw instances %4 · start instance %5")
                .arg(text(invocation.at("vertex_ordinal")), text(invocation.at("instance")),
                     text(invocation.at("actual_vertex_index")), text(invocation.at("instance_count")),
                     text(invocation.at("first_instance"))));
        if (invocation.contains("raw_index"))
            invocationReference_->setText(invocationReference_->text() +
                QString("\nIndex %1 · base vertex %2 · index-buffer byte offset %3")
                    .arg(text(invocation.at("raw_index")), text(invocation.at("base_vertex")),
                         text(invocation.at("index_byte_offset"))));
        if (invocation.value("indirect", false))
            invocationReference_->setText(invocationReference_->text() +
                "\nIndirect arguments may change between replays; bounds use the indexed action.");
        if (const auto reference = result.value("gpu_vertex_reference", Json()); reference.is_object()) {
            invocationReference_->setToolTip(QString::fromStdString(
                Json{{"invocation", invocation}, {"gpu_vertex_reference", reference},
                     {"vertex_input_snapshots", result.value("vertex_input_snapshots", Json())}}.dump(2)));
            if (reference.value("status", "") == "read") {
                invocationReference_->setText(invocationReference_->text() +
                    "\nGPU VS position " + text(reference.at("value").at("floatValue")));
                const auto comparison = reference.value("trace_output_comparison", Json::object());
                const auto status = comparison.value("status", "");
                if (status == "exact_output_match")
                    invocationReference_->setText(invocationReference_->text() +
                        "\nFinal position components match exactly.");
                else if (status == "output_mismatch")
                    invocationReference_->setText(invocationReference_->text() +
                        "\nFinal trace position differs from GPU output; this trace is not validated.");
                else
                    invocationReference_->setText(invocationReference_->text() +
                        "\nPosition comparison unavailable: " + text(comparison.value("reason", Json("Unknown"))));
                const auto outputs = reference.value("output_comparison", Json::object());
                if (outputs.value("status", "unavailable") != "unavailable") {
                    invocationReference_->setText(invocationReference_->text() +
                        QString("\nVS outputs: %1 match · %2 differ · %3 unavailable")
                            .arg(text(outputs.at("matched")), text(outputs.at("mismatched")), text(outputs.at("unavailable"))));
                    QStringList differences;
                    for (const auto &output : outputs.at("outputs"))
                        if (output.value("status", "") == "output_mismatch" && differences.size() < 6)
                            differences << text(output.at("semantic")) + text(output.at("semantic_index")) +
                                " components " + text(output.at("mismatched_components"));
                    if (!differences.empty()) invocationReference_->setText(invocationReference_->text() +
                        "\nOutput differences: " + differences.join("; ") + "; this trace is not validated.");
                    invocationReference_->setText(invocationReference_->text() +
                        "\nSeparate instrumented replay; instructions and input stability remain unverified.");
                } else {
                    invocationReference_->setText(invocationReference_->text() +
                        "\nSeparate instrumented replay; other outputs, instructions and input stability remain unverified.");
                }
            } else {
                invocationReference_->setText(invocationReference_->text() +
                    "\nGPU VS position unavailable: " + text(reference.value("reason", Json("Unknown"))));
            }
        }
        if (const auto snapshots = result.value("vertex_input_snapshots", Json()); snapshots.is_object()) {
            const auto status = snapshots.value("status", "unavailable");
            invocationReference_->setText(invocationReference_->text() + (status == "observed_bytes_changed"
                ? "\nSelected vertex input bytes changed between replays; this trace is not validated."
                : status == "observed_bytes_unchanged" ? "\nSelected vertex input bytes unchanged at three observation points."
                                                      : "\nSelected vertex input byte comparison unavailable."));
        }
        invocationReference_->show();
    }
    if (const auto invocation = result.value("compute_invocation", Json());
        dx12_ && stage_ == "cs" && invocation.is_object()) {
        invocationReference_->setToolTip(QString::fromStdString(invocation.dump(2)));
        invocationReference_->setText(
            QString("Compute invocation · group %1 · local thread %2\n"
                    "Dispatch groups %3 · shader group size %4 · dispatch-thread ID %5\n"
                    "Texture coordinates are computed by the shader; they need not equal the dispatch-thread ID.")
                .arg(text(invocation.at("group")), text(invocation.at("thread")),
                     text(invocation.at("group_count")), text(invocation.at("shader_group_size")),
                     text(invocation.at("dispatch_thread"))));
        if (invocation.value("indirect", false))
            invocationReference_->setText(invocationReference_->text() +
                "\nIndirect arguments may change between replays; bounds use the indexed action.");
        invocationReference_->show();
    }
    rendering_ = true;
    assembly_->setPlainText(text(result.at("disassembly")));
    if (textureStores_) {
        textureStores_->clear();
        const auto stores = result.value("texture_stores", Json::object());
        for (const auto &row : stores.value("rows", Json::array())) {
            const auto ref = row.value("gpu_reference", Json::object());
            const auto type = ref.value("interpretation", "float");
            const auto key = type == "uint" ? "uintValue" : type == "sint" ? "intValue" : "floatValue";
            const auto resource = row.at("resource_creation").is_string()
                ? text(row.at("resource_creation")) : "RDC " + text(row.at("rdc_resource"));
            auto item = new QTreeWidgetItem(textureStores_, {text(row.at("instruction")), resource,
                text(row.at("view_coordinates")), text(row.at("value")),
                ref.value("status", "") == "read" ? text(ref.at("value").at(key)) : QString("Unavailable"),
                row.value("status", "") == "observed_store" ? "Store attempt" : row.value("status", "") == "out_of_bounds" ? "Out of bounds" : "Operands unavailable"});
            item->setData(0, Qt::UserRole, row.at("ordinal").get<uint32_t>());
            item->setToolTip(1, text(row.at("resource_name")));
            item->setToolTip(2, QString::fromStdString(row.value("location", Json()).dump()));
            item->setToolTip(3, text(row.at("value")) + "\nRaw bits: " + text(row.at("value_bits")));
            item->setToolTip(4, QString::fromStdString(ref.dump(2)));
            item->setToolTip(5, QString::fromStdString(row.dump(2)));
        }
    }
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
    summary_->setText(QString(dx12_ ? "%1 · DX12 execution %2 · Step %3/%4 · Instruction %5 · Flags %6"
                                    : "%1 · API %2 · Step %3/%4 · Instruction %5 · Flags %6")
                          .arg(stage_.toUpper())
                          .arg(event_)
                          .arg(position)
                          .arg(model_->size() - 1)
                          .arg(text(step.at("nextInstruction")))
                          .arg(text(step.value("flags", Json(0)))));
    const auto instructionPoints = model_->instructionBreakpoints();
    if (!instructionPoints.empty())
        summary_->setText(summary_->text() + " · Instruction breakpoints " +
                          QString::fromStdString(instructionPoints.dump()));
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
    showAssembly(step.at("nextInstruction"), true);
    refreshRules();
    refreshWatches();
    showSource(true);
}
void ReplayDebugView::showAssembly(uint64_t instruction, bool follow) {
    QList<QTextEdit::ExtraSelection> highlight;
    const auto add = [&](uint64_t id, const QColor &color) {
        const auto info = model_->instructionInfo(id);
        if (info.is_null() || info.at("instruction") != id) return;
        const auto line = info.value("lineInfo", Json::object()).value("disassemblyLine", 0);
        const auto block = assembly_->document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(block);
            s.format.setBackground(color);
            s.format.setProperty(QTextFormat::FullWidthSelection, true);
            highlight << s;
        }
    };
    for (const auto &id : model_->instructionBreakpoints()) add(id, QColor(110, 45, 55));
    add(model_->result().at("steps").at(size_t(model_->position())).at("nextInstruction"), QColor(27, 87, 109));
    assembly_->setExtraSelections(highlight);
    const auto info = model_->instructionInfo(instruction);
    if (follow && !info.is_null() && info.at("instruction") == instruction) {
        const auto line = info.value("lineInfo", Json::object()).value("disassemblyLine", 0);
        const auto block = assembly_->document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            assembly_->setTextCursor(QTextCursor(block));
            assembly_->ensureCursorVisible();
        }
    }
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
    Json selected;
    if (const auto item = rules_->currentItem()) selected = points_.at(item->data(0, Qt::UserRole).toULongLong());
    rules_->clear();
    points_ = model_->breakpoints();
    const auto instructionPoints = model_->instructionBreakpoints();
    for (const auto &id : instructionPoints) points_.push_back({{"instruction", id}});
    std::map<uint64_t, size_t> visits;
    if (!instructionPoints.empty())
        for (int64_t i = 0; i <= model_->position(); ++i)
            ++visits[model_->result().at("steps").at(size_t(i)).at("nextInstruction").get<uint64_t>()];
    for (size_t i = 0; i < points_.size(); ++i) {
        const auto &p = points_[i];
        if (p.contains("instruction")) {
            const auto id = p.at("instruction").get<uint64_t>();
            auto item = new QTreeWidgetItem(rules_, {"Instruction " + text(p.at("instruction")), "—", "always",
                QString::number(visits[id])});
            item->setData(0, Qt::UserRole, qulonglong(i));
            item->setToolTip(0, "Recorded instruction breakpoint; selecting it locates assembly without moving the trace");
            item->setToolTip(1, "Conditions and hit-count rules currently apply to source breakpoints only");
            if (p == selected) rules_->setCurrentItem(item);
            continue;
        }
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
        if (p == selected) rules_->setCurrentItem(item);
    }
    updateRuleActions();
}
void ReplayDebugView::updateRuleActions() {
    const auto item = rules_->currentItem();
    const bool instruction = item && points_.at(item->data(0, Qt::UserRole).toULongLong()).contains("instruction");
    const bool source = !busy_ && model_ && !model_->files().empty() && !instruction;
    condition_->setEnabled(source);
    hitMode_->setEnabled(source);
    count_->setEnabled(source);
    findChild<QAction *>("debugApplyRule")->setEnabled(source);
    findChild<QAction *>("debugRemoveRule")->setEnabled(!busy_ && model_ && item);
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
