#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <variant>

namespace flora {
class ExpressionError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
using ExpressionScalar = std::variant<bool, int64_t, uint64_t, double>;
struct ExpressionValue {
    std::string kind;
    std::vector<ExpressionScalar> values;
    const ExpressionScalar &scalar() const;
    bool truth() const;
    std::string text() const;
};
namespace expression_detail {
struct Node;
}
class DebugEnvironment;
class DebugExpression {
    std::string text_;
    std::shared_ptr<const expression_detail::Node> tree_;
    friend class DebugEnvironment;

  public:
    explicit DebugExpression(const std::string &text);
    const std::string &text() const { return text_; }
    ExpressionValue evaluate(const DebugEnvironment &environment) const;
};
class DebugEnvironment {
    nlohmann::json records_;
    std::set<std::string> localRoots_, ambiguousRoots_;
    std::map<std::string, std::vector<std::vector<std::string>>> groups_;
    std::optional<ExpressionValue> lookup(const std::string &path) const;
    std::optional<ExpressionValue> exact(const std::string &path) const;
    std::optional<std::string> path(const expression_detail::Node &node) const;
    uint32_t index(const expression_detail::Node &node) const;
    ExpressionValue evaluate(const expression_detail::Node &node) const;
    static DebugEnvironment initialize(const nlohmann::json &variables, const nlohmann::json &values,
                                       const std::set<std::string> &selected);

  public:
    explicit DebugEnvironment(nlohmann::json records);
    static DebugEnvironment native(const nlohmann::json &symbols, const nlohmann::json &values,
                                   uint64_t offset, const std::string &frameId);
    // Consumes verified SDBG assignments; assignment reconstruction is a separate reader.
    static DebugEnvironment sdbg(const nlohmann::json &symbols, const nlohmann::json &values,
                                 uint64_t offset);
    ExpressionValue expression(const DebugExpression &expression) const;
};
} // namespace flora
