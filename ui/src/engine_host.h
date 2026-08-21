// The shell's control thread, which is the GUI thread (design_doc.md sec. 7.4.3).
//
// `engine::Engine` is documented as not thread-safe against itself and as expecting one control
// thread; `ipc::ValetSupervisor` makes the same assumption. This class is where that single
// thread is *named*: it is the Qt GUI thread, and every call into the engine below happens on it,
// either from a widget's slot or from the servicing timer. Nothing here may be called from
// anywhere else.
//
// The one thing that genuinely arrives from another thread is the supervisor's state callback,
// which runs on the supervisor's own thread. It is turned into a Qt signal and nothing else -- a
// cross-thread signal to a receiver in the GUI thread is delivered through the event loop, so the
// engine is still only ever touched from one place.
//
// Why the servicing timer exists at all: protocol v1 announces the endpoint's format nowhere
// (sec. 4.5), so the only way to learn it is to look at what the audio thread has seen. The
// timer is that look. It also drains the plugin callbacks, which is what carries a knob turned in
// a plugin's editor across to its processor.

#pragma once

#include "aip/engine/engine.h"
#include "aip/ipc/endpoints.h"
#include "aip/ipc/valet_supervisor.h"
#include "aip/rt/realtime_guard.h"

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <memory>

namespace aip::ui {

class EngineHost final : public QObject {
    Q_OBJECT

public:
    /// Everything the shell displays, gathered in one pass so the numbers on screen belong to the
    /// same instant.
    struct Status {
        bool attached = false;
        ipc::LinkState linkState = ipc::LinkState::Detached;
        QString endpointName;
        ipc::ValetCounters::Snapshot counters;
        /// Blocks in the last tick interval, scaled to a second.
        double blocksPerSecond = 0.0;
        std::uint32_t attachCycles = 0;

        engine::StreamFormat builtFormat;
        std::uint64_t chainBlocks = 0;
        std::uint64_t passedThrough = 0;
        std::uint64_t formatMismatches = 0;
        std::uint64_t droppedEdits = 0;
        std::uint64_t deliveredParameters = 0;
        std::uint64_t droppedParameters = 0;
        std::size_t strandedPlugins = 0;

        rt::ViolationCounts violations;
    };

    explicit EngineHost(QObject* parent = nullptr);
    ~EngineHost() override;

    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    /// GUI thread only, like everything else here.
    [[nodiscard]] engine::Engine& engine() noexcept { return engine_; }

    /// Attaches to `endpoint` as a protocol v1 valet. Detaches from whatever was attached first.
    /// The rack is untouched: the plugins stay loaded and keep their parameters, and the chain is
    /// rebuilt from the first block the new endpoint produces.
    bool attach(const ipc::RenderEndpoint& endpoint, QString& error);

    /// Stops the valet thread and gives the stream back. The rack survives.
    void detach();

    [[nodiscard]] bool attached() const noexcept { return supervisor_ != nullptr; }

    /// Not const: reading the chain processor's counters goes through a non-const accessor, and
    /// pretending otherwise would only mean a const_cast here instead.
    [[nodiscard]] Status status();

Q_SIGNALS:
    /// From the supervisor's thread, delivered on the GUI thread. `state` is an
    /// `ipc::LinkState` and `reason` an `ipc::ValetExitReason`, passed as int because that is
    /// what crosses a queued connection without registering metatypes.
    void linkStateChanged(int state, int reason);

    /// A chain was built or rebuilt for a geometry the audio thread reported.
    void chainBuilt(unsigned sampleRate, unsigned channelCount, int maxFrames);

    /// A rebuild was attempted and failed. Audio keeps flowing, unprocessed.
    void chainFailed(const QString& error);

    /// One servicing tick completed; the status is worth re-reading.
    void serviced();

private:
    void tick();

    engine::Engine engine_;
    /// Rebuilt per attach: the supervisor takes its endpoint at construction, and switching
    /// endpoints is therefore a new supervisor rather than a setter.
    std::unique_ptr<ipc::ValetSupervisor> supervisor_;
    QString endpointName_;

    /// Kept so the counters do not blank out the moment the user detaches -- what the run did is
    /// the interesting part, and it is gone with the supervisor.
    ipc::ValetCounters::Snapshot lastCounters_{};
    ipc::ValetCounters::Snapshot previousCounters_{};
    std::chrono::steady_clock::time_point previousTick_{};
    double blocksPerSecond_ = 0.0;
};

} // namespace aip::ui
