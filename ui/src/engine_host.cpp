#include "engine_host.h"

#include <QTimer>

#include <chrono>
#include <string>

namespace aip::ui {

namespace {

/// Ten times a second. The chain can only be built once a block has revealed the format
/// (sec. 4.5), so this interval is how much audio goes through unprocessed on every attach -- one
/// or two blocks. It is also how long a knob in a plugin's editor can lag the audio, which is the
/// same reason to keep it short.
constexpr int kTickMs = 100;

/// How long the block counter has to stand still before the link is called idle. The valet times
/// out every 100 ms, and a playing endpoint delivers a block far more often than that -- roughly
/// one every 10 ms at the geometry Windows actually uses -- so half a second is dozens of missed
/// blocks and cannot be reached by ordinary jitter. Long enough not to flicker at the end of a
/// track, short enough that the label is right by the time a user looks at it.
constexpr auto kIdleAfter = std::chrono::milliseconds(500);

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

    // The endpoint's speaker layout, before anything is prepared for it. This is the only place
    // in the program that knows it -- protocol v1 carries no channel-order information and its
    // header is frozen (sec. 4.3) -- and it is knowable here only because the object names are
    // derived from the endpoint GUID, so an endpoint we can attach to is one we can also ask.
    // Zero when the device reports a plain WAVEFORMATEX, which the engine treats as "guess".
    engine_.setChannelMask(endpoint.channelMask);

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

    // Prepare for the endpoint's configured format now, rather than waiting for a block to reveal
    // one. Nothing is playing on a freshly attached endpoint more often than not, and without
    // this the whole rack sits unprepared -- no warm-up, and no way to learn that a plugin
    // refuses the format -- until the user happens to play something.
    //
    // Failure is reported and otherwise ignored: the guess is an optimisation, and the first real
    // block will build the chain properly whatever happens here.
    std::string guessError;
    if (engine_.prepareSpeculatively(endpoint.deviceSampleRate, endpoint.deviceChannelCount,
                                     guessError)) {
        const engine::StreamFormat built = engine_.builtFormat();
        if (built.valid()) {
            Q_EMIT chainBuilt(built.sampleRate, built.channelCount, built.maxFrames,
                              engine_.builtFormatIsSpeculative());
        }
    } else {
        Q_EMIT chainFailed(QString::fromStdString(guessError));
    }

    previousCounters_ = ipc::ValetCounters::Snapshot{};
    lastCounters_ = ipc::ValetCounters::Snapshot{};
    lastBlockAt_ = std::chrono::steady_clock::now();
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
        // The block *counter* moving, not the rate: a rate computed over one tick can round to
        // zero for a slow endpoint, and the question here is whether anything arrived at all.
        if (counters.blocks != previousCounters_.blocks) {
            lastBlockAt_ = now;
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
        Q_EMIT chainBuilt(built.sampleRate, built.channelCount, built.maxFrames,
                          engine_.builtFormatIsSpeculative());
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
    // Only while attached, and only in the state where blocks are the expectation. A relinquished
    // link is not idle, it is finished, and saying "idle" of it would be the same kind of
    // misreading this flag exists to prevent.
    status.idle = status.attached && status.linkState == ipc::LinkState::Attached &&
                  (std::chrono::steady_clock::now() - lastBlockAt_) >= kIdleAfter;

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
