#pragma once
#include "QueryInspection.h"
#include <QStringList>
#include <optional>

namespace flora {
// Immutable projection of the existing API search contract. Full command JSON
// is discarded after each row; selection details are prepared separately.
class CommandSearchIndex final {
  public:
    static std::shared_ptr<const CommandSearchIndex> prepare(
        std::shared_ptr<const Frame> frame, std::shared_ptr<const QueryInspection> queries,
        const CancelCheck &cancelled = {});
    const Frame *frame() const { return frame_.get(); }
    size_t size() const { return rows_.size(); }
    bool matches(Id event, const QStringList &foldedWords, std::optional<Id> resource) const;
  private:
    struct Row {
        Id id;
        QString text;
        std::vector<Id> references;
    };
    explicit CommandSearchIndex(std::shared_ptr<const Frame> frame) : frame_(std::move(frame)) {}
    std::shared_ptr<const Frame> frame_;
    std::vector<Row> rows_;
};
}
