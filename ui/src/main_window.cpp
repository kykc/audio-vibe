#include "main_window.h"

#include "window_chrome.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), host_(this), editors_(this) {
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
            [this](unsigned sampleRate, unsigned channelCount, int maxFrames) {
                log(QStringLiteral("chain built for %1 Hz x%2 ch, up to %3 frames")
                        .arg(sampleRate)
                        .arg(channelCount)
                        .arg(maxFrames));
                rack_->refresh();
            });
    connect(&host_, &EngineHost::chainFailed, this, [this](const QString& error) {
        log(QStringLiteral("chain not built: %1").arg(error));
        rack_->refresh();
    });
    connect(&editors_, &EditorManager::message, this, &MainWindow::log);
    connect(&editors_, &EditorManager::openCountChanged, this, [this] { rack_->refresh(); });
    connect(rack_, &RackPanel::message, this, &MainWindow::log);

    refreshEndpoints();
    updateStatus();
    resize(760, 620);
}

MainWindow::~MainWindow() {
    // Explicit, and in this order. An editor holds a plugin's IPlugView, which holds its
    // controller; the engine owns the instance behind it. Leaving this to Qt's child destruction
    // would run it after `host_` has already been destroyed.
    editors_.closeAll();
    host_.detach();
}

void MainWindow::applyStartupOptions(const QStringList& pluginPaths, bool openEditors,
                                     bool attach) {
    for (const QString& path : pluginPaths) {
        std::string error;
        if (!host_.engine().appendPlugin(path.toStdString(), error)) {
            log(QStringLiteral("could not load %1: %2").arg(path, QString::fromStdString(error)));
            continue;
        }
        log(QStringLiteral("loaded %1").arg(path));
    }
    rack_->refresh();

    if (openEditors) {
        for (std::size_t i = 0; i < host_.engine().pluginCount(); ++i) {
            if (engine::PluginInstance* plugin = host_.engine().pluginAt(i)) {
                editors_.open(*plugin, this);
            }
        }
    }

    if (attach) {
        // The default endpoint, which is what `refreshEndpoints` has already selected.
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
    updateStatus();
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

void MainWindow::updateStatus() {
    const EngineHost::Status status = host_.status();

    attachButton_->setText(status.attached ? QStringLiteral("Detach")
                                           : QStringLiteral("Attach"));
    endpointBox_->setEnabled(!status.attached);
    refreshButton_->setEnabled(!status.attached);

    QString link = QStringLiteral("%1").arg(QLatin1String(linkStateName(status.linkState)));
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
        text += QStringLiteral("audio thread: allocations %1   frees %2   locks %3")
                    .arg(status.violations.allocations)
                    .arg(status.violations.deallocations)
                    .arg(status.violations.locks);
    } else {
        text += QStringLiteral("audio thread: violation detector compiled out (Release)");
    }
    countersLabel_->setText(text);
}

void MainWindow::log(const QString& text) {
    logView_->appendPlainText(
        QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                                     text));
}

} // namespace aip::ui
