#include "CompatibilityButton.h"
#include "application/FrameValidation.h"
#include <QDialog>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QSaveFile>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
namespace flora {
CompatibilityButton::CompatibilityButton(QWidget *parent) : QPushButton(parent) {
    setObjectName("captureCompatibility");
    setFlat(true);
    setToolTip("Check original capture structure and known replay gaps; does not validate GPU output");
    connect(this, &QPushButton::clicked, this, &CompatibilityButton::showReport);
    setCapture({});
}
CompatibilityButton::~CompatibilityButton() {
    if (cancel_)
        cancel_->store(true);
}
void CompatibilityButton::setCapture(const QString &path) {
    if (cancel_)
        cancel_->store(true);
    cancel_.reset();
    ++serial_;
    path_ = path;
    report_ = nullptr;
    setEnabled(!path.isEmpty());
    setText("Preflight: Not checked");
    if (dialog_)
        delete dialog_.data();
}
void CompatibilityButton::showReport() {
    if (!dialog_) {
        dialog_ = new QDialog(this);
        dialog_->setAttribute(Qt::WA_DeleteOnClose);
        dialog_->setObjectName("compatibilityDialog");
        dialog_->setWindowTitle("Capture Compatibility");
        dialog_->resize(860, 480);
        auto layout = new QVBoxLayout(dialog_);
        summary_ = new QLabel(dialog_);
        summary_->setObjectName("compatibilitySummary");
        layout->addWidget(summary_);
        findings_ = new QTreeWidget(dialog_);
        findings_->setObjectName("compatibilityFindings");
        findings_->setColumnCount(5);
        findings_->setHeaderLabels({"Severity", "Event / Entry", "Resource", "Kind", "Reason"});
        findings_->setRootIsDecorated(false);
        findings_->header()->setStretchLastSection(true);
        layout->addWidget(findings_);
        auto buttons = new QHBoxLayout;
        retry_ = new QPushButton("Check", dialog_);
        retry_->setObjectName("compatibilityRetry");
        cancelButton_ = new QPushButton("Cancel", dialog_);
        cancelButton_->setObjectName("compatibilityCancel");
        export_ = new QPushButton("Export JSON…", dialog_);
        buttons->addWidget(retry_);
        buttons->addWidget(cancelButton_);
        buttons->addStretch();
        buttons->addWidget(export_);
        layout->addLayout(buttons);
        connect(retry_, &QPushButton::clicked, this, &CompatibilityButton::run);
        connect(cancelButton_, &QPushButton::clicked, this, [this] {
            if (cancel_)
                cancel_->store(true);
        });
        connect(export_, &QPushButton::clicked, this, [this] {
            auto path = QFileDialog::getSaveFileName(dialog_, "Export Compatibility", "validation.json",
                                                     "JSON (*.json)");
            if (path.isEmpty())
                return;
            QSaveFile file(path);
            auto bytes = QByteArray::fromStdString(report_.dump(2) + "\n");
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                QMessageBox::warning(dialog_, "Export failed", file.errorString());
        });
        connect(dialog_, &QDialog::finished, this, [this] {
            if (cancel_)
                cancel_->store(true);
        });
    }
    dialog_->show();
    dialog_->raise();
    if (report_.is_null() && !cancel_)
        run();
    else
        refresh();
}
void CompatibilityButton::run() {
    if (cancel_ || path_.isEmpty())
        return;
    report_ = nullptr;
    auto token = std::make_shared<std::atomic_bool>(false);
    cancel_ = token;
    const auto serial = ++serial_;
    const auto path = path_;
    setText("Preflight: Checking…");
    refresh();
    auto watcher = new QFutureWatcher<nlohmann::json>(this);
    connect(watcher, &QFutureWatcher<nlohmann::json>::finished, this, [this, watcher, serial] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (serial != serial_)
            return;
        if (cancel_ && cancel_->load()) {
            result["status"] = "cancelled";
            result["cancelled"] = true;
            result["completed"] = false;
        }
        cancel_.reset();
        report_ = result;
        const auto status = result.value("status", std::string("blocked"));
        setText(status == "checked"     ? "Preflight: Checked"
                : status == "blocked"   ? "Preflight: Blocked"
                : status == "cancelled" ? "Preflight: Cancelled"
                                        : "Preflight: Review");
        refresh();
        emit resultReady();
    });
    watcher->setFuture(QtConcurrent::run([path, token] {
        return validateFrame(std::filesystem::path(path.toStdWString()), [token] { return token->load(); });
    }));
}
void CompatibilityButton::refresh() {
    if (!dialog_)
        return;
    retry_->setEnabled(!cancel_);
    cancelButton_->setEnabled(bool(cancel_));
    export_->setEnabled(!report_.is_null());
    summary_->setText(cancel_             ? "Checking original capture…"
                      : report_.is_null() ? "Not checked"
                                          : QString("%1 errors · %2 warnings · GPU not tested")
                                                .arg(report_.value("errors", 0))
                                                .arg(report_.value("warnings", 0)));
    findings_->clear();
    if (report_.is_null())
        return;
    for (const auto &item : report_["findings"]) {
        auto text = [&](const char *key) {
            const auto &v = item.at(key);
            return v.is_null() ? QString()
                               : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
        };
        auto row = new QTreeWidgetItem(findings_, {text("severity"), text("entry_id"), text("resource_id"),
                                                   text("kind"), text("reason")});
        row->setToolTip(4, text("reason"));
    }
}
} // namespace flora
