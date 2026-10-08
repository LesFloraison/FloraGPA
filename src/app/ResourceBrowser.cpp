#include "ResourceBrowser.h"
#include <QHeaderView>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

namespace flora {
using Json = nlohmann::json;
namespace {
QString formatLabel(unsigned format) {
    static const std::map<unsigned, QString> names{
#include "DxgiFormats.inc"
    };
    const auto found = names.find(format);
    return found == names.end() ? QString::number(format) : found->second;
}
} // namespace

ResourceBrowser::ResourceBrowser(QWidget *parent) : QWidget(parent) {
    setObjectName("drawResourceBrowser");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    summary_ = new QLabel("Select a draw");
    summary_->setObjectName("drawResourceSummary");
    summary_->setWordWrap(true);
    summary_->setMargin(5);
    layout->addWidget(summary_);
    tree_ = new QTreeWidget;
    tree_->setObjectName("drawResources");
    tree_->setHeaderHidden(true);
    tree_->setIconSize({72, 48});
    tree_->setIndentation(10);
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree_->header()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(tree_);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (const auto selected = this->selected())
            emit bindingSelected(QString::fromStdString(selected->key));
    });
    connect(tree_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { emit previewsNeeded(); });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this] { emit previewsNeeded(); });
}
QString ResourceBrowser::cacheKey(const DrawResourceBinding &b) const {
    auto identity = drawResourceJson(b);
    identity.erase("key");
    identity.erase("stage");
    identity.erase("slot");
    return contextKey_ + ':' + QString::fromStdString(identity.dump());
}
void ResourceBrowser::clear(const QString &message) {
    QSignalBlocker block(tree_);
    contextKey_.clear();
    event_ = 0;
    bindings_.clear();
    items_.clear();
    done_.clear();
    pending_.clear();
    tree_->clear();
    summary_->setText(message.isEmpty() ? "Select a draw" : message);
}
void ResourceBrowser::setContext(const QString &key, std::vector<DrawResourceBinding> bindings, Id event) {
    if (key == contextKey_)
        return;
    clear();
    contextKey_ = key;
    event_ = event;
    bindings_ = std::move(bindings);
    QSignalBlocker block(tree_);
    const auto inventory = drawResourceInventory(bindings_, event);
    const auto &c = inventory.at("counts");
    summary_->setText(QString("%1 textures · %2 bindings\n%3 RTs · %4 depth · %5 UAVs")
                          .arg(c.at("input_textures").get<unsigned>())
                          .arg(c.at("input_bindings").get<unsigned>())
                          .arg(c.at("rtv").get<unsigned>())
                          .arg(c.at("dsv").get<unsigned>())
                          .arg(c.at("uav").get<unsigned>()));
    summary_->setToolTip("Effective bound resources. Bindings do not prove sampling or writes.");
    auto outputs = new QTreeWidgetItem(tree_, {"Outputs · After draw"});
    auto inputs = new QTreeWidgetItem(tree_, {"Inputs · Before draw"});
    auto buffers = new QTreeWidgetItem(inputs, {"Buffers"});
    buffers->setFlags(buffers->flags() & ~Qt::ItemIsSelectable);
    inputs->setFlags(inputs->flags() & ~Qt::ItemIsSelectable);
    outputs->setFlags(outputs->flags() & ~Qt::ItemIsSelectable);
    for (const auto &b : bindings_) {
        const auto id = b.image.resource ? b.image.resource : b.image.view;
        QString text = QString("%1 %2%3 · %4%5")
                           .arg(QString::fromStdString(b.stage), QString::fromStdString(b.kind))
                           .arg(b.slot)
                           .arg(b.texture ? "T:" : "ID:")
                           .arg(id);
        if (b.texture)
            text += QString("\n%1 × %2\n%3").arg(b.width).arg(b.height).arg(formatLabel(b.image.format));
        auto item = new QTreeWidgetItem(
            b.role == ResourceRole::Input ? (b.texture || !b.error.empty() ? inputs : buffers) : outputs,
            {text});
        item->setData(0, Qt::UserRole, QString::fromStdString(b.key));
        item->setToolTip(0, QString::fromStdString(drawResourceJson(b).dump(2)));
        item->setIcon(0, style()->standardIcon(
                             b.error.empty() ? (b.texture ? QStyle::SP_FileIcon : QStyle::SP_DriveHDIcon)
                                             : QStyle::SP_MessageBoxWarning));
        if (!b.error.empty())
            item->setText(0, text + "\nUnavailable");
        items_[b.key] = item;
    }
    inputs->setExpanded(true);
    outputs->setExpanded(true);
    showCached();
}
std::optional<DrawResourceBinding> ResourceBrowser::selected() const {
    const auto item = tree_->currentItem();
    if (!item)
        return {};
    const auto key = item->data(0, Qt::UserRole).toString().toStdString();
    for (const auto &b : bindings_)
        if (b.key == key)
            return b;
    return {};
}
bool ResourceBrowser::select(const std::string &key, bool notify) {
    const auto found = items_.find(key);
    if (found == items_.end())
        return false;
    if (notify)
        tree_->setCurrentItem(found->second);
    else {
        QSignalBlocker block(tree_);
        tree_->setCurrentItem(found->second);
    }
    tree_->scrollToItem(found->second);
    return true;
}
void ResourceBrowser::clearSelection() {
    QSignalBlocker block(tree_);
    tree_->clearSelection();
    tree_->setCurrentItem(nullptr);
}
void ResourceBrowser::showCached() {
    for (const auto &b : bindings_)
        if (auto cached = cache_.object(cacheKey(b))) {
            items_.at(b.key)->setIcon(0, QPixmap::fromImage(*cached, Qt::NoFormatConversion));
            done_.insert(b.key);
        }
}
Json ResourceBrowser::nextPreviews() {
    showCached();
    Json keys = Json::array();
    if (!pending_.empty() || !isVisible())
        return {{"previews", keys}};
    auto add = [&](const DrawResourceBinding &b) {
        if (keys.size() >= 16 || !b.texture || !b.error.empty() || done_.contains(b.key) ||
            pending_.contains(b.key))
            return;
        keys.push_back(b.key);
        pending_.insert(b.key);
    };
    if (const auto current = selected())
        add(*current);
    for (const auto &b : bindings_)
        if (tree_->visualItemRect(items_.at(b.key)).intersects(tree_->viewport()->rect()))
            add(b);
    return {{"previews", keys}};
}
void ResourceBrowser::acceptPreviews(const QString &key, ThumbnailOutput result) {
    if (key != contextKey_)
        return;
    if (result.event != event_) throw std::runtime_error("Thumbnail report event changed");
    struct Ready { QTreeWidgetItem *item; QString cacheKey, tooltip; QImage image; QIcon icon; };
    std::vector<Ready> ready;
    // Validate the entire requested batch and allocate paint objects before any
    // item/cache changes. A later invalid binding must not publish earlier rows.
    for (const auto &bindingKey : pending_) {
        const auto found = result.rows.find(bindingKey);
        if (found == result.rows.end() || !items_.contains(bindingKey))
            throw std::runtime_error("Requested thumbnail binding is missing: " + bindingKey);
        const auto &r = found->second;
        const auto row = std::find_if(bindings_.begin(), bindings_.end(),
                                      [&](const auto &b) { return b.key == bindingKey; });
        if (row == bindings_.end() || r.resource != row->image.resource ||
            r.view != row->image.view || r.event != row->image.event)
            throw std::runtime_error("Thumbnail binding identity changed: " + bindingKey);
        const auto identity = drawResourceJson(*row);
        if (r.identity != identity) throw std::runtime_error("Thumbnail subresource or binding identity changed: " + bindingKey);
        Ready item{items_.at(bindingKey), cacheKey(*row)};
        if (!r.display.isNull()) {
            if (r.error || r.display.width() > 96 || r.display.height() > 96 ||
                r.display.format() != QImage::Format_ARGB32_Premultiplied)
                throw std::runtime_error("Invalid prepared thumbnail: " + bindingKey);
            auto pixmap = QPixmap::fromImage(r.display, Qt::NoFormatConversion);
            if (pixmap.isNull()) throw std::runtime_error("Cannot allocate thumbnail pixmap");
            item.icon = QIcon(pixmap); item.image = r.display;
            item.tooltip = QString::fromStdString(identity.dump(2));
        } else if (r.error) {
            item.tooltip = *r.error;
            item.icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        } else {
            throw std::runtime_error("Requested thumbnail has no preview or error: " + bindingKey);
        }
        ready.push_back(std::move(item));
    }
    for (auto &item : ready) {
        item.item->setIcon(0, item.icon); item.item->setToolTip(0, item.tooltip);
        if (!item.image.isNull()) {
            const int cost = int(std::max<qsizetype>(1, (item.image.sizeInBytes() + 1023) / 1024));
            cache_.insert(item.cacheKey, new QImage(std::move(item.image)), cost);
        }
    }
    done_.insert(pending_.begin(), pending_.end());
    pending_.clear();
}
void ResourceBrowser::failPreviews(const QString &key, const QString &error) {
    if (key != contextKey_)
        return;
    for (const auto &k : pending_)
        if (items_.contains(k))
            items_.at(k)->setToolTip(0, error);
    if (error != "Selection changed")
        done_.insert(pending_.begin(), pending_.end());
    pending_.clear();
}
} // namespace flora
