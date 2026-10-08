#pragma once
#include <QAbstractItemModel>
namespace flora::testing {
inline QModelIndex structureField(QAbstractItemModel *model, const QString &key, const QModelIndex &parent = {}) {
    while(model->canFetchMore(parent))model->fetchMore(parent);
    for(int row=0;row<model->rowCount(parent);++row){auto index=model->index(row,0,parent);if(index.data().toString()==key)return index;}
    return {};
}
inline QModelIndex structureEvent(QAbstractItemModel *model, quint64 event, const QModelIndex &parent = {}) {
    while(model->canFetchMore(parent))model->fetchMore(parent);
    for(int row=0;row<model->rowCount(parent);++row){
        auto index=model->index(row,0,parent);
        if(index.siblingAtColumn(1).data(Qt::UserRole).toULongLong()==event)return index;
        if(auto child=structureEvent(model,event,index);child.isValid())return child;
    }
    return {};
}
}
