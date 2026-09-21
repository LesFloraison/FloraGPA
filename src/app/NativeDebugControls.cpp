#include "NativeDebugControls.h"
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
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
QTreeWidget *table(const QString &name, const QStringList &columns, QWidget *parent) {
    auto tree = new QTreeWidget(parent);
    tree->setObjectName(name);
    tree->setHeaderLabels(columns);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(true);
    return tree;
}
} // namespace
NativeDebugControls::NativeDebugControls(QWidget *parent) : QWidget(parent) {
    setObjectName("nativeDebugControls");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto tools = new QHBoxLayout;
    layout->addLayout(tools);
    auto button = [&](QHBoxLayout *bar, const char *name, const char *label,
                      const std::function<void()> &action) {
        auto widget = new QPushButton(label, this);
        widget->setObjectName(name);
        bar->addWidget(widget);
        connect(widget, &QPushButton::clicked, this, [this, action] { invoke(action); });
        return widget;
    };
    button(tools, "debugImport", "Import…", [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Import debug configuration", {},
                                                       "Debug configuration (*.json)");
        if (!path.isEmpty())
            importConfiguration(readNativeDebugConfig(path.toStdWString()));
    });
    button(tools, "debugSave", "Save…", [this] {
        const auto config = configuration();
        const auto path = QFileDialog::getSaveFileName(this, "Save debug configuration", {},
                                                       "Debug configuration (*.json)");
        if (!path.isEmpty())
            writeNativeDebugConfig(path.toStdWString(), config);
    });
    tools->addStretch();
    auto split = new QSplitter(Qt::Vertical, this);
    layout->addWidget(split, 1);
    auto breakpointPane = new QWidget(split);
    auto breakpoints = new QVBoxLayout(breakpointPane);
    breakpoints->setContentsMargins(0, 0, 0, 0);
    auto ruleBar = new QHBoxLayout;
    breakpoints->addLayout(ruleBar);
    condition_ = new QLineEdit;
    condition_->setObjectName("debugCondition");
    condition_->setPlaceholderText("Breakpoint condition");
    condition_->setToolTip("Read-only expression at the selected source line");
    ruleBar->addWidget(condition_, 1);
    mode_ = new QComboBox;
    mode_->setObjectName("debugHitMode");
    for (const auto &[label, value] : {std::pair{"Always", "always"},
                                       {"Equal", "equal"},
                                       {"At least", "at_least"},
                                       {"Multiple", "multiple"}})
        mode_->addItem(label, value);
    ruleBar->addWidget(mode_);
    count_ = new QLineEdit("1");
    count_->setObjectName("debugHitCount");
    count_->setMaximumWidth(90);
    count_->setToolTip("Hit count: 1–4294967295");
    ruleBar->addWidget(count_);
    button(ruleBar, "debugApplyRule", "Set", [this] {
        bool valid = false;
        const auto count = count_->text().trimmed().toLongLong(&valid);
        if (!valid)
            throw std::runtime_error("Invalid breakpoint hit count");
        settings().source.setRule(point_.first, point_.second, condition_->text().toStdString(),
                                  mode_->currentData().toString().toStdString(), count);
        refresh();
        emit rulesChanged();
    })->setToolTip("Apply to the selected source line");
    button(ruleBar, "debugRemoveRule", "Remove", [this] {
        const auto item = rules_->currentItem();
        if (!item)
            throw std::runtime_error("Select a source breakpoint");
        const auto &point = points_.at(item->data(0, Qt::UserRole).toULongLong());
        settings().source.toggle(point.at("file"), point.at("line"));
        refresh();
        emit rulesChanged();
    });
    rules_ = table("debugRules", {"Source", "Condition", "Hit count"}, breakpointPane);
    rules_->setColumnWidth(0, 200);
    rules_->setColumnWidth(1, 160);
    breakpoints->addWidget(rules_, 1);
    connect(rules_, &QTreeWidget::itemSelectionChanged, this, [this] {
        invoke([&] {
            const auto item = rules_->currentItem();
            if (!item)
                return;
            const auto &point = points_.at(item->data(0, Qt::UserRole).toULongLong());
            setSourcePoint(point.at("file"), point.at("line"));
            condition_->setText(text(point.value("condition", Json(""))));
            const auto rule = point.value("hit_count", Json{{"mode", "always"}, {"count", 1}});
            mode_->setCurrentIndex(mode_->findData(text(rule.at("mode"))));
            count_->setText(text(rule.at("count")));
            emit sourcePointSelected(point_.first, point_.second);
        });
    });
    auto watchPane = new QWidget(split);
    auto watches = new QVBoxLayout(watchPane);
    watches->setContentsMargins(0, 0, 0, 0);
    auto watchBar = new QHBoxLayout;
    watches->addLayout(watchBar);
    watch_ = new QLineEdit;
    watch_->setObjectName("debugWatchExpression");
    watch_->setPlaceholderText("Watch expression");
    watchBar->addWidget(watch_, 1);
    auto add = button(watchBar, "debugAddWatch", "Add", [this] {
        auto &watches = settings().watches;
        if (watches.size() >= 64)
            throw std::runtime_error("At most 64 watch expressions are supported");
        DebugExpression expression(watch_->text().toStdString());
        for (const auto &old : watches)
            if (old.text() == expression.text())
                throw std::runtime_error("Duplicate watch expression");
        watches.push_back(std::move(expression));
        watch_->clear();
        updateWatches();
    });
    connect(watch_, &QLineEdit::returnPressed, add, &QPushButton::click);
    button(watchBar, "debugRemoveWatch", "Remove", [this] {
        const auto item = values_->currentItem();
        if (!item)
            throw std::runtime_error("Select a watch expression");
        auto &watches = settings().watches;
        watches.erase(watches.begin() + item->data(0, Qt::UserRole).toULongLong());
        updateWatches();
    });
    values_ = table("debugWatchValues", {"Expression", "Type", "Value / Status"}, watchPane);
    values_->setColumnWidth(0, 200);
    values_->setColumnWidth(1, 65);
    watches->addWidget(values_, 1);
    split->setSizes({180, 220});
    setEnabled(false);
}
void NativeDebugControls::invoke(const std::function<void()> &action) {
    try {
        action();
    } catch (const std::exception &e) {
        emit error(QString::fromUtf8(e.what()));
    }
}
NativeDebugSettings &NativeDebugControls::settings() {
    if (!settings_)
        throw std::runtime_error("Read the shader instruction catalog first");
    return *settings_;
}
void NativeDebugControls::reset(Json result, Json rows, SourceTrace::ValueLoader loader, bool retain) {
    auto replacement = retain && settings_ ? prepareNativeDebugConfig(result, rows, loader, configuration())
                                           : NativeDebugSettings{SourceTrace(result, rows, loader), {}, {}};
    result_ = std::move(result);
    rows_ = std::move(rows);
    loader_ = std::move(loader);
    settings_ = std::make_unique<NativeDebugSettings>(std::move(replacement));
    record_.reset();
    frame_.reset();
    point_ = {-1, 0};
    condition_->clear();
    count_->setText("1");
    mode_->setCurrentIndex(0);
    watch_->clear();
    setEnabled(true);
    refresh();
}
NativeDebugControls::Json NativeDebugControls::configuration() const {
    if (!settings_)
        throw std::runtime_error("Read the shader instruction catalog first");
    return exportNativeDebugConfig(result_, *settings_);
}
void NativeDebugControls::importConfiguration(const Json &config) {
    auto replacement = prepareNativeDebugConfig(result_, rows_, loader_, config);
    settings_ = std::make_unique<NativeDebugSettings>(std::move(replacement));
    refresh();
    emit rulesChanged();
}
void NativeDebugControls::setSourcePoint(int64_t file, int64_t line) { point_ = {file, line}; }
void NativeDebugControls::setRecord(std::optional<size_t> record, std::optional<std::string> frame) {
    record_ = record;
    frame_ = std::move(frame);
    updateWatches();
}
void NativeDebugControls::refresh() {
    const QSignalBlocker block(rules_);
    rules_->clear();
    points_ = settings().source.exportBreakpoints();
    const auto files = result_.value("source_lines", Json::object()).value("files", Json::array());
    for (size_t i = 0; i < points_.size(); ++i) {
        const auto &point = points_.at(i);
        const auto file = point.at("file").get<size_t>();
        const auto location =
            (file < files.size() ? text(files.at(file).at("name")) : QString::number(file)) + ":" +
            text(point.at("line"));
        const auto rule = point.value("hit_count", Json{{"mode", "always"}, {"count", 1}});
        const auto hit =
            rule.at("mode") == "always"
                ? QString("Always")
                : mode_->itemText(mode_->findData(text(rule.at("mode")))) + " " + text(rule.at("count"));
        auto item = new QTreeWidgetItem(rules_, {location, text(point.value("condition", Json(""))), hit});
        item->setData(0, Qt::UserRole, qulonglong(i));
        for (int c = 0; c < 3; ++c)
            item->setToolTip(c, item->text(c));
    }
    updateWatches();
}
void NativeDebugControls::updateWatches() {
    values_->clear();
    watchResults_ = Json::array();
    if (!settings_)
        return;
    std::optional<DebugEnvironment> environment;
    std::string issue = "No selected snapshot";
    if (record_ && !settings_->watches.empty()) {
        try {
            environment.emplace(settings_->source.environment(*record_, frame_));
        } catch (const std::exception &e) {
            issue = e.what();
        }
    }
    const bool sdbg =
        result_.value("source_variables", Json::object()).value("format", "") == "SDBG assignments";
    for (size_t i = 0; i < settings_->watches.size(); ++i) {
        const auto &watch = settings_->watches[i];
        Json row{{"expression", watch.text()}, {"status", "unavailable"}, {"type", ""}, {"text", issue}};
        if (sdbg)
            row["basis"] = "sdbg_assignment_history";
        if (environment) {
            try {
                const auto value = environment->expression(watch);
                row.update({{"status", "available"}, {"type", value.kind}, {"text", value.text()}});
            } catch (const ExpressionError &e) {
                row["text"] = e.what();
            }
        }
        watchResults_.push_back(row);
        auto item = new QTreeWidgetItem(
            values_, {text(row.at("expression")), text(row.at("type")), text(row.at("text"))});
        item->setData(0, Qt::UserRole, qulonglong(i));
        for (int c = 0; c < 3; ++c)
            item->setToolTip(c, item->text(c));
    }
}
} // namespace flora
