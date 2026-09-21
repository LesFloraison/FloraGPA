#include "ShaderProjectDialog.h"
#include "application/ShaderProject.h"
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

namespace flora {
using Json = nlohmann::json;
ShaderProjectDialog::ShaderProjectDialog(Json project, QWidget *parent) : QDialog(parent) {
    setObjectName("shaderProjectDialog");
    setWindowTitle("Shader Project");
    resize(1000, 700);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    auto toolbar = new QToolBar(this);
    toolbar->addWidget(new QLabel(" Root  "));
    root_ = new QComboBox;
    root_->setObjectName("shaderProjectRoot");
    root_->setMinimumContentsLength(24);
    root_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    toolbar->addWidget(root_);
    toolbar->addSeparator();
    apply_ = toolbar->addAction("Compile && Apply", this, &ShaderProjectDialog::applyRequested);
    apply_->setObjectName("applyShaderProject");
    toolbar->addAction("Save JSON", this, [this] {
        auto path = QFileDialog::getSaveFileName(this, "Save shader project", {}, "Shader project (*.json)");
        if (path.isEmpty())
            return;
        try {
            exportProject(path);
        } catch (const std::exception &e) {
            error(e);
        }
    });
    import_ = toolbar->addAction("Import JSON", this, [this] {
        auto path =
            QFileDialog::getOpenFileName(this, "Import shader project", {}, "Shader project (*.json)");
        if (path.isEmpty())
            return;
        try {
            importProject(path);
        } catch (const std::exception &e) {
            error(e);
        }
    });
    layout->addWidget(toolbar);
    auto tabs = new QTabWidget;
    auto sources = new QWidget;
    auto sourceLayout = new QVBoxLayout(sources);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    auto files = new QToolBar;
    file_ = new QComboBox;
    file_->setObjectName("shaderProjectFile");
    file_->setMinimumContentsLength(32);
    file_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    files->addWidget(file_);
    add_ = files->addAction("Add", this, [this] {
        bool ok = false;
        auto name = QInputDialog::getText(this, "Add source", "Virtual path", QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        try {
            addFile(name);
        } catch (const std::exception &e) {
            error(e);
        }
    });
    remove_ = files->addAction("Remove", this, [this] {
        try {
            removeFile();
        } catch (const std::exception &e) {
            error(e);
        }
    });
    sourceLayout->addWidget(files);
    source_ = new QPlainTextEdit;
    source_->setObjectName("shaderProjectSource");
    source_->setLineWrapMode(QPlainTextEdit::NoWrap);
    source_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    sourceLayout->addWidget(source_);
    settings_ = new QPlainTextEdit;
    settings_->setObjectName("shaderProjectSettings");
    settings_->setLineWrapMode(QPlainTextEdit::NoWrap);
    settings_->setFont(source_->font());
    tabs->addTab(sources, "Sources");
    tabs->addTab(settings_, "Settings");
    layout->addWidget(tabs);
    connect(file_, &QComboBox::currentIndexChanged, this, &ShaderProjectDialog::selectFile);
    loadProject(project);
}
void ShaderProjectDialog::error(const std::exception &e) {
    QMessageBox::warning(this, "Shader Project", QString::fromUtf8(e.what()));
}
void ShaderProjectDialog::stash() {
    if (current_ >= 0 && size_t(current_) < files_.size() && source_->document()->isModified())
        files_[size_t(current_)]["text"] = source_->toPlainText().toStdString();
    source_->document()->setModified(false);
}
void ShaderProjectDialog::selectFile(int index) {
    stash();
    current_ = index;
    source_->setPlainText(
        index < 0 ? QString{}
                  : QString::fromStdString(files_.at(size_t(index)).at("text").get<std::string>()));
    source_->document()->setModified(false);
}
void ShaderProjectDialog::refreshFiles(int selected) {
    const auto root = root_->currentText();
    const QSignalBlocker block(file_);
    file_->clear();
    root_->clear();
    for (const auto &f : files_) {
        const auto name = QString::fromStdString(f.at("name").get<std::string>());
        file_->addItem(name);
        root_->addItem(name);
    }
    root_->setCurrentIndex(root_->findText(root));
    file_->setCurrentIndex(selected);
    current_ = -1;
    selectFile(selected);
}
void ShaderProjectDialog::loadProject(const Json &project) {
    files_ = project.at("files");
    current_ = -1;
    refreshFiles(files_.empty() ? -1 : 0);
    root_->setCurrentIndex(root_->findText(QString::fromStdString(project.at("root").get<std::string>())));
    auto settings = project;
    for (auto key : {"format", "files", "root"})
        settings.erase(key);
    settings_->setPlainText(QString::fromStdString(settings.dump(2, ' ', true)));
}
Json ShaderProjectDialog::draft() {
    stash();
    auto settings = Json::parse(settings_->toPlainText().toStdString());
    if (!settings.is_object())
        throw std::runtime_error("Project settings must be a JSON object");
    for (auto key : {"format", "files", "root"})
        if (settings.contains(key))
            throw std::runtime_error("Settings cannot override format, files or root");
    settings["format"] = "FloraGPA shader project 1";
    settings["files"] = files_;
    settings["root"] = root_->currentText().toStdString();
    return settings;
}
void ShaderProjectDialog::addFile(const QString &name) {
    if (!add_->isEnabled())
        return;
    const auto key = shaderProjectPathKey(name.toStdString());
    for (const auto &f : files_)
        if (shaderProjectPathKey(f.at("name")) == key)
            throw std::runtime_error("Duplicate virtual source path");
    stash();
    files_.push_back({{"name", name.toStdString()}, {"text", ""}});
    refreshFiles(int(files_.size()) - 1);
}
void ShaderProjectDialog::removeFile() {
    if (!remove_->isEnabled())
        return;
    if (files_.size() <= 1)
        throw std::runtime_error("Keep at least one source file");
    stash();
    files_.erase(files_.begin() + current_);
    refreshFiles(0);
}
void ShaderProjectDialog::importProject(const QString &path) {
    if (!import_->isEnabled())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read shader project");
    loadProject(validateShaderProject(Json::parse(file.readAll().toStdString())));
}
void ShaderProjectDialog::exportProject(const QString &path) {
    const auto serialized = validateShaderProject(draft()).dump(2, ' ', true);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(serialized.data(), qint64(serialized.size())) != qint64(serialized.size()) ||
        !file.commit())
        throw std::runtime_error("Cannot save shader project");
}
void ShaderProjectDialog::setBusy(bool busy) {
    for (auto action : {apply_, add_, remove_, import_})
        action->setEnabled(!busy);
    root_->setEnabled(!busy);
    source_->setReadOnly(busy);
    settings_->setReadOnly(busy);
}
} // namespace flora
