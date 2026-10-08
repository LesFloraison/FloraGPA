#include "StructureModel.h"
#include <QStringList>
#include <limits>
namespace flora {
using Json = nlohmann::json;
namespace {
constexpr int batch = 256;
bool detail(const std::string &key) {
    return key == "assumption" || key == "scope" || key == "limits" || key == "note" || key == "source";
}
QString text(const Json &value) {
    return QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
}
QString notes(const Json &value) {
    QStringList result;
    if (value.is_object()) for (auto it = value.begin(); it != value.end(); ++it)
        if (detail(it.key())) result << QString::fromStdString(it->is_string() ? it->get<std::string>() : it->dump(2));
    return result.join('\n');
}
}
struct StructureModel::Node {
    const Json *value;
    Node *parent;
    int row;
    QString key;
    std::optional<Json> oversized;
    std::vector<std::string> keys;
    size_t total = 0;
    int loaded = 0;
    mutable std::map<int, std::unique_ptr<Node>> children;
    Node(const Json *value, Node *parent = nullptr, int row = 0, QString key = {})
        : value(value), parent(parent), row(row), key(std::move(key)) {
        if (value->is_structured() && value->size() > size_t(std::numeric_limits<int>::max())) {
            oversized = Json{{"error", "Capture structure exceeds Qt row limits; export JSON to inspect all entries"}};
            this->value = value = &*oversized;
        }
        if (value->is_object()) {
            for (auto it = value->begin(); it != value->end(); ++it) if (!detail(it.key())) keys.push_back(it.key());
            total = keys.size();
        } else if (value->is_array()) total = value->size();
        loaded = int(std::min<size_t>(total, batch));
    }
    Node *child(int rowIndex) const {
        auto &item = children[rowIndex];
        if (!item) {
            const auto label = value->is_array() ? QString::number(rowIndex) : QString::fromStdString(keys.at(rowIndex));
            const auto &child = value->is_array() ? value->at(rowIndex) : value->at(keys.at(rowIndex));
            item = std::make_unique<Node>(&child, const_cast<Node *>(this), rowIndex, label);
        }
        return item.get();
    }
};
StructureModel::StructureModel(std::shared_ptr<const Json> document, std::shared_ptr<const Frame> frame, QObject *parent)
    : QAbstractItemModel(parent), document_(std::move(document)), frame_(std::move(frame)),
      root_(document_ ? std::make_unique<Node>(document_.get()) : throw std::invalid_argument("Structure document is unavailable")) {}
StructureModel::~StructureModel() = default;
StructureModel::Node *StructureModel::node(const QModelIndex &index) const {
    return index.isValid() ? static_cast<Node *>(index.internalPointer()) : root_.get();
}
QModelIndex StructureModel::index(int row, int column, const QModelIndex &parent) const {
    if (row < 0 || column < 0 || column >= 2 || (parent.isValid() && parent.column()) || row >= rowCount(parent)) return {};
    return createIndex(row, column, node(parent)->child(row));
}
QModelIndex StructureModel::parent(const QModelIndex &child) const {
    if (!child.isValid()) return {};
    const auto parent = node(child)->parent;
    return !parent || parent == root_.get() ? QModelIndex{} : createIndex(parent->row, 0, parent);
}
int StructureModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() && parent.column() ? 0 : node(parent)->loaded;
}
bool StructureModel::canFetchMore(const QModelIndex &parent) const {
    return !(parent.isValid() && parent.column()) && size_t(node(parent)->loaded) < node(parent)->total;
}
void StructureModel::fetchMore(const QModelIndex &parent) {
    if (!canFetchMore(parent)) return;
    auto item = node(parent); const auto count = int(std::min<size_t>(batch, item->total - item->loaded));
    beginInsertRows(parent, item->loaded, item->loaded + count - 1); item->loaded += count; endInsertRows();
}
QVariant StructureModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) return {};
    const auto item = node(index); const auto &value = *item->value;
    if (role == Qt::DisplayRole) return index.column() == 0 ? item->key : value.is_structured() ? QString{} : text(value);
    if (role == Qt::ToolTipRole) {
        if (index.column() == 0) return notes(value);
        if (data(index, Qt::UserRole).isValid()) return QString("Double-click to locate API event");
    }
    if (role == Qt::UserRole && index.column() == 1 && frame_ && value.is_number_unsigned() &&
        (item->key == "event" || item->key == "unmap_event" || item->key == "id")) {
        const auto id = value.get<Id>(); const auto found = frame_->entries().find(id);
        if (found != frame_->entries().end() && found->second.category == 7) return QVariant::fromValue<qulonglong>(id);
    }
    return {};
}
QVariant StructureModel::headerData(int section, Qt::Orientation orientation, int role) const {
    return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(section ? "Value" : "Field") : QVariant{};
}
QString StructureModel::rootToolTip() const { return notes(*document_); }
}
