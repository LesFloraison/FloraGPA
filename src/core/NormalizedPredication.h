#pragma once
#include "Predication.h"
namespace flora {
struct NormalizedPredicateProof {
    Id event{}, resource{}, witness{};
    uint32_t value{};
};
// GPA 2025 R1: a linked failed marker lookup proves the wrapper's successful
// GetData branch and its normalized comparison. This recovers control only.
std::map<Id, NormalizedPredicateProof> auditNormalizedPredication(const Frame &,
                                                                  const CancelCheck &cancelled = {});
} // namespace flora
