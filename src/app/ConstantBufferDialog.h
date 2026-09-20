#pragma once
#include "application/SetterEdits.h"
#include <functional>
class QWidget;
namespace flora {
bool editConstantBufferDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                              const std::function<void(const nlohmann::json &)> &commit);
}
