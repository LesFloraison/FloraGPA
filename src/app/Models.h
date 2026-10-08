#pragma once
#include "application/ApiCommands.h"
#include "core/Frame.h"
#include "QueryInspection.h"
#include "CommandSearchIndex.h"
#include <QAbstractTableModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <QRegularExpression>
#include <deque>
#include <memory>

namespace flora {
class CaptureModel final : public QAbstractTableModel {
  public:
    enum class Kind { Commands, Resources };
    explicit CaptureModel(Kind kind, QObject *parent = nullptr) : QAbstractTableModel(parent), kind_(kind) {}
    void setFrame(std::shared_ptr<const Frame> frame, std::shared_ptr<const QueryInspection> queries = {},
                  std::shared_ptr<const CommandSearchIndex> search = {});
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(ids_.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
    Id idAt(int row) const { return row >= 0 && row < int(ids_.size()) ? ids_[row] : 0; }
    int rowOf(Id id) const;
    // Return an owned value: cache eviction or capture replacement cannot
    // invalidate a caller's selected command document.
    nlohmann::json command(Id id) const;
    bool matchesCommand(Id id, const QStringList &words, std::optional<Id> resource) const;
    static constexpr size_t detailCacheLimit = 128;
    size_t cachedCommandCount() const { return commandDetails_.size(); }
    bool isCommands() const { return kind_ == Kind::Commands; }
    QString debugNames(Id id) const {
        auto found = names_.find(id);
        return found == names_.end() ? QString{} : found->second.join(" / ");
    }

  private:
    Kind kind_;
    std::shared_ptr<const Frame> frame_;
    std::shared_ptr<const QueryInspection> queries_;
    std::shared_ptr<const CommandSearchIndex> search_;
    std::vector<Id> ids_;
    std::map<Id, QStringList> names_;
    mutable std::map<Id, nlohmann::json> commandDetails_;
    mutable std::deque<Id> detailOrder_;
};
class CaptureFilter final : public QSortFilterProxyModel {
  public:
    bool workOnly = false;
    int resourceType = 0;
    std::optional<Id> referencedResource;
    QString searchText;
    void setFilterFixedString(const QString &text) {
        searchText = text;
        searchWords_ = text.toCaseFolded().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        QSortFilterProxyModel::setFilterFixedString(text);
    }
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void refresh() {
        beginFilterChange();
        endFilterChange(Direction::Rows);
    }

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
  private:
    QStringList searchWords_;
};
class BufferModel final : public QAbstractTableModel {
  public:
    using QAbstractTableModel::QAbstractTableModel;
    void setBytes(QByteArray data, uint64_t offset = 0) {
        beginResetModel();
        data_ = std::move(data);
        offset_ = offset;
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        auto stride = words_ ? 4 : 16;
        return parent.isValid() ? 0 : int((data_.size() + stride - 1) / stride);
    }
    int columnCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : (words_ ? 5 : 3);
    }
    void setWords(bool words) {
        beginResetModel();
        words_ = words;
        endResetModel();
    }
    const QByteArray &bytes() const { return data_; }
    uint64_t offset() const { return offset_; }
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;

  private:
    QByteArray data_;
    bool words_ = false;
    uint64_t offset_ = 0;
};
class GeometryModel final : public QAbstractTableModel {
  public:
    using QAbstractTableModel::QAbstractTableModel;
    void setTable(const QJsonObject &table) {
        beginResetModel();
        columns_ = table["columns"].toArray();
        rows_ = table["rows"].toArray();
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(rows_.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(columns_.size());
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  private:
    QJsonArray columns_, rows_;
};
} // namespace flora
