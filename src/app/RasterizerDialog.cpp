#include "RasterizerDialog.h"
#include "application/RasterizerEdits.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
namespace flora {
bool editRasterizerDialog(QWidget *parent, const nlohmann::json &initial,
                          const std::function<void(const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("rasterizerDialog");
    dialog.setWindowTitle("Rasterizer");
    dialog.resize(700, 480);
    auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget;
    layout->addWidget(tabs);
    auto page = new QWidget;
    auto form = new QFormLayout(page);
    tabs->addTab(page, "State");
    std::map<std::string, QWidget *> fields;
    auto add = [&](const char *key, const QString &label, const QStringList &choices = {},
                   const QList<int> &codes = {}) {
        const auto &value = initial["rasterizer"].at(key);
        QWidget *widget;
        if (!choices.empty()) {
            auto combo = new QComboBox;
            for (int i = 0; i < choices.size(); ++i)
                combo->addItem(choices[i], codes[i]);
            combo->setCurrentIndex(
                combo->findData(value.is_boolean() ? int(value.get<bool>()) : value.get<int>()));
            widget = combo;
        } else
            widget = new QLineEdit(QString::fromStdString(value.dump()));
        widget->setObjectName(QString("rs_%1").arg(key));
        fields[key] = widget;
        form->addRow(label, widget);
    };
    add("fill_mode", "Fill", {"Wireframe", "Solid"}, {2, 3});
    add("cull_mode", "Cull", {"None", "Front", "Back"}, {1, 2, 3});
    add("front_counter_clockwise", "Front winding", {"Clockwise", "Counterclockwise"}, {0, 1});
    add("depth_bias", "Depth bias");
    add("depth_bias_clamp", "Bias clamp");
    add("slope_scaled_depth_bias", "Slope bias");
    add("depth_clip_enable", "Depth clip", {"Disabled", "Enabled"}, {0, 1});
    add("scissor_enable", "Scissor", {"Disabled", "Enabled"}, {0, 1});
    add("multisample_enable", "Multisample", {"Disabled", "Enabled"}, {0, 1});
    add("antialiased_line_enable", "Line antialiasing", {"Disabled", "Enabled"}, {0, 1});
    add("forced_sample_count", "Forced samples", {"Default", "1", "2", "4", "8", "16"}, {0, 1, 2, 4, 8, 16});
    add("conservative_raster", "Conservative raster", {"Disabled", "Enabled"}, {0, 1});
    std::map<std::string, QTableWidget *> tables;
    for (const auto key : {"viewports", "scissors"}) {
        bool viewport = std::string(key) == "viewports";
        auto widget = new QWidget;
        auto box = new QVBoxLayout(widget);
        box->setContentsMargins(0, 0, 0, 0);
        auto bar = new QHBoxLayout;
        auto addRow = new QPushButton("Add");
        auto remove = new QPushButton("Remove");
        addRow->setObjectName(QString("add_%1").arg(key));
        remove->setObjectName(QString("remove_%1").arg(key));
        bar->addWidget(addRow);
        bar->addWidget(remove);
        bar->addStretch();
        box->addLayout(bar);
        auto table = new QTableWidget;
        table->setObjectName(QString::fromLatin1(key));
        table->setColumnCount(viewport ? 6 : 4);
        table->setHorizontalHeaderLabels(
            viewport ? QStringList{"X", "Y", "Width", "Height", "Min depth", "Max depth"}
                     : QStringList{"Left", "Top", "Right", "Bottom"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setRowCount(int(initial.at(key).size()));
        for (int row = 0; row < table->rowCount(); ++row)
            for (int column = 0; column < table->columnCount(); ++column)
                table->setItem(
                    row, column,
                    new QTableWidgetItem(QString::fromStdString(initial.at(key)[row][column].dump())));
        auto enable = [=] {
            addRow->setEnabled(table->rowCount() < 16);
            remove->setEnabled(table->rowCount() > 0);
        };
        enable();
        QObject::connect(addRow, &QPushButton::clicked, &dialog, [=] {
            int row = table->rowCount();
            if (row >= 16)
                return;
            table->insertRow(row);
            QStringList defaults =
                viewport ? QStringList{"0", "0", "1", "1", "0", "1"} : QStringList{"0", "0", "1", "1"};
            for (int col = 0; col < defaults.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(defaults[col]));
            table->selectRow(row);
            enable();
        });
        QObject::connect(remove, &QPushButton::clicked, &dialog, [=] {
            if (table->rowCount())
                table->removeRow(table->currentRow() < 0 ? table->rowCount() - 1 : table->currentRow());
            enable();
        });
        tables[key] = table;
        box->addWidget(table);
        tabs->addTab(widget, viewport ? "Viewports" : "Scissors");
    }
    auto error = new QLabel;
    error->setObjectName("rasterizerError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool changed = false;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            Json patch = Json::object();
            for (const auto &[key, widget] : fields) {
                Json value;
                if (auto combo = qobject_cast<QComboBox *>(widget)) {
                    if (combo->currentIndex() < 0)
                        throw std::runtime_error("Select a valid rasterizer value");
                    value = initial["rasterizer"][key].is_boolean() ? Json(bool(combo->currentData().toInt()))
                                                                    : Json(combo->currentData().toInt());
                } else
                    value = Json::parse(qobject_cast<QLineEdit *>(widget)->text().toStdString());
                if (value != initial["rasterizer"][key])
                    patch["rasterizer"][key] = value;
            }
            for (const auto &[key, table] : tables) {
                auto rows = Json::array();
                for (int row = 0; row < table->rowCount(); ++row) {
                    auto values = Json::array();
                    for (int col = 0; col < table->columnCount(); ++col) {
                        auto cell = table->item(row, col);
                        if (!cell)
                            throw std::runtime_error("Complete all array cells");
                        values.push_back(Json::parse(cell->text().toStdString()));
                    }
                    rows.push_back(values);
                }
                if (rows != initial.at(key))
                    patch[key] = rows;
            }
            if (!patch.empty()) {
                normalizePipeline(patch);
                commit(patch);
                changed = true;
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    return dialog.exec() == QDialog::Accepted && changed;
}
} // namespace flora
