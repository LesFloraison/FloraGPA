#include "MainWindow.h"
#include "QuadView.h"
#include <QTabWidget>
namespace flora {
void MainWindow::buildQuadUi() {
    quad_ = new QuadView;
    centerTabs_->insertTab(2, quad_, "Quad");
    connect(quad_, &QuadView::readRequested, this, [this](const QString &text, qulonglong serial) {
        if (busy()) {
            quad_->finish(serial, {{"error", "Worker is busy"}});
            return;
        }
        runningQuadRequest_ = serial;
        const auto request = nlohmann::json::parse(text.toStdString());
        QStringList args{"quad",          capturePath_,
                         "--id",          QString::number(request.at("event").get<Id>()),
                         "--quad-depth",  QString::fromStdString(request.at("depth").get<std::string>()),
                         "--quad-target", QString::fromStdString(request.at("target").get<std::string>())};
        if (request.contains("layer"))
            args << "--quad-layer" << QString::number(request.at("layer").get<uint32_t>());
        startWorker(args, false);
        if (process_.state() == QProcess::NotRunning)
            quad_->finish(serial, {{"error", "Cannot start Quad capture"}});
    });
    connect(quad_, &QuadView::cancelRequested, this, &MainWindow::cancel);
}
} // namespace flora
