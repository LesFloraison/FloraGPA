#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
// Immutable, capture-owned Query rows prepared before publishing a command model.
class QueryInspection final {
  public:
    static std::shared_ptr<const QueryInspection> prepare(std::shared_ptr<const Frame> frame,
                                                          const CancelCheck &cancelled = {});
    const Frame *frame() const { return frame_.get(); }
    const nlohmann::json *find(Id event) const;
    size_t size() const { return rows_.size(); }
  private:
    explicit QueryInspection(std::shared_ptr<const Frame> frame) : frame_(std::move(frame)) {}
    std::shared_ptr<const Frame> frame_;
    std::map<Id, nlohmann::json> rows_;
};
}
