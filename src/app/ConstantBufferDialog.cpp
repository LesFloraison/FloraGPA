#include "ConstantBufferDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
namespace flora {
bool editConstantBufferDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                              const std::function<void(const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("constantBufferDialog");
    dialog.setWindowTitle(QString::fromStdString(commandName(frame.entry(event).type)) +
                          QString(" — Event %1").arg(event));
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    auto start = new QSpinBox;
    start->setObjectName("cb_start");
    start->setRange(0, 13);
    start->setValue(initial.at("start_slot").get<int>());
    form->addRow("Start slot", start);
    auto window = new QCheckBox("Use constant range", &dialog);
    window->setObjectName("cb_window");
    window->setToolTip("First and count are measured in 16-byte constants; both must be multiples of 16.");
    const bool extended = initial.contains("first_constants");
    window->setChecked(
        extended && (!initial.at("first_constants").is_null() || !initial.at("constant_counts").is_null()));
    if (extended)
        form->addRow(window);
    else
        window->hide();
    layout->addLayout(form);
    auto rows = new QTableWidget;
    rows->setObjectName("cb_rows");
    rows->setColumnCount(3);
    rows->setHorizontalHeaderLabels({"Buffer", "First constant", "Constant count"});
    rows->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    auto error = new QLabel;
    error->setObjectName("cb_error");
    error->setWordWrap(true);
    error->hide();
    auto addRow = [&](Id selected, std::optional<uint32_t> first, std::optional<uint32_t> count) {
        int row = rows->rowCount();
        rows->insertRow(row);
        auto box = new QComboBox;
        box->addItem("None", QVariant::fromValue(qulonglong(0)));
        for (const auto &[id, e] : frame.entries())
            if (e.category == 5 && e.type == 0x83) {
                auto desc = frame.resource(id).desc;
                if (desc.at(2) == 4 && desc.at(0) && desc[0] % 16 == 0)
                    box->addItem(QString("Buffer %1").arg(id), QVariant::fromValue(qulonglong(id)));
            }
        auto n = box->findData(QVariant::fromValue(qulonglong(selected)));
        if (n < 0) {
            box->addItem(QString("Missing %1").arg(selected), QVariant::fromValue(qulonglong(selected)));
            n = box->count() - 1;
        }
        box->setCurrentIndex(n);
        box->setToolTip(box->currentText());
        QObject::connect(box, &QComboBox::currentTextChanged, error, [box, error](const QString &text) {
            box->setToolTip(text);
            error->hide();
        });
        rows->setCellWidget(row, 0, box);
        rows->setItem(row, 1, new QTableWidgetItem(first ? QString::number(*first) : QString()));
        rows->setItem(row, 2, new QTableWidgetItem(count ? QString::number(*count) : QString()));
    };
    auto initialRange = [&](const char *key, size_t row, uint32_t whole) -> std::optional<uint32_t> {
        if (!window->isChecked())
            return whole;
        const auto &values = initial.at(key);
        if (values.is_null())
            return {};
        return values.at(row).get<uint32_t>();
    };
    for (size_t i = 0; i < initial.at("buffers").size(); ++i)
        addRow(initial.at("buffers")[i], initialRange("first_constants", i, 0),
               initialRange("constant_counts", i, 4096));
    auto rangeColumns = [=] {
        rows->setColumnHidden(1, !window->isChecked());
        rows->setColumnHidden(2, !window->isChecked());
        error->hide();
    };
    rangeColumns();
    QObject::connect(window, &QCheckBox::toggled, &dialog, rangeColumns);
    auto bar = new QHBoxLayout;
    auto add = new QPushButton("Add"), remove = new QPushButton("Remove");
    add->setObjectName("cb_add");
    remove->setObjectName("cb_remove");
    bar->addWidget(add);
    bar->addWidget(remove);
    bar->addStretch();
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        if (rows->rowCount() < 14 - start->value()) {
            addRow(0, 0, 4096);
            error->hide();
        }
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [=] {
        if (rows->currentRow() >= 0) {
            rows->removeRow(rows->currentRow());
            error->hide();
        }
    });
    QObject::connect(start, &QSpinBox::valueChanged, error, [error] { error->hide(); });
    QObject::connect(rows, &QTableWidget::itemChanged, error, [error] { error->hide(); });
    layout->addLayout(bar);
    layout->addWidget(rows);
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            Json values{{"start_slot", start->value()}, {"buffers", Json::array()}};
            if (extended) {
                values["first_constants"] = nullptr;
                values["constant_counts"] = nullptr;
            }
            if (window->isChecked()) {
                values["first_constants"] = Json::array();
                values["constant_counts"] = Json::array();
            }
            for (int row = 0; row < rows->rowCount(); ++row) {
                values["buffers"].push_back(uint64_t(
                    qobject_cast<QComboBox *>(rows->cellWidget(row, 0))->currentData().toULongLong()));
                if (window->isChecked())
                    for (int col = 1; col < 3; ++col) {
                        auto item = rows->item(row, col);
                        if (!item)
                            throw std::runtime_error("Complete every binding row");
                        auto text = item->text().trimmed();
                        bool ok = false;
                        auto value =
                            text.toULongLong(&ok, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
                        if (!ok || text.startsWith('-') || value > UINT32_MAX)
                            throw std::runtime_error("Enter an unsigned 32-bit value");
                        values[col == 1 ? "first_constants" : "constant_counts"].push_back(uint32_t(value));
                    }
            }
            values = constantBufferSetterValues(validateConstantBufferSetter(frame, event, values));
            commit(values);
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    dialog.resize(620, 360);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace flora
