#pragma once
#include "core/Frame.h"
#include <functional>
#include <nlohmann/json.hpp>
class QWidget;
namespace flora {
bool editIaSetterDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                        const std::function<void(const nlohmann::json &)> &commit);
}
