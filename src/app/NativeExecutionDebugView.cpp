#include "NativeExecutionDebugView.h"
#include "NativeDebugControls.h"
#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScopedValueRollback>
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
QString text(const Json &v) {
    return v.is_null() ? QString("Unknown")
                       : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
Json triple(const QString &value) {
    const auto parts = value.split(',');
    if (parts.size() != 3)
        throw std::runtime_error("Enter three unsigned coordinates separated by commas");
    Json result = Json::array();
    for (const auto &part : parts) {
        bool valid = false;
        const auto n = part.trimmed().toULongLong(&valid);
        if (!valid || n > UINT32_MAX)
            throw std::runtime_error("Invalid compute coordinate");
        result.push_back(n);
    }
    return result;
}
QTreeWidget *table(const char *name, const QStringList &columns) {
    auto tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setHeaderLabels(columns);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    return tree;
}
} // namespace
NativeExecutionDebugView::NativeExecutionDebugView(QWidget *parent) : QWidget(parent) {
    setObjectName("nativeExecutionDebugView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    auto action = [this](QToolBar *toolbar, const char *name, const char *label,
                         const std::function<void()> &fn) {
        auto item = toolbar->addAction(label);
        item->setObjectName(name);
        connect(item, &QAction::triggered, this, [this, fn] { invoke(fn); });
        return item;
    };
    read_ = action(bar, "nativeDebugRead", "Debug", [this] {
        if (busy_ || !event_ || key_.isEmpty())
            return;
        request();
        invalidate();
        summary_->setText("Reading native execution…");
        emit readRequested();
    });
    cancel_ = action(bar, "nativeDebugCancel", "Cancel", [this] {
        if (operationActive())
            cancelOperation();
        else
            emit cancelRequested();
    });
    export_ = action(bar, "nativeDebugExport", "Export session…", [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export native debug session",
                                                       "debug-session.zip", "Debug session (*.zip)");
        if (!path.isEmpty())
            startOperation(NativeDebugCommand::Position, model()->position(), path);
    });
    auto select = new QToolBar;
    layout->addWidget(select);
    stage_ = new QComboBox;
    stage_->setObjectName("nativeDebugStage");
    stage_->addItems({"CS", "VS", "PS (private primitive)"});
    select->addWidget(stage_);
    auto firstLabel = new QLabel("Group ");
    select->addWidget(firstLabel);
    group_ = new QLineEdit("0,0,0");
    group_->setObjectName("nativeDebugGroup");
    select->addWidget(group_);
    auto secondLabel = new QLabel(" Thread ");
    select->addWidget(secondLabel);
    thread_ = new QLineEdit("0,0,0");
    thread_->setObjectName("nativeDebugThread");
    select->addWidget(thread_);
    auto indexLabel = new QLabel(" Actual index ");
    select->addWidget(indexLabel);
    index_ = new QLineEdit;
    index_->setObjectName("nativeDebugIndex");
    index_->setPlaceholderText("From draw");
    index_->setToolTip("Override the per-vertex attribute index after all draw offsets; system IDs remain "
                       "selected from the draw");
    select->addWidget(index_);
    indexLabel->hide();
    index_->hide();
    auto subresourceLabel = new QLabel(" Mip, layer, slice ");
    select->addWidget(subresourceLabel);
    pixelSubresource_ = new QLineEdit("0,0,0");
    pixelSubresource_->setObjectName("nativeDebugPixelSubresource");
    select->addWidget(pixelSubresource_);
    subresourceLabel->hide();
    pixelSubresource_->hide();
    connect(stage_, &QComboBox::currentIndexChanged, this,
            [this, firstLabel, secondLabel, indexLabel, subresourceLabel](int index) {
                const bool pixel = index == 2;
                firstLabel->setText(pixel ? "Pixel x, y, sample " : index ? "Input reference " : "Group ");
                secondLabel->setText(pixel ? " Primitive ordinal " : index ? " Instance " : " Thread ");
                indexLabel->setText(pixel ? " GPA resource " : " Actual index ");
                group_->setText(index == 1 ? "0" : "0,0,0");
                thread_->setText(index ? "0" : "0,0,0");
                index_->clear();
                index_->setPlaceholderText(pixel ? "Required" : "From draw");
                index_->setToolTip(pixel ? "Bound RTV or DSV resource ID for private primitive analysis"
                                         : "Override the per-vertex attribute index after all draw offsets; "
                                           "system IDs remain selected from the draw");
                indexLabel->setVisible(index != 0);
                index_->setVisible(index != 0);
                subresourceLabel->setVisible(pixel);
                pixelSubresource_->setVisible(pixel);
                pixelSubresource_->setText("0,0,0");
                invalidate();
            });
    steps_ = new QToolBar;
    layout->addWidget(steps_);
    navigation_ = new QComboBox;
    navigation_->setObjectName("nativeDebugNavigation");
    navigation_->addItems({"Instruction", "Source"});
    steps_->addWidget(navigation_);
    action(steps_, "nativeDebugPrevious", "Previous", [this] {
        if (!model() || model()->position() == 0)
            return;
        if (navigation_->currentIndex() == 0 || model()->position() == model()->size())
            move(model()->position() - 1);
        else
            startOperation(NativeDebugCommand::PreviousSource, model()->position());
    });
    action(steps_, "nativeDebugNext", "Step", [this] {
        if (!model() || model()->position() == model()->size())
            return;
        if (navigation_->currentIndex() == 0)
            move(model()->position() + 1);
        else
            startOperation(NativeDebugCommand::NextSource, model()->position());
    });
    action(steps_, "nativeDebugOver", "Over", [this] {
        if (model())
            startOperation(NativeDebugCommand::Over, model()->position());
    });
    action(steps_, "nativeDebugOut", "Out", [this] {
        if (model())
            startOperation(NativeDebugCommand::Out, model()->position());
    });
    action(steps_, "nativeDebugBreakpoint", "Breakpoint", [this] {
        if (!model())
            return;
        if (navigation_->currentIndex() == 1) {
            controls_->settings().source.toggle(files_->currentIndex(), line_->value());
        } else {
            if (model()->position() == model()->size())
                throw std::runtime_error("Terminal state has no instruction breakpoint");
            auto &breakpoints = controls_->settings().instructions;
            const auto instruction = snapshot_.at("state").at("instruction").get<uint64_t>();
            if (!breakpoints.erase(instruction))
                breakpoints.insert(instruction);
        }
        controls_->refresh();
        render();
    });
    action(steps_, "nativeDebugContinue", "Continue", [this] {
        if (!model())
            return;
        startOperation(NativeDebugCommand::Continue, model()->position());
    });
    position_ = new QSpinBox;
    position_->setObjectName("nativeDebugPosition");
    position_->setPrefix("Step ");
    steps_->addWidget(position_);
    summary_ = new QLabel("Select a Dispatch event");
    summary_->setObjectName("nativeDebugSummary");
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    auto split = new QSplitter;
    layout->addWidget(split, 1);
    auto codeTabs = new QTabWidget(split);
    auto sourcePage = new QWidget;
    auto sourceLayout = new QVBoxLayout(sourcePage);
    auto sourceBar = new QHBoxLayout;
    sourceLayout->addLayout(sourceBar);
    files_ = new QComboBox;
    files_->setObjectName("nativeDebugFiles");
    sourceBar->addWidget(files_, 1);
    line_ = new QSpinBox;
    line_->setObjectName("nativeDebugLine");
    line_->setRange(1, INT_MAX);
    sourceBar->addWidget(line_);
    source_ = new QPlainTextEdit;
    source_->setObjectName("nativeDebugSource");
    source_->setReadOnly(true);
    sourceLayout->addWidget(source_, 1);
    codeTabs->addTab(sourcePage, "Source");
    assembly_ = new QPlainTextEdit;
    assembly_->setObjectName("nativeDebugAssembly");
    assembly_->setReadOnly(true);
    codeTabs->addTab(assembly_, "Assembly");
    auto values = new QTabWidget(split);
    auto registerPage = new QWidget;
    auto registerLayout = new QVBoxLayout(registerPage);
    interpretation_ = new QComboBox;
    interpretation_->setObjectName("nativeDebugInterpretation");
    interpretation_->addItems({"float", "double", "int", "uint", "hex"});
    interpretation_->setToolTip(
        "Double interpretation uses the xy and zw word pairs; both words must be known.");
    registerLayout->addWidget(interpretation_);
    registers_ = table("nativeDebugRegisters", {"Register", "Value", "Raw 32-bit components", "Valid XYZW"});
    registers_->setColumnWidth(0, 170);
    registers_->setColumnWidth(1, 230);
    registers_->setColumnWidth(2, 360);
    registerLayout->addWidget(registers_, 1);
    values->addTab(registerPage, "Registers");
    locals_ = table("nativeDebugLocals", {"Name", "Type", "Value / Status"});
    values->addTab(locals_, "Locals");
    stack_ = new QPlainTextEdit;
    stack_->setReadOnly(true);
    stack_->setObjectName("nativeDebugStack");
    values->addTab(stack_, "Call stack");
    controls_ = new NativeDebugControls;
    values->addTab(controls_, "Breakpoints / Watches");
    connect(controls_, &NativeDebugControls::error, this, &NativeExecutionDebugView::error);
    connect(controls_, &NativeDebugControls::sourcePointSelected, this,
            [this](qlonglong file, qlonglong line) {
                files_->setCurrentIndex(int(file));
                line_->setValue(int(line));
            });
    connect(position_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!rendering_)
            invoke([&] { move(size_t(value)); });
    });
    connect(interpretation_, &QComboBox::currentIndexChanged, this, [this] {
        if (model())
            invoke([&] { move(model()->position()); });
    });
    connect(files_, &QComboBox::currentIndexChanged, this, [this] {
        if (!rendering_)
            showSource();
    });
    connect(line_, &QSpinBox::valueChanged, this, [this] {
        if (!rendering_)
            showSource();
    });
    connect(group_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(thread_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(index_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(pixelSubresource_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(&operation_, &NativeDebugOperationController::finished, this,
            [this](const NativeDebugOperationOutcome &result) {
                const bool current = operationRequest_ == requestId_ && bool(session_);
                bool accepted = false;
                try {
                    if (current) {
                        if (result.error.isEmpty()) {
                            snapshot_ = result.snapshot;
                            render();
                            if (!result.exported.isEmpty())
                                summary_->setText("Session exported: " + result.exported);
                            accepted = true;
                        } else {
                            // The previous snapshot remains visible and the operation can be retried.
                            const QSignalBlocker block(position_);
                            const QSignalBlocker interpretationBlock(interpretation_);
                            position_->setValue(snapshot_.at("position").get<int>());
                            interpretation_->setCurrentText(text(snapshot_.at("interpretation")));
                            summary_->setText(result.error);
                            if (!result.cancelled)
                                emit error(result.error);
                        }
                    }
                } catch (const std::exception &e) {
                    emit error(QString::fromUtf8(e.what()));
                }
                updateActions();
                emit activityChanged(false);
                emit operationFinished(accepted);
            });
    updateActions();
}
NativeExecutionDebugView::~NativeExecutionDebugView() {
    operation_.disconnect(this);
    operation_.cancel();
}
void NativeExecutionDebugView::invoke(const std::function<void()> &fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        emit error(QString::fromUtf8(e.what()));
    }
}
void NativeExecutionDebugView::setContext(const QString &key, qulonglong event) {
    if (key_ == key && event_ == event)
        return;
    key_ = key;
    event_ = event;
    invalidate();
}
void NativeExecutionDebugView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void NativeExecutionDebugView::invalidate() {
    ++requestId_;
    operation_.cancel();
    controls_->clear();
    session_.reset();
    snapshot_ = Json();
    const QScopedValueRollback guard(rendering_, true);
    registers_->clear();
    locals_->clear();
    files_->clear();
    source_->clear();
    assembly_->clear();
    stack_->clear();
    position_->setRange(0, 0);
    summary_->setText(event_ ? "Ready to inspect the selected invocation"
                             : "Select a Draw or Dispatch event");
    updateActions();
}
void NativeExecutionDebugView::updateActions() {
    const bool occupied = busy_ || operationActive();
    read_->setEnabled(!occupied && event_ && !key_.isEmpty());
    cancel_->setEnabled(occupied);
    export_->setEnabled(!occupied && bool(session_));
    steps_->setEnabled(!occupied && bool(session_));
    controls_->setEnabled(!occupied && bool(session_));
    interpretation_->setEnabled(!occupied);
    files_->setEnabled(!occupied && bool(session_));
    line_->setEnabled(!occupied && bool(session_));
    group_->setEnabled(!occupied);
    thread_->setEnabled(!occupied);
    index_->setEnabled(!occupied);
    pixelSubresource_->setEnabled(!occupied);
    stage_->setEnabled(!occupied);
}
std::string NativeExecutionDebugView::action() const {
    return stage_->currentIndex() == 2 ? "debug-pixel"
           : stage_->currentIndex()    ? "debug-vertex"
                                       : "debug-thread";
}
NativeExecutionDebugView::Json NativeExecutionDebugView::request() const {
    if (stage_->currentIndex() == 2) {
        const auto location = triple(group_->text());
        const auto subresource = triple(pixelSubresource_->text());
        const auto scalar = [](const QString &text, uint64_t maximum) {
            bool valid = false;
            const auto value = text.toULongLong(&valid);
            if (!valid || value > maximum || text != text.trimmed() || text.startsWith('+') ||
                text.startsWith('-'))
                throw std::runtime_error("Enter a valid unsigned primitive ordinal or GPA resource ID");
            return value;
        };
        const auto resource = scalar(index_->text(), UINT64_MAX);
        if (!resource)
            throw std::runtime_error("Private pixel debugging requires a nonzero GPA resource ID");
        return {{"gpa_event", event_},
                {"debug_scope", "private-primitive"},
                {"primitive", scalar(thread_->text(), UINT32_MAX)},
                {"resource", resource},
                {"x", location[0]},
                {"y", location[1]},
                {"sample", location[2]},
                {"mip", subresource[0]},
                {"layer", subresource[1]},
                {"slice", subresource[2]}};
    }
    if (stage_->currentIndex()) {
        const auto scalar = [](const QString &value) {
            bool valid = false;
            const auto n = value.toULongLong(&valid);
            if (!valid || n > UINT32_MAX || value != value.trimmed() || value.startsWith('+') ||
                value.startsWith('-'))
                throw std::runtime_error("Enter an unsigned 32-bit reference, instance or actual index");
            return n;
        };
        Json selection = {
            {"gpa_event", event_}, {"vertex", scalar(group_->text())}, {"instance", scalar(thread_->text())}};
        if (!index_->text().isEmpty())
            selection["index"] = scalar(index_->text());
        return selection;
    }
    return {{"gpa_event", event_}, {"group", triple(group_->text())}, {"thread", triple(thread_->text())}};
}
bool NativeExecutionDebugView::finish(uint64_t id, std::shared_ptr<NativeDebugSession> session,
                                      const QString &errorText) {
    if (id != requestId_)
        return false;
    if (!errorText.isEmpty()) {
        summary_->setText(errorText);
        emit inspectionFinished(false);
        return false;
    }
    if (!session)
        throw std::runtime_error("Native analysis returned no debug session");
    if (operationActive())
        throw std::runtime_error("Wait for the previous native debug operation to finish");
    const auto selection = request();
    if (session->job().at("action") != action())
        throw std::runtime_error("Native debug stage changed");
    if (stage_->currentIndex() == 1 &&
        session->job().at("options").value("index", Json(nullptr)) != selection.value("index", Json(nullptr)))
        throw std::runtime_error("Native debug index override changed");
    for (const auto &[key, value] : selection.items())
        if (session->job().at("options").value(key, key == "gpa_event"       ? Json(nullptr)
                                                    : stage_->currentIndex() ? Json(0)
                                                                             : Json({0, 0, 0})) != value)
            throw std::runtime_error("Native debug selection changed");
    session_ = std::move(session);
    snapshot_ = session_->initialSnapshot();
    controls_->reset(model()->result(), model()->rows(), [this](size_t i) {
        if (snapshot_.is_null() || snapshot_.at("position") != i)
            throw std::runtime_error("Native source values are not loaded at this position");
        return snapshot_.at("source_values");
    });
    {
        const QScopedValueRollback guard(rendering_, true);
        files_->clear();
        for (const auto &file : model()->files())
            files_->addItem(text(file.at("name")));
        assembly_->setPlainText(text(model()->result().at("disassembly")));
        position_->setRange(0, int(model()->size()));
    }
    render();
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void NativeExecutionDebugView::move(size_t position) {
    startOperation(NativeDebugCommand::Position, position);
}
void NativeExecutionDebugView::startOperation(NativeDebugCommand command, size_t position,
                                              const QString &archive) {
    if (operationActive())
        throw std::runtime_error("A native debug operation is already running");
    if (!session_)
        return;
    operationRequest_ = requestId_;
    operation_.start(session_, command, position, controls_->configuration(),
                     interpretation_->currentText().toStdString(), archive);
    updateActions();
    emit activityChanged(true);
}
void NativeExecutionDebugView::render() {
    if (!model())
        return;
    const QScopedValueRollback guard(rendering_, true);
    const auto state = snapshot_.at("state");
    const auto position = snapshot_.at("position").get<size_t>();
    const bool terminal = position == model()->size();
    position_->setValue(int(position));
    summary_->setText(terminal ? "Terminal invocation state · no source instruction"
                               : QString("Step %1 / %2 · before DXBC instruction %3 · global step %4%5")
                                     .arg(position)
                                     .arg(model()->size())
                                     .arg(text(state.at("instruction")))
                                     .arg(text(state.at("global_step")))
                                     .arg(controls_->settings().instructions.contains(
                                              state.at("instruction").get<uint64_t>())
                                              ? " · breakpoint"
                                              : ""));
    if (session_->job().at("action") == "debug-pixel")
        summary_->setText("Private primitive replay · original fragment eligibility unknown · " +
                          summary_->text());
    registers_->clear();
    const auto &raw = snapshot_.at("registers");
    const auto &rows = snapshot_.at("register_rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        QStringList words;
        QString validity;
        for (size_t component = 0; component < 4; ++component) {
            words.push_back(
                "0x" +
                QString::number(raw[i].at("bits")[component].get<uint32_t>(), 16).rightJustified(8, '0'));
            validity += raw[i].at("written")[component].get<bool>() ? '1' : '0';
        }
        auto item =
            new QTreeWidgetItem(registers_, {text(rows[i][0]), text(rows[i][1]), words.join(", "), validity});
        for (int column = 0; column < 4; ++column)
            item->setToolTip(column, item->text(column));
    }
    locals_->clear();
    for (const auto &v : snapshot_.at("source_values"))
        new QTreeWidgetItem(
            locals_, {text(v.value("name", Json())), text(v.value("type", Json())),
                      text(v.value("value", Json()).is_null() ? v.value("status", Json()) : v.at("value"))});
    stack_->setPlainText(QString::fromStdString(snapshot_.at("source_stack").dump(2)) + "\n\n" +
                         QString::fromStdString(state.dump(2)));
    const auto location = snapshot_.at("location");
    if (!location.is_null()) {
        files_->setCurrentIndex(location.at("file"));
        line_->setValue(location.at("line_start"));
    }
    showSource();
    controls_->setRecord(terminal ? std::nullopt : std::optional<size_t>(position));
}
void NativeExecutionDebugView::showSource() {
    if (!model() || files_->currentIndex() < 0) {
        source_->setPlainText("Source unavailable");
        return;
    }
    const auto &file = model()->files().at(size_t(files_->currentIndex()));
    const auto contents = text(file.at("text"));
    if (source_->toPlainText() != contents)
        source_->setPlainText(contents);
    controls_->setSourcePoint(files_->currentIndex(), line_->value());
    const auto block = source_->document()->findBlockByNumber(line_->value() - 1);
    if (block.isValid()) {
        QTextCursor cursor(block);
        cursor.select(QTextCursor::LineUnderCursor);
        source_->setTextCursor(cursor);
        source_->centerCursor();
    }
}
NativeExecutionDebugView::Json NativeExecutionDebugView::exportReport() {
    if (!model())
        throw std::runtime_error("Read a native execution trace before exporting");
    model()->importConfiguration(controls_->configuration());
    auto report = model()->exportReport();
    report["analysis_context"] = session_->result().at("context");
    report["analysis_context_id"] = session_->result().at("context_id");
    report["invocation"] = session_->result().at("data").at("invocation");
    return report;
}
void NativeExecutionDebugView::exportResult(const QString &path) {
    const auto bytes = QByteArray::fromStdString(exportReport().dump(2));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot export native debug state");
}
void NativeExecutionDebugView::exportArchive(const QString &path) {
    if (!model())
        throw std::runtime_error("Read an idle native trace before exporting");
    startOperation(NativeDebugCommand::Position, model()->position(), path);
}
} // namespace flora
