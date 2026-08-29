// Which render endpoints this project's APO is installed on, and the way to change that.
//
// The shell already greys out every endpoint the APO is not on and tells the user to "install it
// on the device you want to process" (`MainWindow::refreshEndpoints`) -- and then offers no way
// to. This is that way. The predecessor shipped the same dialog as a separate executable that its
// installer ran once (`Config/ConfigForm.cs` in the 2013 tree); it is in the shell here because a
// fresh machine has the APO on nothing, and the moment the user finds that out is while they are
// looking at the greyed-out list.
//
// **This is the one dialog laid out in a `.ui` file** rather than in C++, and the first of them.
// It is also the first form in the project dense enough for that to be worth anything: a table,
// a status line and three buttons, none of which is built conditionally.
//
// Two things it does not do, both deliberate:
//
//   - **It does not register the DLL.** Putting a CLSID in an endpoint's effect chain and making
//     that CLSID loadable are separate acts with different blast radii (README, `apo/README.md`),
//     and this dialog does only the first. An endpoint whose slot names an unregistered DLL is
//     inert rather than broken.
//   - **It does not write the registry itself.** The shell is unelevated and stays that way; it
//     spawns `apo_admin.exe` elevated for the mutation, which is also what keeps the backup, the
//     modern-slot policy and the service restart in exactly one place.

#pragma once

#include "aip/ipc/endpoints.h"

#include <QDialog>

#include <memory>
#include <vector>

class QTreeWidgetItem;

namespace Ui {
class AudioDeviceDialog;
}

namespace aip::ui {

class AudioDeviceDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AudioDeviceDialog(QWidget* parent = nullptr);
    ~AudioDeviceDialog() override;

signals:
    /// Progress and outcomes, for the shell's log view. The dialog says nothing to the console --
    /// there isn't one -- and its own status line only holds a sentence.
    void message(const QString& text);

    /// An endpoint's registration actually changed. The shell re-enumerates on this: the combo
    /// box, the Attach button and the session-restore policy all read `ApoRegistration`, and all
    /// of them are stale the moment this fires.
    void registrationChanged();

protected:
    /// Asks before throwing away ticks that have not been applied.
    void reject() override;

private:
    /// One row per active render endpoint, tick state from the registry. Called on open, on
    /// Refresh, and after every Apply.
    void reload();

    void onItemChanged(QTreeWidgetItem* item, int column);
    void onApply();

    /// Runs `apo_admin` elevated for one direction. `guids` is never empty and never implicit:
    /// `apo_admin` with no `--endpoint` means *every* endpoint on the machine.
    bool runApoAdmin(bool install, const QStringList& guids, bool restartAudio);

    /// How many rows differ from what the registry says. Drives the Apply button.
    [[nodiscard]] int pendingCount() const;

    std::unique_ptr<Ui::AudioDeviceDialog> ui_;

    /// The endpoints behind the rows, in row order.
    std::vector<ipc::RenderEndpoint> endpoints_;

    /// Set while `reload` is filling the tree, so the check-state changes it makes itself do not
    /// count as the user asking for anything.
    bool populating_ = false;
};

} // namespace aip::ui
