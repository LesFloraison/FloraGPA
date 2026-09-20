#pragma once
#include <functional>
#include <nlohmann/json.hpp>
class QWidget;
namespace flora {
bool editRasterizerDialog(QWidget *parent, const nlohmann::json &initial,
                          const std::function<void(const nlohmann::json &)> &commit);
}
