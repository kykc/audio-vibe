#include "engine_host.h"

#include <QTimer>

namespace aip::ui {

namespace {

/// Ten times a second. The chain can only be built once a block has revealed the format
/// (sec. 4.5), so this interval is how much audio goes through unprocessed on every attach -- one
/// or two blocks. It is also how long a knob in a plugin's editor can lag the audio, which is the
/// same reason to keep it short.
constexpr int kTickMs = 100;

} // namespace

EngineHost::EngineHost(QObject* parent) : QObject(parent) {
    auto* timer = new QTimer(this);
    timer->setInterval(kTickMs);
    connect(timer, &QTimer::timeout, this, &EngineHost::tick);
    timer->start();
    previousTick_ = std::chrono::steady_clock::now();
}

EngineHost::~EngineHost() {
    // The supervisor must stop before the engine goes: its valet thread is inside
    // ChainProcessor::processBlock, and the engine owns the chain that names the plugins.
    detach();
}

bool EngineHost::attach(const ipc::RenderEndpoint& endpoint, QString& error) {
    error.clear();
    if (endpoint.guid.empty()) {
        error = QStringLiteral("that endpoint reports no GUID, so its object names cannot be "
                               "derived (sec. 4.2)");
        return false;
    }
    detach();

    // Nothing below can fail: attaching is asynchronous by design. The supervisor starts detached
    // and stays that way until `audiodg.exe` has created the shared objects, which it only does
    // while something is playing on the endpoint.
    auto supervisor =
        std::make_unique<ipc::ValetSupervisor>(endpoint.guid, engine_.blockProcessor());
    supervisor->setStateCallback([this](ipc::LinkState state, ipc::ValetExitReason reason) {
        // Called on the supervisor's thread. Emitting a signal is all that is allowed to happen
        // here; the engine is the GUI thread's, and the queued delivery is what keeps it so.
        Q_EMIT linkStateChanged(static_cast<int>(state), static_cast<int>(reason));
    });
    supervisor->start();

    supervisor_ = std::move(supervisor);
    endpointName_ = QString::fromStdWString(endpoint.friendlyName);
    previousCounters_ = ipc::ValetCounters::Snapshot{};
    lastCounters_ = ipc::ValetCounters::Snapshot{};
    blocksPerSecond_ = 0.0;
    return true;
}

void EngineHost::detach() {
    if (!supervisor_) {
        return;
    }
    lastCounters_ = supervisor_->counters().snapshot();
    supervisor_->stop();
    supervisor_.reset();
    blocksPerSecond_ = 0.0;

    // The chain is left published on purpose. Nothing is calling it -- the valet thread is gone --
    // and keeping it means re-attaching to an endpoint of the same geometry costs no rebuild.
}

void EngineHost::tick() {
    const auto now = std::chrono::steady_clock::now();

    if (supervisor_) {
        const ipc::ValetCounters::Snapshot counters = supervisor_->counters().snapshot();
        const double seconds = std::chrono::duration<double>(now - previousTick_).count();
        if (seconds > 0.0 && counters.blocks >= previousCounters_.blocks) {
            blocksPerSecond_ = static_cast<double>(counters.blocks - previousCounters_.blocks) /
                               seconds;
        }
        previousCounters_ = counters;
        lastCounters_ = counters;
    }
    previousTick_ = now;

    // Both of these are no-ops when there is nothing to do, and neither may ever run on the valet
    // thread (sec. 7.4.3).
    std::string error;
    if (engine_.serviceFormatChange(error)) {
        const engine::StreamFormat built = engine_.builtFormat();
        Q_EMIT chainBuilt(built.sampleRate, built.channelCount, built.maxFrames);
    } else if (!error.empty()) {
        Q_EMIT chainFailed(QString::fromStdString(error));
    }
    engine_.serviceParameterEdits();

    Q_EMIT serviced();
}

EngineHost::Status EngineHost::status() {
    Status status;
    status.attached = supervisor_ != nullptr;
    status.linkState = supervisor_ ? supervisor_->state() : ipc::LinkState::Detached;
    status.endpointName = endpointName_;
    status.counters = lastCounters_;
    status.blocksPerSecond = blocksPerSecond_;
    status.attachCycles = supervisor_ ? supervisor_->attachCount() : 0;

    const engine::ChainProcessor& processor = engine_.chainProcessor();
    status.builtFormat = engine_.builtFormat();
    status.chainBlocks = processor.blocksProcessed();
    status.passedThrough = processor.blocksPassedThrough();
    status.formatMismatches = processor.formatMismatches();
    status.droppedEdits = engine_.droppedParameterEdits();
    status.deliveredParameters = engine_.deliveredParameters();
    status.droppedParameters = engine_.droppedParameters();
    status.strandedPlugins = engine_.strandedPlugins();

    if constexpr (rt::checksEnabled()) {
        status.violations = rt::violations();
    }
    return status;
}

} // namespace aip::ui
