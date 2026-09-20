#include "OutputDialog.h"
#include "application/OutputEdits.h"
#include "core/Commands.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
namespace flora {
namespace {
using Json = nlohmann::json;
QStandardItemModel *resources(QObject *parent, const Frame &frame, uint16_t type, const QString &prefix) {
    auto model = new QStandardItemModel(parent);
    auto add = [&](Id id, const QString &label) {
        auto item = new QStandardItem(label);
        item->setData(QVariant::fromValue(qulonglong(id)), Qt::UserRole);
        model->appendRow(item);
    };
    add(0, "None");
    for (const auto &[id, entry] : frame.entries())
        if (entry.category == 5 && entry.type == type &&
            (type != 0x83 || (frame.resource(id).desc[2] & 0x10)))
            add(id, QString("%1 %2").arg(prefix).arg(id));
    return model;
}
void select(QComboBox *box, Id id) {
    auto value = QVariant::fromValue(qulonglong(id));
    int index = box->findData(value);
    if (index < 0) {
        box->addItem(QString("Missing %1").arg(id), value);
        index = box->count() - 1;
    }
    box->setCurrentIndex(index);
}
uint32_t number(const QString &text) {
    const auto value = text.trimmed();
    if (value.compare("KEEP", Qt::CaseInsensitive) == 0)
        return UINT32_MAX;
    bool valid = false;
    auto n = value.toULongLong(&valid, value.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
    if (!valid || value.startsWith('-') || n > UINT32_MAX)
        throw std::runtime_error("Enter a uint32 value or KEEP");
    return uint32_t(n);
}
class BindingPage final : public QWidget {
    std::string countKey_, arrayKey_, scalarKey_;
    QSpinBox *count_;
    QLineEdit *start_{};
    QCheckBox *keep_{}, *array_, *scalars_{};
    QTableWidget *table_;
    QStandardItemModel *model_;
    QComboBox *dsv_{};
    void sync() {
        const bool kept = keep_ && keep_->isChecked();
        count_->setEnabled(!kept);
        array_->setEnabled(!kept);
        if (scalars_)
            scalars_->setEnabled(!kept);
        table_->setEnabled(!kept);
        while (table_->rowCount() > count_->value())
            table_->removeRow(table_->rowCount() - 1);
        while (table_->rowCount() < count_->value()) {
            int row = table_->rowCount();
            table_->insertRow(row);
            auto slot = new QTableWidgetItem;
            slot->setFlags(Qt::ItemIsEnabled);
            table_->setItem(row, 0, slot);
            auto resource = new QComboBox;
            resource->setModel(model_);
            resource->setMinimumContentsLength(16);
            resource->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            table_->setCellWidget(row, 1, resource);
            if (scalars_) {
                auto value = new QLineEdit("KEEP");
                value->setToolTip(
                    "Unsigned 32-bit value, decimal or 0x hexadecimal; KEEP preserves the current value.");
                table_->setCellWidget(row, 2, value);
            }
        }
        for (int row = 0; row < table_->rowCount(); ++row) {
            bool valid = true;
            uint32_t start = 0;
            try {
                if (start_)
                    start = number(start_->text());
            } catch (const std::exception &) {
                valid = false;
            }
            table_->item(row, 0)->setText(valid && uint64_t(start) + row < 64 ? QString::number(start + row)
                                                                              : QString("—"));
            table_->cellWidget(row, 1)->setEnabled(array_->isChecked());
            if (scalars_)
                table_->cellWidget(row, 2)->setEnabled(scalars_->isChecked());
        }
    }

  public:
    BindingPage(const Frame &frame, const Json &initial, const std::string &kind, bool allowKeep) {
        const bool so = kind == "so", uav = kind == "uav";
        countKey_ = so ? "count" : kind + "_count";
        arrayKey_ = so ? "buffers" : kind + "s";
        scalarKey_ = so ? "offsets" : uav ? "initial_counts" : "";
        auto layout = new QVBoxLayout(this);
        auto form = new QFormLayout;
        layout->addLayout(form);
        if (uav) {
            start_ = new QLineEdit(QString::number(initial.at("start_slot").get<uint32_t>()));
            start_->setObjectName("start_slot");
            start_->setToolTip("UAV start slot; ignored when UAV bindings are kept.");
            form->addRow("Start slot", start_);
        }
        auto countRow = new QWidget;
        auto countLayout = new QHBoxLayout(countRow);
        countLayout->setContentsMargins(0, 0, 0, 0);
        count_ = new QSpinBox;
        count_->setObjectName(QString::fromStdString(countKey_));
        count_->setRange(0, so ? 4 : uav ? 64 : 8);
        auto n = initial.at(countKey_).get<uint32_t>();
        count_->setValue(n == UINT32_MAX ? 0 : int(n));
        countLayout->addWidget(count_);
        if (allowKeep) {
            keep_ = new QCheckBox("Keep bindings");
            keep_->setObjectName(QString::fromStdString(kind + "_keep"));
            keep_->setChecked(n == UINT32_MAX);
            countLayout->addWidget(keep_);
        }
        form->addRow("Count", countRow);
        array_ = new QCheckBox("Provided");
        array_->setObjectName(QString::fromStdString(arrayKey_ + "_provided"));
        array_->setToolTip(
            "Unchecked encodes a null array pointer. A provided zero-length array stays distinct.");
        array_->setChecked(!initial.at(arrayKey_).is_null());
        form->addRow(so ? "Buffers" : "Views", array_);
        if (!scalarKey_.empty()) {
            scalars_ = new QCheckBox("Provided");
            scalars_->setObjectName(QString::fromStdString(scalarKey_ + "_provided"));
            scalars_->setChecked(!initial.at(scalarKey_).is_null());
            scalars_->setToolTip(so ? "Unchecked preserves the current SO offsets."
                                    : "Unchecked preserves UAV counters.");
            form->addRow(so ? "Offsets" : "Initial counts", scalars_);
        }
        if (!so && !uav) {
            dsv_ = new QComboBox;
            dsv_->setObjectName("dsv");
            dsv_->setModel(resources(this, frame, 0x8e, "DSV"));
            select(dsv_, initial.at("dsv").get<Id>());
            form->addRow("Depth / stencil", dsv_);
        }
        model_ = resources(this, frame, so ? 0x83 : uav ? 0x8f : 0x8d, so ? "Buffer" : uav ? "UAV" : "RTV");
        table_ = new QTableWidget(0, scalars_ ? 3 : 2);
        table_->setObjectName(QString::fromStdString(kind + "Bindings"));
        QStringList headers{"Slot", so ? "Buffer" : "View"};
        if (scalars_)
            headers << (so ? "Byte offset" : "Initial count");
        table_->setHorizontalHeaderLabels(headers);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        if (scalars_)
            table_->setColumnWidth(2, 130);
        layout->addWidget(table_);
        sync();
        for (int row = 0; row < table_->rowCount(); ++row) {
            if (array_->isChecked())
                select(qobject_cast<QComboBox *>(table_->cellWidget(row, 1)),
                       initial.at(arrayKey_).at(row).get<Id>());
            if (scalars_ && scalars_->isChecked()) {
                auto value = initial.at(scalarKey_).at(row).get<uint32_t>();
                qobject_cast<QLineEdit *>(table_->cellWidget(row, 2))
                    ->setText(value == UINT32_MAX ? "KEEP" : QString::number(value));
            }
        }
        connect(count_, &QSpinBox::valueChanged, this, [this] { sync(); });
        connect(array_, &QCheckBox::toggled, this, [this] { sync(); });
        if (start_)
            connect(start_, &QLineEdit::textChanged, this, [this] { sync(); });
        if (keep_)
            connect(keep_, &QCheckBox::toggled, this, [this] { sync(); });
        if (scalars_)
            connect(scalars_, &QCheckBox::toggled, this, [this] { sync(); });
    }
    void collect(Json &out) const {
        const bool kept = keep_ && keep_->isChecked();
        out[countKey_] = kept ? UINT32_MAX : uint32_t(count_->value());
        if (start_)
            out["start_slot"] = number(start_->text());
        if (dsv_)
            out["dsv"] = uint64_t(dsv_->currentData().toULongLong());
        out[arrayKey_] = !kept && array_->isChecked() ? Json::array() : Json(nullptr);
        if (scalars_)
            out[scalarKey_] = !kept && scalars_->isChecked() ? Json::array() : Json(nullptr);
        if (!kept)
            for (int row = 0; row < table_->rowCount(); ++row) {
                if (array_->isChecked())
                    out[arrayKey_].push_back(uint64_t(
                        qobject_cast<QComboBox *>(table_->cellWidget(row, 1))->currentData().toULongLong()));
                if (scalars_ && scalars_->isChecked())
                    out[scalarKey_].push_back(
                        number(qobject_cast<QLineEdit *>(table_->cellWidget(row, 2))->text()));
            }
    }
};
} // namespace
bool editOutputSetterDialog(QWidget *parent, const Frame &frame, Id event, const Json &initial,
                            const std::function<void(const Json &)> &commit) {
    QDialog dialog(parent);
    dialog.setObjectName("outputSetterDialog");
    dialog.setWindowTitle(QString::fromStdString(commandName(frame.entry(event).type)) +
                          QString(" — Event %1").arg(event));
    dialog.resize(580, 480);
    auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget;
    layout->addWidget(tabs);
    std::vector<BindingPage *> pages;
    auto add = [&](const std::string &kind, const QString &title, bool keep) {
        auto page = new BindingPage(frame, initial, kind, keep);
        tabs->addTab(page, title);
        pages.push_back(page);
    };
    if (initial.contains("rtv_count"))
        add("rtv", "Render Targets", frame.entry(event).type == 0x3500);
    if (initial.contains("uav_count"))
        add("uav", "Unordered Access", frame.entry(event).type == 0x3500);
    if (initial.contains("buffers"))
        add("so", "Stream Output", false);
    auto error = new QLabel;
    error->setObjectName("setterError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    for (auto edit : dialog.findChildren<QLineEdit *>())
        QObject::connect(edit, &QLineEdit::textChanged, error, &QWidget::hide);
    for (auto spin : dialog.findChildren<QSpinBox *>())
        QObject::connect(spin, &QSpinBox::valueChanged, error, &QWidget::hide);
    for (auto box : dialog.findChildren<QCheckBox *>())
        QObject::connect(box, &QCheckBox::toggled, error, &QWidget::hide);
    for (auto box : dialog.findChildren<QComboBox *>())
        QObject::connect(box, &QComboBox::currentIndexChanged, error, &QWidget::hide);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    bool changed = false;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            Json value = Json::object();
            for (auto page : pages)
                page->collect(value);
            if (value != initial) {
                validateOutputSetter(frame, event, value);
                commit(value);
                changed = true;
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    dialog.exec();
    return changed;
}
} // namespace flora
