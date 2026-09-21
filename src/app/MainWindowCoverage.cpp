#include "CoverageView.h"
#include "MainWindow.h"
#include "PixelHistoryView.h"
#include "ReplayDebugView.h"
#include <QAction>
#include <QStatusBar>
#include <QTabWidget>
namespace flora {
using Json = nlohmann::json;
void MainWindow::buildCoverageUi() {
    coverage_ = new CoverageView;
    centerTabs_->insertTab(1, coverage_, "Coverage");
    connect(coverage_, &CoverageView::readRequested, this, [this](const QString &text, qulonglong serial) {
        if (busy()) {
            coverage_->finish(serial, {{"error", "Worker is busy"}});
            return;
        }
        runningCoverageRequest_ = serial;
        const auto request = Json::parse(text.toStdString());
        QStringList args{"coverage",
                         capturePath_,
                         "--id",
                         QString::number(request.at("event").get<Id>()),
                         "--coverage-mode",
                         QString::fromStdString(request.at("mode").get<std::string>()),
                         "--coverage-target",
                         QString::fromStdString(request.at("target").get<std::string>())};
        if (request.contains("layer"))
            args << "--coverage-layer" << QString::number(request.at("layer").get<uint32_t>());
        if (!request.at("depth_test").get<bool>())
            args << "--ignore-depth";
        startWorker(args, false);
        if (process_.state() == QProcess::NotRunning)
            coverage_->finish(serial, {{"error", "Cannot start coverage"}});
    });
    connect(coverage_, &CoverageView::cancelRequested, this, &MainWindow::cancel);
    connect(coverage_, &CoverageView::pixelRequested, this, &MainWindow::selectCoveragePixel);
}
void MainWindow::updateCoverageContext(const QString &experimentKey) {
    std::optional<State> state;
    QString error;
    try {
        if (frame_ && selectedEvent_) {
            const auto &entry = frame_->entry(selectedEvent_);
            if (entry.category == 7 && entry.type >= 0x37 && entry.type <= 0x3d) {
                ReplayOptions options;
                if (experiment_)
                    experiment_->apply(*frame_, options);
                state = effectiveBindings(*frame_, selectedEvent_,
                                          frame_->state(frame_->event(selectedEvent_).state), options);
            }
        }
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
    }
    coverage_->setContext(frame_, selectedEvent_,
                          experimentKey + ":" + QString::number(adapter_->currentIndex()), state, error);
}
void MainWindow::selectCoveragePixel(const QString &text, int x, int y, const QColor &color) {
    if (!frame_ || busy())
        return;
    try {
        const auto report = Json::parse(text.toStdString());
        if (report.at("target_kind") == "viewport")
            return;
        const auto event = report.at("event").at("id").get<Id>();
        const auto resource = report.at("target_resource").get<Id>();
        locateEvent(event);
        if (report.at("target_kind") == "buffer") {
            const auto &view = report.at("target_buffer_view");
            if (y != 0 || x < 0 || uint64_t(x) >= view.at("element_count").get<uint64_t>())
                return;
            inspectResource(resource);
            bufferOffset_->setText(QString::number((view.at("first_element").get<uint64_t>() + uint64_t(x)) *
                                                   view.at("element_size").get<uint64_t>()));
            bufferLength_->setText(QString::number(view.at("element_size").get<uint64_t>()));
            bufferBoundary_->setCurrentIndex(2);
            replayTimer_.stop();
            bufferTimer_.start();
            return;
        }
        selectedResource_ = resource;
        updateExperimentActions();
        const auto &sub = report.at("target_subresource");
        history_->selectPixel(resource, x, y, sub.at("mip").get<int>(), sub.at("layer").get<int>(), 0);
        replayDebug_[1]->selectPixel(x, y, 0);
        properties("Coverage Pixel", {{"Resource", QString::number(resource)},
                                      {"Event", QString::number(event)},
                                      {"Pixel", QString("%1, %2").arg(x).arg(y)},
                                      {"Mip", QString::number(sub.at("mip").get<unsigned>())},
                                      {sub.value("layer_kind", "") == "w_slice" ? "Slice" : "Layer",
                                       QString::number(sub.at("layer").get<unsigned>())},
                                      {"Overlay RGBA", QString("%1, %2, %3, %4")
                                                           .arg(color.red())
                                                           .arg(color.green())
                                                           .arg(color.blue())
                                                           .arg(color.alpha())}});
        auto root = properties_->topLevelItem(0);
        if (root && root->childCount())
            root->child(0)->setData(
                0, Qt::UserRole,
                QString::fromStdString(Json{{"id", resource}, {"exists", true}, {"category", 5}}.dump()));
        findChild<QTabWidget *>("inspectorTabs")->setCurrentIndex(0);
        statusBar()->showMessage(QString("Pixel %1, %2 · T:%3").arg(x).arg(y).arg(resource), 4000);
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
    }
}
} // namespace flora
