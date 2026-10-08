#pragma once
#include "core/Frame.h"
#include <QString>
#include <nlohmann/json.hpp>

namespace flora {
// Fixed Worker artifacts. Both files are staged and checked before either is
// committed. The two final renames are not an atomic filesystem transaction.
void exportOutputStorageFiles(const QString &directory, const QString &destination,
                              const nlohmann::json &display, const CancelCheck &cancelled = {});
}
