#pragma once
#include "core/Frame.h"
#include <QAbstractTableModel>
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

  private:
    Kind kind_;
    std::shared_ptr<const Frame> frame_;
    std::vector<Id> ids_;
};
class CaptureFilter final : public QSortFilterProxyModel {
  public:
    bool workOnly = false;
    int resourceType = 0;
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void refresh() { invalidateFilter(); }

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
};
class BufferModel final : public QAbstractTableModel {
  public:
    using QAbstractTableModel::QAbstractTableModel;
    void setBytes(QByteArray data) {
        beginResetModel();
        data_ = std::move(data);
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int((data_.size() + 15) / 16);
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;

  private:
    QByteArray data_;
};
} // namespace flora
