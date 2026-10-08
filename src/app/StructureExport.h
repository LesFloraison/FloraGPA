#pragma once
#include "core/Cancellation.h"
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
void exportStructureFile(const QString &path, const nlohmann::json &document, const CancelCheck &cancelled = {});
}
