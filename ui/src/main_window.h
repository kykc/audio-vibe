// The shell (design_doc.md sec. 5.1, sec. 7.2).
//
// One window, three things in it: which endpoint we are attached to, what is in the rack, and what
// the link is actually doing. The third is not decoration -- protocol v1 gives a client no way to
// ask how it is doing, so the counters are the only evidence that blocks are flowing, that none
// are being dropped, and that the audio thread is still allocation-free (sec. 7.4.3). A shell that
// hid them would make every real problem invisible.
//
// Attaching takes over the machine's audio for every application at once, and a client that
// stalls holds up `audiodg.exe` for up to a second (sec. 3.7.1) -- so the window says what it
// means before the user presses the button. It is asked for once, not once per launch: a session
// that was attached when it closed attaches again on the next start (project owner, 2026-08-22),
// because being attached is a state someone put this application into rather than a transient.
//
// The reattach is conditional in exactly one way. It happens only when the endpoint the session
// named is still present. Attaching to whatever device happens to be default now, because the one
// the user chose has been unplugged, would be taking over the wrong stream on their behalf -- so
// that case selects the default, says so, and waits to be asked.

#pragma once

#include "editor_manager.h"
#include "engine_host.h"
#include "rack_panel.h"

#include "aip/config/session.h"
#include "aip/ipc/endpoints.h"

#include <QMainWindow>
#include <QStringList>

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace aip::ui {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// `configPath` is what `--config` named, empty for the usual search (config/session_file.h).
    /// An explicit path overrides both locations, including the portable one, and is honoured
    /// even when the file is not there yet -- that is how a session is started somewhere else.
    explicit MainWindow(const QString& configPath = QString(), QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Puts the window into the state the command line asked for. Called after `show()`, because
    /// opening an editor needs a shown window to parent it to. Every step reports through the log
    /// view rather than the console -- this is a WIN32 executable and has no console to report to.
    void applyStartupOptions(const QStringList& pluginPaths, bool openEditors, bool attach,
                             bool scan);

protected:
    /// Where the session is written. Not the destructor: by then the window has no geometry left
    /// to record, and a failure has nowhere to be reported.
    void closeEvent(QCloseEvent* event) override;

private:
    void refreshEndpoints();
    void toggleAttach();
    void updateStatus();
    void log(const QString& text);

    void onLinkStateChanged(int state, int reason);

    /// Reads the session and puts the window into it: the rack, then the geometry, then the
    /// endpoint selection. Called at the end of construction, once there is a log view for it to
    /// report through.
    void loadSession();
    void saveSession();

    /// Marks the rack entries that must not be loaded, and says why in the log. Two sources: the
    /// breadcrumb the previous start left behind if it died mid-load, and the scan report, which
    /// already knows which modules crash or hang because a child process found out safely.
    /// Returns how many were blocked.
    std::size_t blockDangerousEntries(config::Session& session);

    /// Restores a saved geometry, unless it would put the window somewhere there is no longer a
    /// screen -- an external monitor that is not plugged in this time should not cost the user
    /// their window.
    void applySavedGeometry(const config::WindowGeometry& geometry);

    /// Selects the endpoint the session named. Returns false when it is no longer there, which
    /// is also what suppresses the reattach -- see the note at the top of this file.
    [[nodiscard]] bool selectSavedEndpoint(const std::string& endpointId,
                                           const std::string& endpointName);

    QWidget* buildLinkGroup();
    QWidget* buildRackGroup();
    QWidget* buildCountersGroup();

    // Declaration order is destruction order reversed, and it matters here: the editors must be
    // gone before the engine that owns the plugins behind them. `~MainWindow` closes them by hand
    // rather than trusting that, because Qt deletes child objects after member destruction, which
    // would be far too late.
    EngineHost host_;
    EditorManager editors_;

    /// Where the session was read from, and therefore where it goes back to. Empty means nothing
    /// was read -- a clean install -- and saving picks AppData (config/session_file.h).
    std::filesystem::path sessionPath_;
    /// Entries that were not loaded, with the rack position they had. Held so that saving can put
    /// them back: they are still part of the chain the user built, and the alternative is that a
    /// plugin which crashes once disappears from their setup without explanation.
    std::vector<std::pair<std::size_t, config::RackEntry>> blockedEntries_;
    /// The session was attached when it closed, *and* the endpoint it used is still here. Acted
    /// on in `applyStartupOptions` rather than in `loadSession`, because attaching before the
    /// window is shown would start the valet thread behind an invisible window -- and because it
    /// puts the automatic attach through the same one line as `--attach`.
    bool sessionWantsAttach_ = false;
    /// Set when a session file exists but could not be read. Nothing is written for the rest of
    /// the run: the file is the only copy of whatever the user had in it, and overwriting it with
    /// the empty rack its failure to parse left behind would destroy exactly what they want back.
    bool sessionSaveBlocked_ = false;

    std::vector<ipc::RenderEndpoint> endpoints_;

    QComboBox* endpointBox_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* attachButton_ = nullptr;
    QLabel* linkLabel_ = nullptr;
    RackPanel* rack_ = nullptr;
    QLabel* countersLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
};

} // namespace aip::ui
