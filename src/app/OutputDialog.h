#pragma once
#include "core/Frame.h"
#include <QWidget>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
bool editOutputSetterDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                            const std::function<void(const nlohmann::json &)> &commit);
}
