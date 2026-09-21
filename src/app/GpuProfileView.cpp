#include "GpuProfileView.h"
#include "application/GpuProfile.h"
#include "application/ZipArchive.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace flora {
using Json = nlohmann::json;
class ProfileTimeline final : public QGraphicsView {
  public:
    std::function<void()> resized;
    std::function<void(Id)> picked;
    using QGraphicsView::QGraphicsView;
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (resized)
            resized();
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (auto item = itemAt(event->position().toPoint()); item && item->data(0).isValid() && picked)
            picked(item->data(0).toULongLong());
        QGraphicsView::mousePressEvent(event);
    }
};
GpuProfileView::GpuProfileView(QWidget *parent) : QWidget(parent) {
    setObjectName("gpuProfileView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    read_ = bar->addAction("Sample");
    read_->setObjectName("sampleGpuProfile");
    current_ = bar->addAction("Selection");
    current_->setObjectName("profileSelection");
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportGpuProfile");
    layout->addWidget(bar);
    auto settings = new QToolBar;
    auto field = [&](const char *name, const char *placeholder, const QString &value, int width) {
        auto edit = new QLineEdit(value);
        edit->setObjectName(name);
        edit->setPlaceholderText(placeholder);
        edit->setMaximumWidth(width);
        settings->addWidget(edit);
        return edit;
    };
    start_ = field("profileStart", "First API", {}, 125);
    end_ = field("profileEnd", "Last API", {}, 125);
    settings->addWidget(new QLabel(" Samples "));
    samples_ = field("profileSamples", "Samples", "5", 55);
    settings->addWidget(new QLabel(" Warmup "));
    warmup_ = field("profileWarmup", "Warmup", "1", 55);
    writes_ = new QCheckBox("Writes");
    writes_->setToolTip("Include resource-writing commands");
    writes_->setObjectName("profileWrites");
    settings->addWidget(writes_);
    layout->addWidget(settings);
    summary_ = new QLabel("No samples");
    summary_->setObjectName("profileSummary");
    summary_->setMargin(6);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    auto splitter = new QSplitter(Qt::Vertical);
    table_ = new QTreeWidget;
    table_->setObjectName("profileEvents");
    table_->setRootIsDecorated(false);
    table_->setUniformRowHeights(true);
    table_->setAlternatingRowColors(true);
    table_->setHeaderLabels(
        {"Event", "API", "Enabled", "Samples", "Median ms", "P95 ms", "Min ms", "Max ms"});
    table_->setColumnWidth(0, 80);
    table_->setColumnWidth(1, 160);
    for (int i = 2; i < 8; ++i)
        table_->setColumnWidth(i, 90);
    splitter->addWidget(table_);
    auto pane = new QWidget;
    auto bottom = new QVBoxLayout(pane);
    bottom->setContentsMargins(0, 0, 0, 0);
    auto timeBar = new QToolBar;
    timeBar->addWidget(new QLabel(" Pass "));
    pass_ = new QComboBox;
    pass_->setObjectName("profilePass");
    pass_->addItem("0");
    timeBar->addWidget(pass_);
    bottom->addWidget(timeBar);
    scene_ = new QGraphicsScene(this);
    timeline_ = new ProfileTimeline(scene_);
    timeline_->setObjectName("profileTimeline");
    timeline_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    timeline_->setMinimumHeight(170);
    bottom->addWidget(timeline_);
    splitter->addWidget(pane);
    layout->addWidget(splitter);
    timeline_->resized = [this] { draw(); };
    timeline_->picked = [this](Id id) {
        if (!busy_)
            emit eventRequested(id);
    };
    connect(pass_, &QComboBox::currentIndexChanged, this, [this] { draw(); });
    connect(table_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!busy_)
            emit eventRequested(item->data(0, Qt::UserRole).toULongLong());
    });
    connect(table_->header(), &QHeaderView::sectionClicked, this, [this](int col) {
        if (col == 0 || col == 4)
            populate(col == 4);
    });
    connect(current_, &QAction::triggered, this, [this] {
        if (!busy_ && selected_) {
            start_->setText(QString::number(selected_));
            end_->setText(QString::number(selected_));
        }
    });
    connect(read_, &QAction::triggered, this, &GpuProfileView::read);
    connect(export_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export GPU timings", "gpu-profile.zip",
                                                       "GPU timings (*.zip)");
        if (path.isEmpty())
            return;
        try {
            exportResult(path);
        } catch (const std::exception &e) {
            summary_->setText("Export failed");
            summary_->setToolTip(QString::fromUtf8(e.what()));
        }
    });
    updateActions();
}
void GpuProfileView::setContext(std::shared_ptr<const Frame> frame, Id selected, const QString &key) {
    if (frame_ != frame) {
        result_ = nullptr;
        table_->clear();
        scene_->clear();
        summary_->setText("No samples");
        restoreSettings(Json::object());
    }
    if (frame_ != frame || key_ != key)
        ++revision_;
    frame_ = std::move(frame);
    selected_ = selected;
    key_ = key;
    updateActions();
}
void GpuProfileView::restoreSettings(const Json &s) {
    if (!s.is_object())
        throw std::runtime_error("Invalid GPU profile settings");
    std::array<QString, 4> values;
    const std::array<const char *, 4> names{"start", "end", "samples", "warmup"};
    const std::array<const char *, 4> defaults{"", "", "5", "1"};
    for (size_t n = 0; n < 4; ++n) {
        auto value = s.value(names[n], Json(defaults[n]));
        if (!value.is_string())
            throw std::runtime_error("Invalid GPU profile setting text");
        values[n] = QString::fromStdString(value.get<std::string>());
    }
    auto writes = s.value("include_writes", Json(false));
    if (!writes.is_boolean())
        throw std::runtime_error("Invalid GPU profile writes setting");
    const std::array<QLineEdit *, 4> fields{start_, end_, samples_, warmup_};
    for (size_t n = 0; n < 4; ++n)
        fields[n]->setText(values[n]);
    writes_->setChecked(writes.get<bool>());
    result_ = nullptr;
    table_->clear();
    scene_->clear();
    pass_->clear();
    pass_->addItem("0");
    summary_->setText("No samples");
    summary_->setToolTip({});
    ++revision_;
    updateActions();
}
Json GpuProfileView::settings() const {
    return {{"start", start_->text().toStdString()},
            {"end", end_->text().toStdString()},
            {"samples", samples_->text().toStdString()},
            {"warmup", warmup_->text().toStdString()},
            {"include_writes", writes_->isChecked()}};
}
void GpuProfileView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void GpuProfileView::updateActions() {
    read_->setEnabled(bool(frame_) && !busy_);
    current_->setEnabled(bool(frame_) && selected_ && !busy_);
    export_->setEnabled(!result_.is_null() && !busy_);
    for (auto field : {start_, end_, samples_, warmup_})
        field->setEnabled(!busy_);
    writes_->setEnabled(!busy_);
}
void GpuProfileView::read() {
    if (!frame_ || busy_)
        return;
    try {
        Json request{{"include_writes", writes_->isChecked()}};
        for (const auto &[name, field] : std::array<std::pair<const char *, QLineEdit *>, 4>{
                 {{"start", start_}, {"end", end_}, {"samples", samples_}, {"warmup", warmup_}}}) {
            const auto text = field->text().trimmed();
            if (text.isEmpty() && (field == start_ || field == end_))
                continue;
            bool ok = false;
            const auto value = text.toULongLong(&ok);
            if (!ok || text.startsWith('-'))
                throw std::runtime_error("Enter unsigned profile parameters");
            request[name] = uint64_t(value);
        }
        gpuProfileSelection(*frame_, gpuProfileRequest(request));
        summary_->setText("Sampling…");
        emit readRequested(QString::fromStdString(request.dump()), ++revision_);
    } catch (const std::exception &e) {
        summary_->setText("Invalid range or sample count");
        summary_->setToolTip(QString::fromUtf8(e.what()));
    }
}
bool GpuProfileView::finish(uint64_t request, const Json &result) {
    if (request != revision_)
        return false;
    if (result.contains("error")) {
        summary_->setText("Sampling failed");
        summary_->setToolTip(QString::fromStdString(result.at("error").get<std::string>()));
        return false;
    }
    result_ = result;
    result_["experiment_key"] = key_.toStdString();
    pass_->clear();
    for (const auto &p : result_.at("passes"))
        pass_->addItem(QString::number(p.at("index").get<unsigned>()));
    pass_->setCurrentIndex(0);
    populate();
    draw();
    summary_->setText(
        QString("%1 · %2 events × %3 samples · %4")
            .arg(QString::fromStdString(result_.at("execution_device").at("selected").get<std::string>()))
            .arg(result_.at("events").size())
            .arg(result_.at("requested_samples").get<unsigned>())
            .arg(key_.left(8)));
    summary_->setToolTip(QString::fromStdString(result_.at("notes").dump(2)));
    updateActions();
    return true;
}
void GpuProfileView::populate(bool ranked) {
    table_->clear();
    if (result_.is_null())
        return;
    auto rows = result_.at("events").get<std::vector<Json>>();
    if (ranked)
        std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            if (a.at("median_ms").is_null())
                return false;
            if (b.at("median_ms").is_null())
                return true;
            return a.at("median_ms").template get<double>() > b.at("median_ms").template get<double>();
        });
    for (const auto &r : rows) {
        QStringList values{QString::number(r.at("event").get<Id>()),
                           QString::fromStdString(r.at("api").get<std::string>()),
                           r.at("enabled").get<bool>() ? "Yes" : "No",
                           QString("%1/%2")
                               .arg(r.at("valid_samples").get<unsigned>())
                               .arg(result_.at("requested_samples").get<unsigned>())};
        for (const auto *key : {"median_ms", "p95_ms", "min_ms", "max_ms"})
            values << (r.at(key).is_null() ? QString("—") : QString::number(r.at(key).get<double>(), 'f', 6));
        auto item = new QTreeWidgetItem(table_, values);
        item->setData(0, Qt::UserRole, QVariant::fromValue(qulonglong(r.at("event").get<Id>())));
        item->setToolTip(0, QString::fromStdString(r.dump(2)));
    }
}
void GpuProfileView::draw() {
    scene_->clear();
    if (result_.is_null() || pass_->currentIndex() < 0)
        return;
    const auto &p = result_.at("passes").at(size_t(pass_->currentIndex()));
    const auto &envelope = p.at("envelope");
    auto text = [&](const QString &s, double x, double y) {
        auto item = scene_->addSimpleText(s);
        item->setBrush(palette().text());
        item->setPos(x, y);
    };
    if (!envelope.at("available").get<bool>()) {
        text("Unavailable: " + QString::fromStdString(envelope.at("unavailable_reason").get<std::string>()),
             8, 8);
        return;
    }
    const double total = envelope.at("elapsed_ms").get<double>(),
                 width = std::max(1, timeline_->viewport()->width() - 180);
    text(QString("0 → %1 ms").arg(total, 0, 'f', 6), 145, 5);
    size_t n = 0;
    for (const auto &row : result_.at("events")) {
        const auto id = row.at("event").get<Id>();
        const double y = 30 + 22 * double(n++);
        text(QString::number(id) + ' ' + QString::fromStdString(row.at("api").get<std::string>()).left(15), 4,
             y - 6);
        const auto &v = p.at("events").at(std::to_string(id));
        if (!v.at("available").get<bool>())
            continue;
        const double x = 145 + width * v.at("offset_ms").get<double>() / std::max(total, 1e-12),
                     length =
                         std::max(1.0, width * v.at("elapsed_ms").get<double>() / std::max(total, 1e-12));
        auto item =
            scene_->addRect(x, y, length, 14, QPen(Qt::NoPen),
                            QBrush(row.at("enabled").get<bool>() ? QColor("#70caff") : QColor("#7b8792")));
        item->setData(0, QVariant::fromValue(qulonglong(id)));
        item->setToolTip(QString::fromStdString(v.dump(2)));
    }
    scene_->setSceneRect(0, 0, width + 170, 50 + 22 * double(n));
}
void GpuProfileView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No GPU timing results");
    QTemporaryDir directory;
    if (!directory.isValid())
        throw std::runtime_error("Cannot create export directory");
    exportGpuProfile(result_, directory.path());
    std::vector<ZipEntry> entries;
    for (const auto *name : {"profile.json", "profile.csv", "profile-samples.csv"})
        entries.push_back({name, directory.filePath(name), {}});
    entries.push_back({"result.json", {}, QByteArray::fromStdString(result_.dump(2))});
    if (result_.contains("loaded_modules"))
        entries.push_back(
            {"loaded_modules.json", {}, QByteArray::fromStdString(result_.at("loaded_modules").dump(2))});
    writeZipArchive(path, entries);
}
} // namespace flora
