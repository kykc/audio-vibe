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
// The reattach is conditional in two ways, and both are in `config::shouldReattach`. It happens
// only when the endpoint the session named is still present -- attaching to whatever device
// happens to be default now, because the one the user chose has been unplugged, would be taking
// over the wrong stream on their behalf. And it happens only when the previous run ended tidily:
// a run that vanished while attached is what a plugin faulting in `process` looks like, and
// reattaching into that is a boot loop that costs the machine's audio every time round
// (config/attach_guard.h). Either case selects the endpoint, says why in the log, and waits to be
// asked. The second also says so in a dialog, because a user who has just lost their sound to a
// crash should not have to read a log to find out that this start is deliberately quiet.

#pragma once

#include "editor_manager.h"
#include "engine_host.h"
#include "rack_panel.h"
#include "session_end_filter.h"

#include "aip/config/attach_guard.h"
#include "aip/config/session.h"
#include "aip/ipc/endpoints.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QStringList>

#include <cstddef>
#include <filesystem>
#include <memory>
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

    /// Reports what the engine's warm-up found, after a chain is built. Nothing when no plugin
    /// was warmed.
    void logWarmUp();

    /// Last value of `Engine::builtFormatIsSpeculative` the rack list was drawn with, so the
    /// "(expected)" qualifier disappears when the first block confirms the guess.
    bool speculativeShown_ = false;
    void log(const QString& text);

    void onLinkStateChanged(int state, int reason);

    /// Reads the session and puts the window into it: the rack, then the geometry, then the
    /// endpoint selection. Called at the end of construction, once there is a log view for it to
    /// report through.
    void loadSession();
    void saveSession();

    /// Writes down that the shell is attached, or that it is not, so that the next start can tell
    /// a run that ended from a run that stopped existing. Called wherever the attached state can
    /// have changed rather than only where it is changed on purpose -- the link can also end
    /// without anyone pressing anything (sec. 4.1) -- and marking twice with the same endpoint
    /// costs nothing.
    void syncAttachMark();

    /// Says, in a dialog rather than only in the log, that the previous run stopped existing
    /// while it was attached and that this one is therefore detached. Called once, after the
    /// window is on screen.
    void reportUncleanAttach();

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
    /// Watches for the message Windows sends before it ends the session, which is the difference
    /// between "the user rebooted" and "the shell died with the audio running through it".
    SessionEndFilter* sessionEnd_ = nullptr;
    /// The mark that says the shell is attached. Built in `loadSession`, because until then there
    /// is no session file for it to sit next to. Null before that and in a run with nowhere to
    /// write, which costs this run its protection and nothing else.
    std::unique_ptr<config::AttachGuard> attachGuard_;
    /// What the previous run left behind, taken before anything is marked. Held because the
    /// dialog that reports it cannot be raised until the window is on screen.
    config::UncleanAttach uncleanAttach_;
    /// What `config::shouldReattach` decided: the session was attached when it closed, and
    /// nothing about this start argues against doing it again. Acted
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

    /// How long this run has been going, for the counters. Started in the constructor rather than
    /// read from the process creation time: the difference is the few milliseconds Qt spends
    /// starting up, which nothing here is measuring, and this way the figure cannot outlive a
    /// window that was opened later than the process.
    QElapsedTimer uptime_;
};

} // namespace aip::ui
