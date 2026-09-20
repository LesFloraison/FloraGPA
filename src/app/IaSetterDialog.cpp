#include "IaSetterDialog.h"
#include "application/SetterEdits.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
namespace flora {
bool editIaSetterDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                        const std::function<void(const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("iaSetterDialog");
    dialog.setWindowTitle(QString::fromStdString(commandName(frame.entry(event).type)) +
                          QString(" — Event %1").arg(event));
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    const bool vertex = initial.contains("buffers"), index = initial.contains("ib");
    auto resource = [&](Id selected, uint32_t flag) {
        auto box = new QComboBox;
        box->addItem("None", QVariant::fromValue(qulonglong(0)));
        for (const auto &[id, e] : frame.entries())
            if (e.category == 5 && e.type == (flag ? 0x83 : 0x82)) {
                if (flag && !(frame.resource(id).desc.at(2) & flag))
                    continue;
                box->addItem(QString(flag ? "Buffer %1" : "Layout %1").arg(id),
                             QVariant::fromValue(qulonglong(id)));
            }
        auto n = box->findData(QVariant::fromValue(qulonglong(selected)));
        if (n < 0) {
            box->addItem(QString("Missing %1").arg(selected), QVariant::fromValue(qulonglong(selected)));
            n = box->count() - 1;
        }
        box->setCurrentIndex(n);
        box->setToolTip(box->currentText());
        QObject::connect(box, &QComboBox::currentTextChanged, box,
                         [box](const QString &text) { box->setToolTip(text); });
        return box;
    };
    QComboBox *single = nullptr, *format = nullptr;
    QLineEdit *offset = nullptr;
    QSpinBox *start = nullptr;
    QTableWidget *rows = nullptr;
    if (vertex) {
        start = new QSpinBox;
        start->setObjectName("ia_start");
        start->setRange(0, 31);
        start->setValue(initial.at("start_slot").get<int>());
        form->addRow("Start slot", start);
        rows = new QTableWidget;
        rows->setObjectName("ia_rows");
        rows->setColumnCount(3);
        rows->setHorizontalHeaderLabels({"Buffer", "Stride", "Offset"});
        rows->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        auto addRow = [&, rows](Id id, uint32_t stride, uint32_t off) {
            int n = rows->rowCount();
            rows->insertRow(n);
            rows->setCellWidget(n, 0, resource(id, 1));
            rows->setItem(n, 1, new QTableWidgetItem(QString::number(stride)));
            rows->setItem(n, 2, new QTableWidgetItem(QString::number(off)));
        };
        for (size_t i = 0; i < initial.at("buffers").size(); ++i)
            addRow(initial.at("buffers")[i], initial.at("strides")[i], initial.at("offsets")[i]);
        auto bar = new QHBoxLayout;
        auto add = new QPushButton("Add"), remove = new QPushButton("Remove");
        add->setObjectName("ia_add");
        remove->setObjectName("ia_remove");
        bar->addWidget(add);
        bar->addWidget(remove);
        bar->addStretch();
        layout->addLayout(bar);
        layout->addWidget(rows);
        QObject::connect(add, &QPushButton::clicked, &dialog, [=] {
            if (rows->rowCount() < 32 - start->value())
                addRow(0, 0, 0);
        });
        QObject::connect(remove, &QPushButton::clicked, &dialog, [rows] {
            if (rows->currentRow() >= 0)
                rows->removeRow(rows->currentRow());
        });
        dialog.resize(610, 360);
    } else {
        single = resource(initial.at(index ? "ib" : "input_layout"), index ? 2 : 0);
        single->setObjectName("ia_resource");
        form->addRow(index ? "Index buffer" : "Input layout", single);
        if (index) {
            format = new QComboBox;
            format->setObjectName("ia_format");
            format->addItem("Unknown", 0);
            format->addItem("R16_UINT", 57);
            format->addItem("R32_UINT", 42);
            auto value = initial.at("ib_format").get<uint32_t>();
            auto n = format->findData(value);
            if (n < 0) {
                format->addItem(QString::number(value), value);
                n = format->count() - 1;
            }
            format->setCurrentIndex(n);
            form->addRow("Format", format);
            offset = new QLineEdit(QString::number(initial.at("ib_offset").get<uint32_t>()));
            offset->setObjectName("ia_offset");
            form->addRow("Offset", offset);
        }
    }
    auto error = new QLabel;
    error->setObjectName("ia_error");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    if (rows)
        QObject::connect(rows, &QTableWidget::itemChanged, error, [error] { error->hide(); });
    if (start)
        QObject::connect(start, &QSpinBox::valueChanged, error, [error] { error->hide(); });
    if (offset)
        QObject::connect(offset, &QLineEdit::textChanged, error, [error] { error->hide(); });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto integer = [](QString text) {
                text = text.trimmed();
                bool ok;
                auto v = text.toULongLong(&ok, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
                if (!ok || text.startsWith('-') || v > UINT32_MAX)
                    throw std::runtime_error("Enter an unsigned 32-bit value");
                return uint32_t(v);
            };
            Json values = initial;
            if (vertex) {
                values["start_slot"] = start->value();
                for (auto key : {"buffers", "strides", "offsets"})
                    values[key] = Json::array();
                for (int row = 0; row < rows->rowCount(); ++row) {
                    values["buffers"].push_back(uint64_t(
                        qobject_cast<QComboBox *>(rows->cellWidget(row, 0))->currentData().toULongLong()));
                    for (int col = 1; col < 3; ++col) {
                        auto item = rows->item(row, col);
                        if (!item)
                            throw std::runtime_error("Complete every binding row");
                        values[col == 1 ? "strides" : "offsets"].push_back(integer(item->text()));
                    }
                }
            } else {
                values[index ? "ib" : "input_layout"] = uint64_t(single->currentData().toULongLong());
                if (index) {
                    values["ib_format"] = format->currentData().toUInt();
                    values["ib_offset"] = integer(offset->text());
                }
            }
            values = iaSetterValues(validateIaSetter(frame, event, values));
            commit(values);
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    dialog.setMinimumWidth(400);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace flora
