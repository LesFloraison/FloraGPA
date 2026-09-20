#pragma once
#include "core/Frame.h"
#include <QWidget>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
bool editViewDialog(QWidget *parent, const Frame &frame, Id selected,
                    const std::function<nlohmann::json(Id)> &load,
                    const std::function<void(Id, const nlohmann::json &)> &commit);
}
