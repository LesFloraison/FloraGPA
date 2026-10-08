#pragma once
#include "core/Frame.h"
#include <QAbstractItemModel>
#include <nlohmann/json.hpp>

namespace flora {
// The JSON and frame stay immutable; only visited rows acquire Qt nodes.
class StructureModel final : public QAbstractItemModel {
    Q_OBJECT
  public:
    StructureModel(std::shared_ptr<const nlohmann::json> document,
                   std::shared_ptr<const Frame> frame, QObject *parent = nullptr);
    ~StructureModel() override;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return 2; }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    QString rootToolTip() const;
  private:
    struct Node;
    Node *node(const QModelIndex &index) const;
    std::shared_ptr<const nlohmann::json> document_;
    std::shared_ptr<const Frame> frame_;
    std::unique_ptr<Node> root_;
};
}
