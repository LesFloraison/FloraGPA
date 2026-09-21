#pragma once
#include <QDialog>
#include <nlohmann/json.hpp>
class QComboBox;
class QPlainTextEdit;
class QAction;
namespace flora {
class ShaderProjectDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit ShaderProjectDialog(nlohmann::json project, QWidget *parent = nullptr);
    nlohmann::json draft();
    void loadProject(const nlohmann::json &project);
    void setBusy(bool busy);
    void setContextKey(const QString &key) { contextKey_ = key; }
    QString contextKey() const { return contextKey_; }
    void importProject(const QString &path);
    void exportProject(const QString &path);
    void addFile(const QString &name);
    void removeFile();
  signals:
    void applyRequested();

  private:
    void stash();
    void selectFile(int index);
    void refreshFiles(int selected);
    void error(const std::exception &e);
    nlohmann::json files_;
    QComboBox *root_, *file_;
    QPlainTextEdit *source_, *settings_;
    QAction *apply_, *add_, *remove_, *import_;
    QString contextKey_;
    int current_ = -1;
};
} // namespace flora
