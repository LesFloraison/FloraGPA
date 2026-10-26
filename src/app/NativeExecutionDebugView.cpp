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
        summary_->setText("Reading compute execution…");
        emit readRequested();
    });
    cancel_ = action(bar, "nativeDebugCancel", "Cancel", [this] { emit cancelRequested(); });
    export_ = action(bar, "nativeDebugExport", "Export session…", [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export native debug session",
                                                       "debug-session.zip", "Debug session (*.zip)");
        if (!path.isEmpty())
            exportArchive(path);
    });
    auto select = new QToolBar;
    layout->addWidget(select);
    select->addWidget(new QLabel("Group "));
    group_ = new QLineEdit("0,0,0");
    group_->setObjectName("nativeDebugGroup");
    select->addWidget(group_);
    select->addWidget(new QLabel(" Thread "));
    thread_ = new QLineEdit("0,0,0");
    thread_->setObjectName("nativeDebugThread");
    select->addWidget(thread_);
    steps_ = new QToolBar;
    layout->addWidget(steps_);
    navigation_ = new QComboBox;
    navigation_->setObjectName("nativeDebugNavigation");
    navigation_->addItems({"Instruction", "Source"});
    steps_->addWidget(navigation_);
    action(steps_, "nativeDebugPrevious", "Previous", [this] {
        if (!model() || model()->position() == 0)
            return;
        move(navigation_->currentIndex() == 0 || model()->position() == model()->size()
                 ? model()->position() - 1
                 : model()->source().next(model()->position(), -1));
    });
    action(steps_, "nativeDebugNext", "Step", [this] {
        if (!model() || model()->position() == model()->size())
            return;
        move(navigation_->currentIndex() == 0 ? model()->position() + 1
                                              : model()->source().next(model()->position(), 1));
    });
    action(steps_, "nativeDebugOver", "Over", [this] {
        if (model())
            move(model()->source().functionStep(model()->position()));
    });
    action(steps_, "nativeDebugOut", "Out", [this] {
        if (model())
            move(model()->source().functionStep(model()->position(), true));
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
            const auto instruction = model()->instructionState().at("instruction").get<uint64_t>();
            if (!breakpoints.erase(instruction))
                breakpoints.insert(instruction);
        }
        controls_->refresh();
        render();
    });
    action(steps_, "nativeDebugContinue", "Continue", [this] {
        if (!model())
            return;
        model()->importConfiguration(controls_->configuration());
        move(model()->seek(model()->position()));
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
    interpretation_->addItems({"float", "int", "uint", "hex"});
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
    connect(interpretation_, &QComboBox::currentIndexChanged, this, [this] { invoke([&] { render(); }); });
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
    updateActions();
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
    controls_->clear();
    session_.reset();
    const QScopedValueRollback guard(rendering_, true);
    registers_->clear();
    locals_->clear();
    files_->clear();
    source_->clear();
    assembly_->clear();
    stack_->clear();
    position_->setRange(0, 0);
    summary_->setText(event_ ? "Ready to inspect the selected compute invocation"
                             : "Select a Dispatch event");
    updateActions();
}
void NativeExecutionDebugView::updateActions() {
    read_->setEnabled(!busy_ && event_ && !key_.isEmpty());
    cancel_->setEnabled(busy_);
    export_->setEnabled(!busy_ && bool(session_));
    steps_->setEnabled(!busy_ && bool(session_));
    controls_->setEnabled(!busy_ && bool(session_));
    group_->setEnabled(!busy_);
    thread_->setEnabled(!busy_);
}
NativeExecutionDebugView::Json NativeExecutionDebugView::request() const {
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
        throw std::runtime_error("Native compute analysis returned no debug session");
    const auto selection = request();
    for (const auto &[key, value] : selection.items())
        if (session->job().at("options").value(key, key == "gpa_event" ? Json(nullptr) : Json({0, 0, 0})) !=
            value)
            throw std::runtime_error("Native debug selection changed");
    session_ = std::move(session);
    const std::weak_ptr<NativeDebugSession> weak = session_;
    controls_->reset(model()->result(), model()->rows(), [weak](size_t i) {
        const auto retained = weak.lock();
        if (!retained)
            throw std::runtime_error("Native debug session expired");
        return retained->model().sourceValuesAt(i);
    });
    {
        const QScopedValueRollback guard(rendering_, true);
        files_->clear();
        for (const auto &file : model()->files())
            files_->addItem(text(file.at("name")));
        assembly_->setPlainText(text(model()->result().at("disassembly")));
        position_->setRange(0, int(model()->size()));
    }
    move(0);
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void NativeExecutionDebugView::move(size_t position) {
    if (!model())
        return;
    model()->move(position);
    render();
}
void NativeExecutionDebugView::render() {
    if (!model())
        return;
    const QScopedValueRollback guard(rendering_, true);
    const auto state = model()->instructionState();
    const bool terminal = model()->position() == model()->size();
    position_->setValue(int(model()->position()));
    summary_->setText(terminal ? "Terminal group state · no source instruction"
                               : QString("Step %1 / %2 · before DXBC instruction %3 · global step %4%5")
                                     .arg(model()->position())
                                     .arg(model()->size())
                                     .arg(text(state.at("instruction")))
                                     .arg(text(state.at("global_step")))
                                     .arg(controls_->settings().instructions.contains(
                                              state.at("instruction").get<uint64_t>())
                                              ? " · breakpoint"
                                              : ""));
    registers_->clear();
    const auto raw = model()->registerValues();
    const auto rows = model()->registerRows(interpretation_->currentText().toStdString());
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
    for (const auto &v : model()->sourceValues())
        new QTreeWidgetItem(
            locals_, {text(v.value("name", Json())), text(v.value("type", Json())),
                      text(v.value("value", Json()).is_null() ? v.value("status", Json()) : v.at("value"))});
    stack_->setPlainText(QString::fromStdString(model()->stack().dump(2)) + "\n\n" +
                         QString::fromStdString(state.dump(2)));
    const auto location = model()->location();
    if (!location.is_null()) {
        files_->setCurrentIndex(location.at("file"));
        line_->setValue(location.at("line_start"));
    }
    showSource();
    controls_->setRecord(terminal ? std::nullopt : std::optional<size_t>(model()->position()));
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
        throw std::runtime_error("Read a native compute trace before exporting");
    model()->importConfiguration(controls_->configuration());
    auto report = model()->exportReport();
    report["analysis_context"] = session_->result().at("context");
    report["analysis_context_id"] = session_->result().at("context_id");
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
    const auto state = exportReport();
    session_->exportArchive(path, state);
}
} // namespace flora
