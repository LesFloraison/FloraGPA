#pragma once
#include "core/Frame.h"
#include <QAbstractTableModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <memory>

namespace flora {
class CaptureModel final : public QAbstractTableModel {
  public:
    enum class Kind { Commands, Resources };
    explicit CaptureModel(Kind kind, QObject *parent = nullptr) : QAbstractTableModel(parent), kind_(kind) {}
    void setFrame(std::shared_ptr<const Frame> frame);
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(ids_.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
    Id idAt(int row) const { return row >= 0 && row < int(ids_.size()) ? ids_[row] : 0; }
    int rowOf(Id id) const;
    QString debugNames(Id id) const {
        auto found = names_.find(id);
        return found == names_.end() ? QString{} : found->second.join(" / ");
    }

  private:
    Kind kind_;
    std::shared_ptr<const Frame> frame_;
    std::vector<Id> ids_;
    std::map<Id, QStringList> names_;
};
class CaptureFilter final : public QSortFilterProxyModel {
  public:
    bool workOnly = false;
    int resourceType = 0;
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void refresh() {
        beginFilterChange();
        endFilterChange(Direction::Rows);
    }

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
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
