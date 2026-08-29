#include "audio_device_dialog.h"

#include "apo_admin_runner.h"

#include "ui_audio_device_dialog.h"

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstddef>

namespace aip::ui {

namespace {

/// The row's endpoint, as an index into `endpoints_`. Stored on the item rather than taken from
/// the row's position: the tree is rebuilt from scratch on every reload, and a stale row number
/// would name a different device rather than no device at all.
constexpr int kEndpointRole = Qt::UserRole + 1;

/// What the registry says about one endpoint, in the words a person uses about a device.
QString presenceText(const ipc::RenderEndpoint& endpoint) {
    switch (endpoint.apo.presence) {
    case ipc::ApoPresence::Present:
        return endpoint.apo.slot.empty()
            ? QStringLiteral("installed")
            : QStringLiteral("installed (slot %1)").arg(QString::fromStdWString(endpoint.apo.slot));
    case ipc::ApoPresence::Absent:
        return QStringLiteral("not installed");
    case ipc::ApoPresence::Unknown:
        break;
    }
    return QStringLiteral("cannot be read");
}

} // namespace

AudioDeviceDialog::AudioDeviceDialog(QWidget* parent)
    : QDialog(parent), ui_(std::make_unique<Ui::AudioDeviceDialog>()) {
    ui_->setupUi(this);

    // Not in the .ui file, because Designer has no way to say it: the sensible width of the first
    // column is "whatever is left over", and of the second "as wide as its longest phrase".
    ui_->deviceTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui_->deviceTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    connect(ui_->refreshButton, &QPushButton::clicked, this, &AudioDeviceDialog::reload);
    connect(ui_->deviceTree, &QTreeWidget::itemChanged, this, &AudioDeviceDialog::onItemChanged);
    connect(ui_->buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
        if (ui_->buttonBox->standardButton(button) == QDialogButtonBox::Apply) {
            onApply();
        }
    });
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reload();
}

AudioDeviceDialog::~AudioDeviceDialog() = default;

void AudioDeviceDialog::reload() {
    populating_ = true;
    ui_->deviceTree->clear();

    // The same enumeration the endpoint combo box uses, so the two lists cannot disagree about
    // what is on the machine or about what is installed on it. Active endpoints only: a device
    // that is not there is not one a user is choosing to process.
    endpoints_ = ipc::enumerateRenderEndpoints();

    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        const ipc::RenderEndpoint& endpoint = endpoints_[i];

        auto* item = new QTreeWidgetItem(ui_->deviceTree);
        QString name = QString::fromStdWString(endpoint.friendlyName);
        if (endpoint.isDefault) {
            name += QStringLiteral("  (default)");
        }
        item->setText(0, name);
        item->setText(1, presenceText(endpoint));
        item->setData(0, kEndpointRole, static_cast<qulonglong>(i));

        // Always set, and written to be read: it explains an install as readily as a refusal
        // (`ipc/apo_registration.h`).
        const QString detail = QString::fromStdWString(endpoint.apo.detail);
        item->setToolTip(0, detail);
        item->setToolTip(1, detail);

        if (endpoint.apo.presence == ipc::ApoPresence::Unknown) {
            // Deliberately not tickable. A tick means "make this differ from what it is now", and
            // for this endpoint nobody would say what it is now -- so there is no change to ask
            // for. The tooltip carries which read failed.
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setDisabled(true);
            continue;
        }

        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, endpoint.apo.presence == ipc::ApoPresence::Present ? Qt::Checked : Qt::Unchecked);
    }

    populating_ = false;
    onItemChanged(nullptr, 0);

    if (endpoints_.empty()) {
        // Not necessarily a fault, and saying so is the whole point: Windows initialises a render
        // endpoint on first playback, so an enumeration taken while the machine is silent -- which
        // is exactly the state just after the audio service restarts -- can come back empty from a
        // machine whose sound card is perfectly healthy.
        ui_->statusLabel->setText(
            QStringLiteral("No active render endpoints. Windows creates them on first playback, so if the audio "
                           "service has just restarted, play something and press Refresh."));
    }
}

int AudioDeviceDialog::pendingCount() const {
    int pending = 0;
    for (int row = 0; row < ui_->deviceTree->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = ui_->deviceTree->topLevelItem(row);
        if ((item->flags() & Qt::ItemIsUserCheckable) == 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(item->data(0, kEndpointRole).toULongLong());
        if (index >= endpoints_.size()) {
            continue;
        }
        const bool installed = endpoints_[index].apo.presence == ipc::ApoPresence::Present;
        if ((item->checkState(0) == Qt::Checked) != installed) {
            ++pending;
        }
    }
    return pending;
}

void AudioDeviceDialog::onItemChanged(QTreeWidgetItem*, int) {
    if (populating_) {
        return;
    }

    const int pending = pendingCount();
    if (QPushButton* apply = ui_->buttonBox->button(QDialogButtonBox::Apply)) {
        apply->setEnabled(pending > 0);
    }
    if (endpoints_.empty()) {
        return;
    }
    ui_->statusLabel->setText(pending == 0
            ? QStringLiteral("No changes to apply.")
            : QStringLiteral("%1 device(s) to change. Apply needs administrator rights.").arg(pending));
}

void AudioDeviceDialog::onApply() {
    QStringList toInstall;
    QStringList toRemove;

    for (int row = 0; row < ui_->deviceTree->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = ui_->deviceTree->topLevelItem(row);
        if ((item->flags() & Qt::ItemIsUserCheckable) == 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(item->data(0, kEndpointRole).toULongLong());
        if (index >= endpoints_.size()) {
            continue;
        }
        const bool installed = endpoints_[index].apo.presence == ipc::ApoPresence::Present;
        const bool wanted = item->checkState(0) == Qt::Checked;
        if (wanted == installed) {
            continue;
        }
        // The GUID, never the row number. `apo_admin` numbers a *registry* walk that includes
        // disabled and unplugged endpoints; this list is MMDevice and active-only, so the two
        // index spaces do not correspond and an index passed across would name a different device.
        (wanted ? toInstall : toRemove).append(QString::fromStdWString(endpoints_[index].guid));
    }

    if (toInstall.isEmpty() && toRemove.isEmpty()) {
        return;
    }

    // `apo_admin` refuses `--install` and `--uninstall` in one run, so a batch containing both
    // costs two elevated launches, and Windows asks separately for each. Said before the first
    // prompt rather than discovered at the second.
    const bool bothDirections = !toInstall.isEmpty() && !toRemove.isEmpty();
    const QString question =
        QStringLiteral("Change %1 audio device(s)?\n\nThis needs administrator rights and restarts the "
                       "Windows audio service, so sound will cut out for a moment on every device. The "
                       "whole effect-chain registry is backed up first.%2")
            .arg(toInstall.size() + toRemove.size())
            .arg(bothDirections ? QStringLiteral("\n\nWindows will ask for administrator rights twice: "
                                                 "adding and removing cannot be done in one step.")
                                : QString());
    if (QMessageBox::question(this, QStringLiteral("Audio Device Settings"), question,
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }

    // The service restart goes on the last run only: it is the slow, disruptive part, and doing it
    // once after both halves is the same result for half the interruption.
    bool ok = true;
    if (!toRemove.isEmpty()) {
        ok = runApoAdmin(false, toRemove, toInstall.isEmpty());
    }
    if (ok && !toInstall.isEmpty()) {
        ok = runApoAdmin(true, toInstall, true);
    }

    reload();
    emit registrationChanged();
}

bool AudioDeviceDialog::runApoAdmin(bool install, const QStringList& guids, bool restartAudio) {
    emit message(QStringLiteral("%1 this project's APO on %2 endpoint(s); asking for administrator rights")
            .arg(install ? QStringLiteral("installing") : QStringLiteral("removing"))
            .arg(guids.size()));

    QStringList arguments{install ? QStringLiteral("--install") : QStringLiteral("--uninstall")};
    for (const QString& guid : guids) {
        arguments << QStringLiteral("--endpoint") << guid;
    }
    arguments << QStringLiteral("--yes");
    if (restartAudio) {
        arguments << QStringLiteral("--restart-audio");
    }

    // Two sentences: what is being done, and -- when it applies -- why the machine is about to go
    // quiet. The second is the one that stops a user reaching for the power button.
    const QString waitLabel = (install ? QStringLiteral("Adding this project's APO to %1 device(s)...")
                                       : QStringLiteral("Removing this project's APO from %1 device(s)..."))
                                  .arg(guids.size()) +
        (restartAudio ? QStringLiteral("\n\nRestarting the Windows audio service. Sound stops on every "
                                       "device for a few seconds.")
                      : QString());

    const apo_admin::Result result =
        apo_admin::run(this, QStringLiteral("Audio Device Settings"), arguments, waitLabel);
    for (const QString& line : result.messages) {
        emit message(line);
    }

    // The runner's own summary is about the run; this one is about the dialog, which has a list to
    // read again either way. A launch that never happened -- no tool, no elevation -- has nothing
    // to add to what it already said, so that sentence is passed straight through.
    if (result.messages.isEmpty() && !result.succeeded) {
        emit message(result.summary);
        ui_->statusLabel->setText(result.summary);
        return false;
    }
    ui_->statusLabel->setText(result.succeeded
            ? QStringLiteral("Done. The device list has been read again.")
            : QStringLiteral("apo_admin reported a problem -- see the log in the main window."));
    return result.succeeded;
}

void AudioDeviceDialog::reject() {
    if (pendingCount() > 0) {
        const auto answer = QMessageBox::question(this, QStringLiteral("Audio Device Settings"),
            QStringLiteral("There are changes you have not applied. Close and discard them?"),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Ok) {
            return;
        }
    }
    QDialog::reject();
}

} // namespace aip::ui
