#pragma once
#include "application/DrawResources.h"
#include "ThumbnailOutput.h"
#include <QCache>
#include <QImage>
#include <QTreeWidget>
#include <QWidget>

class QLabel;
namespace flora {
// UI-only inventory and bounded thumbnail cache. The owner serializes worker jobs.
class ResourceBrowser final : public QWidget {
    Q_OBJECT
  public:
    explicit ResourceBrowser(QWidget *parent = nullptr);
    void setContext(const QString &key, std::vector<DrawResourceBinding> bindings, Id event);
    void clear(const QString &message = {});
    const QString &contextKey() const { return contextKey_; }
    const std::vector<DrawResourceBinding> &bindings() const { return bindings_; }
    std::optional<DrawResourceBinding> selected() const;
    bool select(const std::string &key, bool notify = false);
    void clearSelection();
    nlohmann::json nextPreviews();
    void acceptPreviews(const QString &key, ThumbnailOutput result);
    void failPreviews(const QString &key, const QString &error);
  signals:
    void bindingSelected(const QString &key);
    void previewsNeeded();

  private:
    void showCached();
    QString cacheKey(const DrawResourceBinding &binding) const;
    QTreeWidget *tree_;
    QLabel *summary_;
    QString contextKey_;
    Id event_{};
    std::vector<DrawResourceBinding> bindings_;
    std::map<std::string, QTreeWidgetItem *> items_;
    std::set<std::string> done_, pending_;
    QCache<QString, QImage> cache_{128 * 1024}; // KiB, decoded image cost.
};
} // namespace flora
