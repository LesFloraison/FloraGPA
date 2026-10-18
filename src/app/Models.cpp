#include "Models.h"
#include "application/CaptureNames.h"
#include <QColor>
#include <QFont>

namespace flora {
QVariant GeometryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size() || index.column() >= columns_.size() ||
        role != Qt::DisplayRole)
        return {};
    auto value = rows_[index.row()].toArray().at(index.column());
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 12);
    if (value.isBool())
        return value.toBool() ? "True" : "False";
    return value.toString();
}
QVariant GeometryModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || section < 0 ||
        (orientation == Qt::Horizontal && section >= columns_.size()))
        return {};
    return orientation == Qt::Horizontal ? QVariant(columns_.at(section).toString()) : QVariant(section + 1);
}
void CaptureModel::setFrame(std::shared_ptr<const Frame> frame, std::shared_ptr<const QueryInspection> queries,
                           std::shared_ptr<const CommandSearchIndex> search) {
    if (queries && queries->frame() != frame.get())
        throw std::invalid_argument("Query inspection belongs to another capture");
    if (frame && kind_ == Kind::Commands && !queries)
        throw std::invalid_argument("Command model requires prepared Query inspection");
    if (search && search->frame() != frame.get())
        throw std::invalid_argument("API search belongs to another capture");
    if (frame && kind_ == Kind::Commands && !search)
        throw std::invalid_argument("Command model requires prepared API search");
    beginResetModel();
    session_.reset();
    frame_ = std::move(frame);
    queries_ = std::move(queries);
    search_ = std::move(search);
    ids_.clear();
    names_.clear();
    commandDetails_.clear();
    detailOrder_.clear();
    if (frame_)
        for (auto &[id, e] : frame_->entries())
            if (e.category == (kind_ == Kind::Commands ? 7 : 5))
                ids_.push_back(id);
    if (frame_ && kind_ == Kind::Resources) {
        auto catalog = capturedNames(*frame_);
        for (const auto &record : catalog["records"]) {
            auto id = std::stoull(record["resource_id"].get<std::string>());
            auto name = QString::fromStdString(record["name"].get<std::string>());
            if (!names_[id].contains(name))
                names_[id].push_back(name);
        }
    }
    endResetModel();
}
void CaptureModel::setSession(std::shared_ptr<const IReplaySession> session) {
    if (session && session->backend() == ReplayBackend::Dx11)
        throw std::invalid_argument("DX11 command model requires prepared Query/search data");
    setFrame({});
    beginResetModel();
    session_ = std::move(session);
    if (session_ && kind_ == Kind::Commands)
        ids_.assign(session_->events().begin(), session_->events().end());
    if (session_ && kind_ == Kind::Resources)
        ids_.assign(session_->resources().begin(), session_->resources().end());
    endResetModel();
}
int CaptureModel::rowOf(Id id) const {
    auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
    return it != ids_.end() && *it == id ? int(it - ids_.begin()) : -1;
}
nlohmann::json CaptureModel::command(Id id) const {
    if (session_) return session_->eventDetails(id);
    if (!frame_) throw std::runtime_error("Command capture is unavailable");
    if (queries_)
        if (const auto row = queries_->find(id)) return *row;
    auto found = commandDetails_.find(id);
    if (found == commandDetails_.end()) {
        auto details = inspectCommand(*frame_, id);
        if (details["name"] == "GetData")
            throw std::runtime_error("Captured Query result was not prepared");
        if (commandDetails_.size() == detailCacheLimit) {
            commandDetails_.erase(detailOrder_.front()); detailOrder_.pop_front();
        }
        detailOrder_.push_back(id);
        try { found = commandDetails_.emplace(id, std::move(details)).first; }
        catch (...) { detailOrder_.pop_back(); throw; }
    }
    return found->second;
}
bool CaptureModel::matchesCommand(Id id, const QStringList &words, std::optional<Id> resource) const {
    if (session_) {
        if (resource) return false; // Do not infer resource references from arbitrary payload bytes.
        auto e = session_->event(id);
        auto text = QString::fromStdString(std::to_string(id) + " " + e.name + " " + e.summary).toCaseFolded();
        const bool needsArguments = std::any_of(words.begin(), words.end(), [&](const auto &word) { return !text.contains(word); });
        if (needsArguments) text += QString::fromStdString(session_->argumentText(id)).toCaseFolded();
        for (const auto &word : words) if (!text.contains(word)) return false;
        return true;
    }
    return search_ && search_->matches(id, words, resource);
}
QVariant CaptureModel::data(const QModelIndex &index, int role) const {
    if (session_) {
        if (!index.isValid() || index.row() < 0 || index.row() >= int(ids_.size())) return {};
        if (kind_ == Kind::Resources) {
            const auto r = session_->resource(ids_[index.row()]);
            if (role == Qt::UserRole) return QVariant::fromValue<qulonglong>(r.id);
            if (role == Qt::ToolTipRole) return QString::fromStdString("Captured object " + std::to_string(r.capturedObject) + " | " + r.summary);
            if (role != Qt::DisplayRole) return {};
            return index.column() == 0 ? QString::number(r.id) : QString::fromStdString(index.column() == 1 ? r.name : r.summary);
        }
        const auto e = session_->event(ids_[index.row()]);
        if (role == Qt::UserRole) return QVariant::fromValue<qulonglong>(e.id);
        if (role == Qt::UserRole + 2) return e.work;
        if (role == Qt::UserRole + 3 && e.thread) return QVariant::fromValue<qulonglong>(*e.thread);
        if (role == Qt::ToolTipRole) return QString::fromStdString(e.summary);
        if (role == Qt::ForegroundRole && e.work) return QColor("#a6dffa");
        if (role != Qt::DisplayRole) return {};
        return index.column() == 0 ? QString::number(e.id) :
               QString::fromStdString(index.column() == 1 ? e.name : e.summary);
    }
    if (!index.isValid() || index.row() >= int(ids_.size()) || !frame_)
        return {};
    auto id = ids_[index.row()];
    const auto &e = frame_->entry(id);
    if (role == Qt::UserRole)
        return QVariant::fromValue<qulonglong>(id);
    if (role == Qt::UserRole + 1)
        return e.type;
    if (role == Qt::UserRole + 2) return isDraw(e.type);
    if (role == Qt::ForegroundRole && kind_ == Kind::Commands && isDraw(e.type))
        return QColor("#a6dffa");
    if (role == Qt::ToolTipRole)
        return (debugNames(id).isEmpty() ? QString{} : debugNames(id) + '\n') +
               QString("ID %1 · type 0x%2 · %3 bytes").arg(id).arg(e.type, 0, 16).arg(e.size);
    if (role != Qt::DisplayRole)
        return {};
    if (index.column() == 0)
        return QString::number(id);
    if (index.column() == 1) {
        auto name =
            QString::fromStdString(kind_ == Kind::Commands ? commandName(e.type) : resourceName(e.type));
        if (kind_ == Kind::Resources && !debugNames(id).isEmpty())
            name += " · " + debugNames(id);
        if (kind_ == Kind::Commands && isDraw(e.type)) {
            auto args = data(this->index(index.row(), 2), Qt::DisplayRole).toString();
            if (!args.isEmpty())
                name += "  " + args;
        }
        return name;
    }
    try {
        if (kind_ == Kind::Commands && isDraw(e.type)) {
            auto ev = frame_->event(id);
            QStringList values;
            if (ev.argumentBuffer)
                values << QString::number(ev.argumentBuffer);
            for (size_t i = 0; i < ev.args.size(); ++i)
                values << ((e.type == 0x39 && i == 2) || (e.type == 0x3a && i == 3)
                               ? QString::number(int32_t(ev.args[i]))
                               : QString::number(ev.args[i]));
            return values.join(", ");
        }
        if (kind_ == Kind::Resources) {
            auto r = frame_->resource(id);
            if (r.type == 0x83)
                return QString("%1 B").arg(r.desc[0]);
            if (r.type >= 0x84 && r.type <= 0x87) {
                auto info = textureInfo(r);
                return QString("%1 × %2").arg(info.width).arg(info.height);
            }
        }
    } catch (const std::exception &) {
        return "Invalid";
    }
    return {};
}
QVariant CaptureModel::headerData(int n, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return QStringList{"ID", kind_ == Kind::Commands ? "API call" : "Resource",
                       kind_ == Kind::Commands ? "Arguments" : "Size"}
        .value(n);
}
bool CaptureFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    auto index = sourceModel()->index(row, 0, parent);
    auto type = index.data(Qt::UserRole + 1).toUInt();
    if (workOnly && !index.data(Qt::UserRole + 2).toBool())
        return false;
    if (thread && (!index.data(Qt::UserRole + 3).isValid() ||
                   index.data(Qt::UserRole + 3).toULongLong() != *thread)) return false;
    if (resourceType == 1 && !(type >= 0x84 && type <= 0x86))
        return false;
    if (resourceType == 2 && type != 0x83)
        return false;
    if (resourceType == 3 && !(type >= 0x90 && type <= 0x95))
        return false;
    auto model = static_cast<const CaptureModel *>(sourceModel());
    if (model->isCommands() && (referencedResource || !filterRegularExpression().pattern().isEmpty())) {
        return model->matchesCommand(model->idAt(row), searchWords_, referencedResource);
    }
    return QSortFilterProxyModel::filterAcceptsRow(row, parent);
}
QVariant BufferModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || role != Qt::DisplayRole)
        return {};
    auto stride = words_ ? 4 : 16;
    auto offset = qsizetype(index.row()) * stride;
    auto bytes = data_.mid(offset, stride);
    if (index.column() == 0)
        return QString("%1").arg(offset_ + uint64_t(offset), 8, 16, QChar('0')).toUpper();
    if (index.column() == 1)
        return QString::fromLatin1(bytes.toHex(' ')).toUpper();
    if (words_) {
        if (bytes.size() != 4)
            return {};
        uint32_t u;
        int32_t i;
        float f;
        std::memcpy(&u, bytes.constData(), 4);
        std::memcpy(&i, bytes.constData(), 4);
        std::memcpy(&f, bytes.constData(), 4);
        if (index.column() == 2)
            return QString::number(u);
        if (index.column() == 3)
            return QString::number(i);
        return QString::number(double(f), 'g', 9);
    }
    QString text;
    for (auto c : bytes)
        text += (uint8_t(c) >= 32 && uint8_t(c) < 127) ? QChar(c) : QChar('.');
    return text;
}
QVariant BufferModel::headerData(int n, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return (words_ ? QStringList{"Offset", "Hexadecimal", "uint32", "int32", "float32"}
                   : QStringList{"Offset", "Hexadecimal", "ASCII"})
        .value(n);
}
} // namespace flora
