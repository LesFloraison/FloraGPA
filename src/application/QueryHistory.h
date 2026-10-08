#pragma once
#include "core/Frame.h"
#include <memory>
#include <nlohmann/json.hpp>
namespace flora {
// Apply to every decoded row in capture order, before filtering output.
class QueryHistory final {
  public:
    explicit QueryHistory(const Frame &frame);
    ~QueryHistory();
    void apply(nlohmann::json &row);
  private:
    struct State;
    std::unique_ptr<State> state_;
};
}
