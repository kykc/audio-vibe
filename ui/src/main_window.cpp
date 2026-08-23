#include "main_window.h"

#include "process_footprint.h"
#include "qt_paths.h"
#include "window_chrome.h"

#include "aip/config/attach_guard.h"
#include "aip/config/load_guard.h"
#include "aip/config/session_file.h"

#include "aip/scanner/scan_result.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QEventLoop>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace aip::ui {

namespace {

const char* linkStateName(ipc::LinkState state) {
    switch (state) {
    case ipc::LinkState::Detached:
        return "detached";
    case ipc::LinkState::Attached:
        return "attached";
    case ipc::LinkState::Relinquished:
        return "relinquished";
    }
    return "?";
}

/// One turn of the message loop, in the middle of startup work that has not reached `exec()` yet.
///
/// `applyStartupOptions` runs after `show()`, so the window is on screen for the whole of it, and
/// loading a plugin module is the one step here that can take seconds -- a bundle that is not in
/// the file cache is disk-bound, and they run to tens of megabytes. A visible top-level
/// window whose thread has not answered a message for five seconds is replaced by the ghost
/// Windows paints itself: right size, right position, no contents. That is what "the window is
/// created at its saved size but never draws" was, and it is why two plugins showed it where one
/// did not -- one cold module load stayed under the five seconds, two did not.
///
/// Pumping once per plugin keeps the real window on screen and makes each log line arrive as its
/// plugin loads rather than all of them at the end. It cannot help *within* one slow load --
/// hosting is single-threaded here by design (sec. 7.2) -- so what is bounded is the list, not a
/// module. `PluginCatalog::run` pumps by hand for the same reason and says more about why keeping
/// the timers alive is wanted rather than merely tolerated.
///
/// User input stays excluded deliberately. Paints, timers and native session messages are all
/// wanted here; a click on Add or Remove is not, because the rack is half-built and the sequence
/// building it has no way of being told that it changed underneath.
void pumpOnce() {
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

/// Bytes as a number a person reads at a glance. MiB throughout and never scaled up to GiB: the
/// shell sits in the low hundreds, and staying in one unit is what makes two readings an hour
/// apart comparable without arithmetic -- which is the whole point of putting them on screen.
QString formatMebibytes(std::size_t bytes) {
    return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

/// Elapsed milliseconds as `h:mm:ss`. Hours are neither padded nor dropped: a soak is quoted in
/// hours, and `0:04:12` says at a glance that this run is not one yet.
QString formatUptime(qint64 elapsedMs) {
    const qint64 seconds = elapsedMs / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600)
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

const char* exitReasonName(ipc::ValetExitReason reason) {
    switch (reason) {
    case ipc::ValetExitReason::None:
        return "none";
    case ipc::ValetExitReason::Stopped:
        return "stopped";
    case ipc::ValetExitReason::Stolen:
        return "stolen by another client";
    case ipc::ValetExitReason::Failed:
        return "the king went away";
    }
    return "?";
}

} // namespace

MainWindow::MainWindow(const QString& configPath, QWidget* parent)
    : QMainWindow(parent), host_(this), editors_(this), sessionPath_(toPath(configPath)) {
    // First statement in the body, so that everything a start costs -- the session, the plugin
    // modules, the editors -- is inside the figure the counters show.
    uptime_.start();

    // Deliberately blank. Repeating the application's own name back at the user in its own title
    // bar is a habit modern Windows applications have dropped, and with the icon gone too the
    // caption is left as nothing but its buttons.
    //
    // Note what this also does: with a standard frame the caption text *is* the window title, so
    // the taskbar and Alt-Tab labels go blank with it. There is no way to separate the two without
    // drawing the title bar ourselves. The application is identified by its icon instead, which is
    // why `aip_ui.rc` exists. Plugin editors keep their titles -- with several open at once, the
    // plugin's name is the only thing telling them apart.
    setWindowTitle(QString());
    hideTitleBarIcon(*this);
    // After the icon call, because that is what forces the native window into existence -- and
    // Qt fills an empty title in with the application name at exactly that moment.
    clearTitleText(*this);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->addWidget(buildLinkGroup());
    layout->addWidget(buildRackGroup(), 1);
    layout->addWidget(buildCountersGroup());
    setCentralWidget(central);

    connect(&host_, &EngineHost::serviced, this, &MainWindow::updateStatus);
    connect(&host_, &EngineHost::linkStateChanged, this, &MainWindow::onLinkStateChanged);
    connect(&host_, &EngineHost::chainBuilt, this,
            [this](unsigned sampleRate, unsigned channelCount, int maxFrames, bool speculative) {
                log(QStringLiteral("chain built for %1 Hz x%2 ch, up to %3 frames%4")
                        .arg(sampleRate)
                        .arg(channelCount)
                        .arg(maxFrames)
                        .arg(speculative
                                 ? QStringLiteral(" (from the endpoint's configured format; no "
                                                  "block seen yet)")
                                 : QString()));
                logWarmUp();
                rack_->refresh();
            });
    connect(&host_, &EngineHost::chainFailed, this, [this](const QString& error) {
        log(QStringLiteral("chain not built: %1").arg(error));
        rack_->refresh();
    });
    // A plugin that moved its own values -- a preset loaded inside it, usually. Only the editors
    // the shell drew itself can be showing something stale; a plugin's own view heard about it
    // before we did.
    connect(&host_, &EngineHost::pluginParametersChanged, this, [this] {
        editors_.refreshValues();
    });
    // Said out loud, all of it. A restart that rebuilt the rack is a real interruption to the
    // audio and belongs in the log next to whatever the user did to cause it; a restart that
    // asked for something this host does not do is worth more than silence, because silence is
    // indistinguishable from the request never arriving.
    connect(&host_, &EngineHost::pluginRestarted, this,
            [this](bool reconfigured, const QString& unhandled, const QString& error) {
                if (reconfigured) {
                    log(QStringLiteral("a plugin asked to be restarted: rack re-prepared at "
                                       "the same format"));
                    logWarmUp();
                    rack_->refresh();
                }
                if (!error.isEmpty()) {
                    log(QStringLiteral("a plugin asked to be restarted, and it failed: %1")
                            .arg(error));
                }
                if (!unhandled.isEmpty()) {
                    log(QStringLiteral("a plugin reported a change this shell does not act on "
                                       "(%1)")
                            .arg(unhandled));
                }
            });
    // Directly connected, because it has to have happened by the time the message returns: a
    // process that is about to be taken away has no later.
    sessionEnd_ = new SessionEndFilter(this);
    connect(sessionEnd_, &SessionEndFilter::sessionEnding, this, [this] {
        // The mark first, because it is the cheap half and the one whose absence causes the false
        // positive this whole mechanism exists to avoid. If saving were somehow to take the
        // process down with it, a mark already cleared is the better wreckage to leave behind:
        // announcing a crash after an ordinary reboot is the failure nobody believes twice.
        if (attachGuard_) {
            attachGuard_->clear();
        }
        saveSessionOnce();
    });
    // The shutdown was called off. The shell is still attached, so the mark goes back -- and the
    // save arms again, because a shutdown that is attempted, cancelled and attempted an hour later
    // should write what the rack looks like then rather than what it looked like the first time.
    connect(sessionEnd_, &SessionEndFilter::sessionEndCancelled, this, [this] {
        syncAttachMark();
        sessionEndSaved_ = false;
    });

    connect(&editors_, &EditorManager::message, this, &MainWindow::log);
    connect(&editors_, &EditorManager::openCountChanged, this, [this] { rack_->refresh(); });
    connect(rack_, &RackPanel::message, this, &MainWindow::log);
    // A preset replaces the chain, and the entries a session was told not to load belong to the
    // chain it replaced. Carrying them into the save would put a plugin the user has just
    // navigated away from back into a rack that never had it -- so they are dropped, and said
    // out loud, because one of them is a plugin somebody was meant to come back to.
    connect(rack_, &RackPanel::rackReplaced, this, [this] {
        if (blockedEntries_.empty()) {
            return;
        }
        log(QStringLiteral("%1 entry(s) that were not loaded went with the chain they came from")
                .arg(blockedEntries_.size()));
        blockedEntries_.clear();
    });

    refreshEndpoints();
    updateStatus();
    resize(760, 620);

    // Last, and deliberately so: it reports through the log view, selects an entry in the endpoint
    // combo box, and overrides the default size above -- none of which exist until here.
    loadSession();
}

MainWindow::~MainWindow() {
    // Explicit, and in this order. An editor holds a plugin's IPlugView, which holds its
    // controller; the engine owns the instance behind it. Leaving this to Qt's child destruction
    // would run it after `host_` has already been destroyed.
    editors_.closeAll();
    host_.detach();
}

void MainWindow::applyStartupOptions(const QStringList& pluginPaths, bool openEditors,
                                     bool attach, bool scan) {
    // First, and before anything can attach: the one thing here the user has to be told rather
    // than left to find in the log.
    if (uncleanAttach_.present) {
        reportUncleanAttach();
    }

    // Before the first load: this is where the window `show()` put on screen actually paints.
    pumpOnce();

    if (scan) {
        // What the first Add would have done, without needing anyone to click Add. The catalog is
        // the one part of the session whose cost is visible -- minutes on a machine with plugins
        // that hang -- so being able to fill it, close, and start again is what makes "the cache
        // is being used" checkable rather than asserted.
        rack_->catalog().ensureScanned(this);
        log(QStringLiteral("catalog: %1").arg(rack_->catalog().summary()));
    }

    for (const QString& path : pluginPaths) {
        std::string error;
        if (!host_.engine().appendPlugin(path.toStdString(), error)) {
            log(QStringLiteral("could not load %1: %2").arg(path, QString::fromStdString(error)));
            continue;
        }
        log(QStringLiteral("loaded %1").arg(path));
        // Per plugin rather than once at the end: a rack that fills in as it is built is the only
        // sign a slow start gives that it is progressing rather than stuck.
        rack_->refresh();
        pumpOnce();
    }
    rack_->refresh();

    if (openEditors) {
        for (std::size_t i = 0; i < host_.engine().pluginCount(); ++i) {
            if (engine::PluginInstance* plugin = host_.engine().pluginAt(i)) {
                editors_.open(*plugin, this);
                pumpOnce();
            }
        }
    }

    // `--attach` means the endpoint `refreshEndpoints` selected, which is the default one; the
    // session route means the endpoint it restored. Both end up here, and the guard matters --
    // toggleAttach() is a toggle, so calling it while attached would detach.
    if ((attach || sessionWantsAttach_) && !host_.attached()) {
        if (sessionWantsAttach_ && !attach) {
            log(QStringLiteral("reattaching: this session was attached when it closed"));
        }
        // The rack is complete and on screen before the endpoint is touched, so a chain build that
        // takes its time is watched from a window that shows what it is building.
        pumpOnce();
        toggleAttach();
    }
    updateStatus();
}

// ------------------------------------------------------------------------------------ the link

QWidget* MainWindow::buildLinkGroup() {
    auto* group = new QGroupBox(QStringLiteral("Link"), this);
    auto* layout = new QVBoxLayout(group);

    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(QStringLiteral("Endpoint:"), group));
    endpointBox_ = new QComboBox(group);
    endpointBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    endpointBox_->setMinimumContentsLength(36);
    row->addWidget(endpointBox_, 1);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), group);
    row->addWidget(refreshButton_);
    attachButton_ = new QPushButton(QStringLiteral("Attach"), group);
    row->addWidget(attachButton_);
    layout->addLayout(row);

    linkLabel_ = new QLabel(group);
    linkLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(linkLabel_);

    auto* note = new QLabel(
        QStringLiteral("Attaching processes every sound on the endpoint, system-wide. Blocks only "
                       "arrive while something is playing."),
        group);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshEndpoints);
    connect(attachButton_, &QPushButton::clicked, this, &MainWindow::toggleAttach);
    return group;
}

void MainWindow::refreshEndpoints() {
    if (host_.attached()) {
        // Re-enumerating while attached would let the combo box and the live supervisor disagree
        // about which endpoint is selected.
        log(QStringLiteral("detach before changing endpoint"));
        return;
    }

    endpoints_ = ipc::enumerateRenderEndpoints();
    endpointBox_->clear();
    int defaultIndex = 0;
    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        const ipc::RenderEndpoint& endpoint = endpoints_[i];
        QString label = QString::fromStdWString(endpoint.friendlyName);
        if (endpoint.isDefault) {
            label += QStringLiteral("  (default)");
            defaultIndex = static_cast<int>(i);
        }
        endpointBox_->addItem(label);
    }
    if (!endpoints_.empty()) {
        endpointBox_->setCurrentIndex(defaultIndex);
    }
    log(QStringLiteral("%1 active render endpoint(s)").arg(endpoints_.size()));
    updateStatus();
}

void MainWindow::toggleAttach() {
    if (host_.attached()) {
        host_.detach();
        log(QStringLiteral("detached"));
        syncAttachMark();
        updateStatus();
        return;
    }

    const int index = endpointBox_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= endpoints_.size()) {
        log(QStringLiteral("no endpoint selected"));
        return;
    }

    QString error;
    if (!host_.attach(endpoints_[static_cast<std::size_t>(index)], error)) {
        log(QStringLiteral("could not attach: %1").arg(error));
        return;
    }
    log(QStringLiteral("attaching to %1").arg(endpointBox_->currentText()));
    // Written before the first block can arrive, which is the point: what this defends against
    // faults on the audio thread, and by then there is nobody left to write anything down.
    syncAttachMark();
    updateStatus();
}

// ---------------------------------------------------------------------------------- the session

void MainWindow::loadSession() {
    // An explicit --config wins outright and is adopted whether or not the file is there: naming
    // a file that does not exist yet is how a session is started somewhere of the user's choosing.
    if (sessionPath_.empty()) {
        sessionPath_ = config::resolveLoadPath();
    }

    // Both marks live next to the session file, and both are wanted even when there is no file
    // there yet: a clean install that attaches and then dies has the same problem as any other
    // run, and the file it *would* be saved to is where the next start will come looking.
    const std::filesystem::path markRoot =
        sessionPath_.empty() ? config::resolveSavePath(sessionPath_) : sessionPath_;
    // Taken before the guard below can write over it, and taken rather than read: a mark that
    // outlived being acted on would suppress every reattach this shell ever made again.
    uncleanAttach_ = config::AttachGuard::takePrevious(markRoot);
    attachGuard_ = std::make_unique<config::AttachGuard>(markRoot);

    std::error_code ec;
    if (sessionPath_.empty() || !std::filesystem::exists(sessionPath_, ec)) {
        const std::filesystem::path target = config::resolveSavePath(sessionPath_);
        log(QStringLiteral("no session yet; this one will be saved to %1").arg(fromPath(target)));
        return;
    }

    config::Session session;
    std::string error;
    if (!config::readSession(sessionPath_, session, error)) {
        // Nothing is written for the rest of the run. See sessionSaveBlocked_: the file is the
        // only copy of the rack the user built, and saving an empty one over it on the way out
        // would destroy the thing they are about to try to recover.
        sessionSaveBlocked_ = true;
        log(QStringLiteral("session not loaded: %1").arg(QString::fromStdString(error)));
        log(QStringLiteral("nothing will be saved over it -- fix or delete the file"));
        return;
    }

    // The catalog before the rack, for two reasons. It is the expensive half of the file, so
    // adopting it is what stops the next Add from re-probing every plugin on the machine -- and it
    // is also what the next line consults to decide which rack entries are safe to load at all.
    rack_->catalog().adopt(session.catalog);
    blockDangerousEntries(session);

    // The breadcrumb lives next to the file being restored, so a start that never finishes leaves
    // the name of what it was loading behind for the next one.
    config::LoadGuard guard(sessionPath_);

    std::vector<std::string> problems;
    const std::size_t restored = config::apply(session, host_.engine(), problems, &guard);

    // Whatever was skipped is held so that saving can put it back where it was. Dropping it would
    // mean a plugin that crashed once vanishing from the user's chain with no record of why.
    blockedEntries_.clear();
    for (std::size_t i = 0; i < session.rack.size(); ++i) {
        if (session.rack[i].blocked) {
            blockedEntries_.emplace_back(i, session.rack[i]);
        }
    }
    for (const std::string& problem : problems) {
        log(QStringLiteral("session: %1").arg(QString::fromStdString(problem)));
    }
    rack_->refresh();
    log(QStringLiteral("session: %1 of %2 plugin(s) restored from %3")
            .arg(restored)
            .arg(session.rack.size())
            .arg(fromPath(sessionPath_)));
    if (!session.catalog.empty()) {
        log(QStringLiteral("session: %1 scanned plugin(s) remembered; the next Add re-probes only "
                           "what has changed")
                .arg(session.catalog.size()));
    }

    applySavedGeometry(session.window);
    const bool endpointStillHere = selectSavedEndpoint(session.endpointId, session.endpointName);
    // The policy is in `config/`, not here: what makes an automatic attach a bad idea is a
    // property of the session, the device list and how the last run ended, and none of the three
    // needs a window to be decided or tested.
    const config::ReattachDecision decision =
        config::shouldReattach(session, endpointStillHere, uncleanAttach_);
    sessionWantsAttach_ = decision.attach;
    if (!decision.reason.empty()) {
        log(QString::fromStdString(decision.reason));
    }
}

void MainWindow::syncAttachMark() {
    if (!attachGuard_) {
        return;
    }
    if (!host_.attached()) {
        attachGuard_->clear();
        return;
    }

    // The endpoint out of the list rather than the combo box's label, which carries a
    // "  (default)" suffix that is about this run's device list and not about the device.
    std::string name;
    const int index = endpointBox_->currentIndex();
    if (index >= 0 && static_cast<std::size_t>(index) < endpoints_.size()) {
        name = QString::fromStdWString(endpoints_[static_cast<std::size_t>(index)].friendlyName)
                   .toStdString();
    }
    attachGuard_->mark(name);
}

void MainWindow::reportUncleanAttach() {
    const QString endpoint = uncleanAttach_.endpointName.empty()
                                 ? QStringLiteral("an endpoint")
                                 : QString::fromStdString(uncleanAttach_.endpointName);
    log(QStringLiteral("the previous run was attached to %1 and did not shut down cleanly")
            .arg(endpoint));
    QMessageBox::warning(
        this, QStringLiteral("The last run did not shut down cleanly"),
        QStringLiteral(
            "This shell was attached to %1 when it last stopped, and it did not close normally."
            "\n\n"
            "A plugin that faults while it is processing takes the machine's audio with it, and "
            "attaching again on its own would do that every time the shell started -- so this "
            "start is detached. Press Attach when you are ready to try again."
            "\n\n"
            "If the machine lost power, or the shell was ended from Task Manager, nothing is "
            "wrong and there is nothing to do.")
            .arg(endpoint));
}

std::size_t MainWindow::blockDangerousEntries(config::Session& session) {
    // What the previous start was loading when it stopped. Taken before anything is loaded,
    // because the guard is about to write over it -- and taken rather than read, so that clearing
    // `blocked` in the file is enough to get the plugin tried again.
    const std::string casualty = config::LoadGuard::takePreviousCasualty(sessionPath_);

    std::vector<std::string> notes;
    const std::size_t blocked = config::blockUnsafeEntries(session, casualty,
                                                           rack_->catalog().modules(), notes);
    for (const std::string& note : notes) {
        log(QStringLiteral("session: %1").arg(QString::fromStdString(note)));
    }
    return blocked;
}

void MainWindow::saveSessionOnce() {
    if (sessionEndSaved_) {
        return;
    }
    sessionEndSaved_ = true;
    saveSession();
}

void MainWindow::saveSession() {
    if (sessionSaveBlocked_) {
        return;
    }

    const std::filesystem::path target = config::resolveSavePath(sessionPath_);

    config::Session session;
    config::capture(host_.engine(), session);
    session.catalog = rack_->catalog().snapshot();

    // Put back what was never loaded, at the position it had. `capture` can only see the rack the
    // engine holds, and a blocked entry is deliberately not in it -- but it is still part of the
    // chain the user built, and the file is where the reason it did not load is written down.
    for (const auto& [index, entry] : blockedEntries_) {
        const std::size_t at = std::min(index, session.rack.size());
        session.rack.insert(session.rack.begin() + static_cast<std::ptrdiff_t>(at), entry);
    }

    // normalGeometry(), not geometry(): for a maximized window the second one is the screen, and
    // saving that means un-maximizing after a restart puts the window back full-screen-sized.
    const QRect bounds = normalGeometry();
    session.window.x = bounds.x();
    session.window.y = bounds.y();
    session.window.width = bounds.width();
    session.window.height = bounds.height();
    session.window.maximized = isMaximized();

    const int index = endpointBox_->currentIndex();
    if (index >= 0 && static_cast<std::size_t>(index) < endpoints_.size()) {
        const ipc::RenderEndpoint& endpoint = endpoints_[static_cast<std::size_t>(index)];
        session.endpointId = QString::fromStdWString(endpoint.guid).toStdString();
        session.endpointName = QString::fromStdWString(endpoint.friendlyName).toStdString();
    }
    // Read from the host rather than from the button, because the link can end without anyone
    // pressing anything: another client takes the stream (sec. 4.1), or the king goes away. A
    // session that recorded "attached" after being displaced would reattach into a fight.
    session.attached = host_.attached();

    std::string error;
    if (!config::writeSession(target, session, error)) {
        log(QStringLiteral("session not saved: %1").arg(QString::fromStdString(error)));
    }
}

void MainWindow::applySavedGeometry(const config::WindowGeometry& geometry) {
    if (!geometry.valid()) {
        return;
    }

    const QRect bounds(geometry.x, geometry.y, geometry.width, geometry.height);
    // A saved position is only good while the screen it was on is still there. Restoring onto a
    // monitor that is not plugged in this time puts the window somewhere the user cannot reach it
    // and cannot see that they cannot reach it.
    if (QApplication::screenAt(bounds.center()) == nullptr) {
        log(QStringLiteral("saved window position is off-screen now; using the default"));
        return;
    }

    setGeometry(bounds);
    if (geometry.maximized) {
        // Before show(), so the geometry above becomes the normal one to fall back to when the
        // user un-maximizes rather than being replaced by it.
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

bool MainWindow::selectSavedEndpoint(const std::string& endpointId,
                                     const std::string& endpointName) {
    if (endpointId.empty()) {
        return false;
    }

    const QString wanted = QString::fromStdString(endpointId);
    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        if (QString::fromStdWString(endpoints_[i].guid) == wanted) {
            endpointBox_->setCurrentIndex(static_cast<int>(i));
            return true;
        }
    }

    // Left on the default, which refreshEndpoints() already selected. Worth a line: attaching is
    // one button press, and the user should not find out afterwards that it went somewhere else.
    log(QStringLiteral("the endpoint this session used (%1) is not available; the default is "
                       "selected instead")
            .arg(endpointName.empty() ? wanted : QString::fromStdString(endpointName)));
    return false;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSession();
    // Under its own power, so nothing that happens during teardown -- a plugin that faults on its
    // way out, say -- is an end the next start needs to be warned about.
    if (attachGuard_) {
        attachGuard_->clear();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::onLinkStateChanged(int state, int reason) {
    const auto linkState = static_cast<ipc::LinkState>(state);
    const auto exitReason = static_cast<ipc::ValetExitReason>(reason);
    log(QStringLiteral("link: %1 (%2)")
            .arg(QLatin1String(linkStateName(linkState)),
                 QLatin1String(exitReasonName(exitReason))));
    if (linkState == ipc::LinkState::Relinquished) {
        // Displacement is by design (sec. 4.1) and the supervisor does not fight for the stream.
        // Say so, because otherwise the client just looks broken.
        log(QStringLiteral("another client took the stream over; detach and attach to try again"));
    }
    syncAttachMark();
    updateStatus();
}

// ------------------------------------------------------------------------------------- the rack

QWidget* MainWindow::buildRackGroup() {
    auto* group = new QGroupBox(QStringLiteral("Rack"), this);
    auto* layout = new QVBoxLayout(group);
    rack_ = new RackPanel(host_, editors_, group);
    layout->addWidget(rack_);
    return group;
}

// --------------------------------------------------------------------------------- the counters

QWidget* MainWindow::buildCountersGroup() {
    auto* group = new QGroupBox(QStringLiteral("Counters"), this);
    auto* layout = new QVBoxLayout(group);

    countersLabel_ = new QLabel(group);
    countersLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(countersLabel_);

    logView_ = new QPlainTextEdit(group);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    logView_->setFixedHeight(120);
    layout->addWidget(logView_);
    return group;
}

void MainWindow::logWarmUp() {
    const engine::Engine::WarmUpReport& report = host_.engine().lastWarmUp();
    if (!report.ran()) {
        return;
    }

    QString text = QStringLiteral("warm-up: %1 block(s) through %2 plugin(s)")
                       .arg(report.blocks)
                       .arg(report.plugins);
    if (report.blocksFailed != 0) {
        text += QStringLiteral(", %1 refused").arg(report.blocksFailed);
    }
    log(text);

    // Deliberately not reported as a fact about the plugin: the detector replaces `operator new`
    // per image and a plugin is a DLL with its own, so what it can see here is our own processing
    // path. A nonzero count is therefore a defect on our side, and reads like one.
    const rt::ViolationCounts& v = report.violations;
    if (v.total() != 0) {
        log(QStringLiteral("warm-up: the host's own processing path performed %1 allocation(s), "
                           "%2 free(s) and %3 lock(s) -- that is a defect, not the plugin")
                .arg(v.allocations)
                .arg(v.deallocations)
                .arg(v.locks));
    }
}

void MainWindow::updateStatus() {
    const EngineHost::Status status = host_.status();

    // The rack list is only rebuilt on demand, and one of the things it shows -- whether the
    // format each plugin was prepared for is a guess or an observation -- changes without any
    // rack mutation to hang a refresh off. It flips exactly once per attach, when the first block
    // confirms the guess, so watching it here costs one comparison a tick.
    const bool speculative = host_.engine().builtFormatIsSpeculative();
    if (speculative != speculativeShown_) {
        speculativeShown_ = speculative;
        rack_->refresh();
    }

    attachButton_->setText(status.attached ? QStringLiteral("Detach")
                                           : QStringLiteral("Attach"));
    endpointBox_->setEnabled(!status.attached);
    refreshButton_->setEnabled(!status.attached);

    QString link = QStringLiteral("%1").arg(QLatin1String(linkStateName(status.linkState)));
    if (status.idle) {
        // The endpoint has gone quiet. Said here rather than left to be inferred from the
        // counters, because what the counters show is `timeouts` climbing ten a second next to
        // two counters that only move when something is wrong -- see EngineHost::Status::idle.
        link += QStringLiteral(", idle");
    }
    if (status.attached) {
        link += QStringLiteral("  --  %1").arg(status.endpointName);
        link += QStringLiteral("  --  %1 attach cycle(s)").arg(status.attachCycles);
    }
    if (status.counters.lastSampleRate != 0) {
        link += QStringLiteral("  --  last block %1 Hz x%2 ch, %3 frames")
                    .arg(status.counters.lastSampleRate)
                    .arg(status.counters.lastChannelCount)
                    .arg(status.counters.lastFrameCount);
    }
    linkLabel_->setText(link);

    // One long line per group rather than a grid: these numbers are read together or not at all,
    // and a grid of eighteen labels is harder to scan than three sentences.
    QString text;
    text += QStringLiteral("blocks %1 (%2/s)   timeouts %3   malformed %4   reclaims %5   "
                           "format changes %6\n")
                .arg(status.counters.blocks)
                .arg(status.blocksPerSecond, 0, 'f', 0)
                .arg(status.counters.timeouts)
                .arg(status.counters.malformedBlocks)
                .arg(status.counters.reclaims)
                .arg(status.counters.formatChanges);

    if (status.builtFormat.valid()) {
        text += QStringLiteral("chain %1 Hz x%2 ch: processed %3   passed through %4   "
                               "format misses %5\n")
                    .arg(status.builtFormat.sampleRate)
                    .arg(status.builtFormat.channelCount)
                    .arg(status.chainBlocks)
                    .arg(status.passedThrough)
                    .arg(status.formatMismatches);
    } else {
        text += QStringLiteral("chain: none built yet -- the format comes from the first block "
                               "(sec. 4.5)\n");
    }

    text += QStringLiteral("parameters delivered %1   dropped %2   plugin edits dropped %3   "
                           "stranded plugins %4\n")
                .arg(status.deliveredParameters)
                .arg(status.droppedParameters)
                .arg(status.droppedEdits)
                .arg(status.strandedPlugins);

    if constexpr (rt::checksEnabled()) {
        // The sec. 7.4.3 acceptance criterion, live. Anything but zero here is a defect, and it is
        // worth having on screen rather than only in a test.
        text += QStringLiteral("audio thread: allocations %1   frees %2   locks %3\n")
                    .arg(status.violations.allocations)
                    .arg(status.violations.deallocations)
                    .arg(status.violations.locks);
    } else {
        text += QStringLiteral("audio thread: violation detector compiled out (Release)\n");
    }

    // The other half of that criterion, and the reason it sits directly under it: "no allocations
    // on the audio thread" and "nothing is leaking anywhere" are different claims, and the line
    // above only makes the first (process_footprint.h). Uptime belongs beside them because a
    // resident set is not evidence of anything without knowing how long it took to get there --
    // 180 MiB after four minutes and 180 MiB after nine hours are opposite findings.
    //
    // Read every tick rather than sampled: it is one kernel call at 10 Hz, and a figure that
    // lagged the log lines beside it would be read against the wrong event.
    const ProcessFootprint footprint = processFootprint();
    text += QStringLiteral("process: resident %1   peak %2   up %3")
                .arg(formatMebibytes(footprint.residentBytes),
                     formatMebibytes(footprint.peakResidentBytes),
                     formatUptime(uptime_.elapsed()));

    countersLabel_->setText(text);
}

void MainWindow::log(const QString& text) {
    logView_->appendPlainText(
        QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                                     text));
}

} // namespace aip::ui
