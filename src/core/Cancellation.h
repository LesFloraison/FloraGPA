#pragma once
#include <functional>
#include <stdexcept>

namespace flora {
using CancelCheck = std::function<bool()>;
class OperationCancelled final : public std::runtime_error {
  public:
    OperationCancelled() : std::runtime_error("Operation cancelled") {}
};
inline void checkCancellation(const CancelCheck &cancelled) {
    if (cancelled && cancelled())
        throw OperationCancelled();
}
} // namespace flora
