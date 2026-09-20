#include "PipelineSetterDialog.h"
#include "application/SetterEdits.h"
#include "core/Commands.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
namespace flora {
bool editPipelineSetterDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                              const std::function<void(const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("pipelineSetterDialog");
    dialog.setWindowTitle(QString::fromStdString(commandName(frame.entry(event).type)) +
                          QString(" — Event %1").arg(event));
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    std::map<std::string, QComboBox *> combos;
    std::map<std::string, QLineEdit *> numbers;
    QTableWidget *table = nullptr;
    std::string arrayKey;
    for (auto it = initial.begin(); it != initial.end(); ++it) {
        const auto key = it.key();
        const auto label = QString::fromStdString(key).replace('_', ' ');
        if (key == "topology" || key == "rasterizer" || key == "blend" || key == "depth_state") {
            auto box = new QComboBox;
            box->setObjectName(QString::fromStdString("ps_" + key));
            auto add = [&](Id id, const QString &name) {
                box->addItem(name, QVariant::fromValue(qulonglong(id)));
            };
            if (key == "topology") {
                const std::map<Id, QString> names{{0, "Undefined"},
                                                  {1, "Point list"},
                                                  {2, "Line list"},
                                                  {3, "Line strip"},
                                                  {4, "Triangle list"},
                                                  {5, "Triangle strip"},
                                                  {10, "Line list adjacency"},
                                                  {11, "Line strip adjacency"},
                                                  {12, "Triangle list adjacency"},
                                                  {13, "Triangle strip adjacency"}};
                for (auto &[id, name] : names)
                    add(id, name);
                for (Id id = 33; id <= 64; ++id)
                    add(id, QString("Patch — %1 control points").arg(id - 32));
            } else {
                add(0, "Default");
                for (auto &[id, e] : frame.entries()) {
                    const bool matches = key == "rasterizer"
                                             ? (e.type == 0x89 || e.type == 0x10e || e.type == 0x10f)
                                         : key == "blend" ? (e.type == 0x8a || e.type == 0x10d)
                                                          : e.type == 0x8b;
                    if (e.category == 5 && matches)
                        add(id, QString("%1 %2").arg(label).arg(id));
                }
            }
            auto id = it.value().get<Id>();
            auto index = box->findData(QVariant::fromValue(qulonglong(id)));
            if (index < 0) {
                add(id, QString("Unknown %1").arg(id));
                index = box->count() - 1;
            }
            box->setCurrentIndex(index);
            form->addRow(label, box);
            combos[key] = box;
        } else if (key == "viewports_values" || key == "scissors_values" || key == "blend_factor") {
            arrayKey = key;
            const bool viewport = key == "viewports_values", factor = key == "blend_factor";
            table = new QTableWidget;
            table->setObjectName("ps_rows");
            table->setColumnCount(viewport ? 6 : 4);
            table->setHorizontalHeaderLabels(
                viewport ? QStringList{"X", "Y", "Width", "Height", "Min depth", "Max depth"}
                : factor ? QStringList{"R", "G", "B", "A"}
                         : QStringList{"Left", "Top", "Right", "Bottom"});
            table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            auto fill = [table](const Json &row) {
                auto n = table->rowCount();
                table->insertRow(n);
                for (int c = 0; c < table->columnCount(); ++c)
                    table->setItem(n, c,
                                   new QTableWidgetItem(row.at(c).is_number_float()
                                                            ? QString::number(row.at(c).get<double>(), 'g', 9)
                                                            : QString::fromStdString(row.at(c).dump())));
            };
            if (factor)
                fill(it.value());
            else
                for (const auto &row : it.value())
                    fill(row);
            if (!factor) {
                auto bar = new QHBoxLayout;
                auto add = new QPushButton("Add"), remove = new QPushButton("Remove");
                add->setObjectName("ps_add");
                remove->setObjectName("ps_remove");
                bar->addWidget(add);
                bar->addWidget(remove);
                bar->addStretch();
                layout->addLayout(bar);
                QObject::connect(add, &QPushButton::clicked, &dialog, [table, fill, viewport] {
                    if (table->rowCount() < 16)
                        fill(viewport ? Json{0, 0, 1, 1, 0, 1} : Json{0, 0, 1, 1});
                });
                QObject::connect(remove, &QPushButton::clicked, &dialog, [table] {
                    if (table->currentRow() >= 0)
                        table->removeRow(table->currentRow());
                });
            }
            layout->addWidget(table);
            dialog.resize(viewport ? 700 : 510, factor ? 230 : 360);
        } else {
            auto edit = new QLineEdit(QString::fromStdString(it.value().dump()));
            edit->setObjectName(QString::fromStdString("ps_" + key));
            edit->setToolTip("Unsigned 32-bit value; decimal or 0x hexadecimal.");
            form->addRow(label, edit);
            numbers[key] = edit;
        }
    }
    auto error = new QLabel;
    error->setObjectName("ps_error");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            Json values = initial;
            for (auto &[key, box] : combos)
                values[key] = uint64_t(box->currentData().toULongLong());
            for (auto &[key, edit] : numbers) {
                auto text = edit->text().trimmed();
                bool ok;
                auto value = text.toULongLong(&ok, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
                if (!ok || text.startsWith('-') || value > UINT32_MAX)
                    throw std::runtime_error("Enter an unsigned 32-bit value");
                values[key] = uint32_t(value);
            }
            if (table) {
                Json rows = Json::array();
                for (int r = 0; r < table->rowCount(); ++r) {
                    Json row = Json::array();
                    for (int c = 0; c < table->columnCount(); ++c) {
                        auto cell = table->item(r, c);
                        if (!cell)
                            throw std::runtime_error("Complete every array cell");
                        bool ok;
                        if (arrayKey == "scissors_values") {
                            auto v = cell->text().trimmed().toLongLong(&ok);
                            if (!ok || v < INT32_MIN || v > INT32_MAX)
                                throw std::runtime_error("Scissor coordinate requires int32");
                            row.push_back(v);
                        } else {
                            auto v = cell->text().trimmed().toDouble(&ok);
                            if (!ok)
                                throw std::runtime_error("Enter a finite float32 value");
                            row.push_back(v);
                        }
                    }
                    rows.push_back(row);
                }
                values[arrayKey] = arrayKey == "blend_factor" ? rows.at(0) : rows;
            }
            values = pipelineSetterValues(validatePipelineSetter(frame, event, values));
            commit(values);
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    dialog.setMinimumWidth(380);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace flora
