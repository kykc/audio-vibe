#include "plugin_picker.h"

#include "aip/engine/plugin_module.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace aip::ui {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;

class PluginPicker final : public QDialog {
public:
    explicit PluginPicker(QWidget* parent) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Add plugin"));
        resize(560, 460);

        auto* layout = new QVBoxLayout(this);

        layout->addWidget(new QLabel(
            QStringLiteral("Installed VST3 plugins, from the standard search locations."), this));

        filter_ = new QLineEdit(this);
        filter_->setPlaceholderText(QStringLiteral("Filter"));
        filter_->setClearButtonEnabled(true);
        layout->addWidget(filter_);

        list_ = new QListWidget(this);
        layout->addWidget(list_, 1);

        auto* browse = new QPushButton(QStringLiteral("Browse for a .vst3 bundle..."), this);
        layout->addWidget(browse);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add"));
        layout->addWidget(buttons);

        for (const std::string& path : engine::PluginModule::installedModulePaths()) {
            const QString qpath = QString::fromStdString(path);
            auto* item = new QListWidgetItem(QFileInfo(qpath).completeBaseName(), list_);
            item->setData(kPathRole, qpath);
            item->setToolTip(qpath);
        }
        if (list_->count() == 0) {
            list_->setEnabled(false);
            list_->addItem(QStringLiteral("(none found -- use Browse)"));
        }

        connect(filter_, &QLineEdit::textChanged, this, &PluginPicker::applyFilter);
        connect(list_, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(browse, &QPushButton::clicked, this, &PluginPicker::browse);
    }

    [[nodiscard]] QString chosen() const {
        if (!browsed_.isEmpty()) {
            return browsed_;
        }
        const QListWidgetItem* item = list_->currentItem();
        if (item == nullptr) {
            return {};
        }
        return item->data(kPathRole).toString();
    }

private:
    void applyFilter(const QString& text) {
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem* item = list_->item(i);
            item->setHidden(!text.isEmpty() &&
                            !item->text().contains(text, Qt::CaseInsensitive));
        }
    }

    void browse() {
        // Directory mode, because a bundle is a directory. `DontUseNativeDialog` is what makes a
        // directory named `X.vst3` selectable rather than merely enterable.
        QFileDialog dialog(this, QStringLiteral("Select a .vst3 bundle"));
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setOption(QFileDialog::ShowDirsOnly);
        const QString common = qEnvironmentVariable("CommonProgramFiles");
        if (!common.isEmpty()) {
            dialog.setDirectory(common + QStringLiteral("/VST3"));
        }
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
            return;
        }
        browsed_ = dialog.selectedFiles().front();
        accept();
    }

    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QString browsed_;
};

} // namespace

QString choosePluginPath(QWidget* parent) {
    PluginPicker picker(parent);
    if (picker.exec() != QDialog::Accepted) {
        return {};
    }
    return picker.chosen();
}

} // namespace aip::ui
