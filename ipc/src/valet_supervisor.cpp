#include "aip/ipc/valet_supervisor.h"

#include "aip/protocol/layout.h"

namespace aip::ipc {

ValetSupervisor::ValetSupervisor(std::wstring endpointGuid, BlockProcessor& processor, SupervisorPolicy policy)
    : ValetSupervisor(ObjectBaseName{protocol::objectBaseName(endpointGuid)}, processor, policy) {}

ValetSupervisor::ValetSupervisor(ObjectBaseName base, BlockProcessor& processor, SupervisorPolicy policy)
    : base_(std::move(base.value)), processor_(processor), policy_(policy) {}

ValetSupervisor::~ValetSupervisor() { stop(); }

void ValetSupervisor::start() {
    if (thread_.joinable()) {
        return;
    }
    if (!stopEvent_.valid() && !ManualEvent::createLocal(false, stopEvent_)) {
        return;
    }
    stopEvent_.reset();
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void ValetSupervisor::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (stopEvent_.valid()) {
        stopEvent_.set();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool ValetSupervisor::sleepOrStop(unsigned ms) {
    if (stopRequested_.load(std::memory_order_acquire)) {
        return true;
    }
    return stopEvent_.wait(ms) == WaitResult::Signaled;
}

void ValetSupervisor::publish(LinkState state, ValetExitReason reason) {
    state_.store(state, std::memory_order_release);
    if (stateCallback_) {
        stateCallback_(state, reason);
    }
}

void ValetSupervisor::run() {
    while (!stopRequested_.load(std::memory_order_acquire)) {
        // Attach is control-thread work: it opens handles, claims the stream and faults in every
        // page of the view before any audio thread exists (sec. 7.4.2).
        if (!valet_.attach(base_)) {
            // The endpoint is not active: the KING/VALET events do not exist yet
            // (sec. 4.4 step 1).
            if (sleepOrStop(policy_.retryDelayMs)) {
                break;
            }
            continue;
        }

        attachCount_.fetch_add(1, std::memory_order_relaxed);
        publish(LinkState::Attached, ValetExitReason::None);

        ValetThread valetThread(valet_, processor_, counters_);
        valetThread.start();

        while (valetThread.running() && !stopRequested_.load(std::memory_order_acquire)) {
            // Sleep on the stop event so a stop request is picked up immediately; the result is
            // irrelevant, since both outcomes lead back to the loop condition above.
            (void)stopEvent_.wait(ValetThread::kBlockWaitMs);
        }

        valetThread.stop();
        const ValetExitReason reason = valetThread.exitReason();
        valet_.detach();

        if (stopRequested_.load(std::memory_order_acquire)) {
            publish(LinkState::Detached, reason);
            break;
        }

        if (reason == ValetExitReason::Stolen && !policy_.reattachAfterSteal) {
            // Displacement is by design (sec. 4.1). Stop rather than fight over the endpoint.
            publish(LinkState::Relinquished, reason);
            return;
        }

        publish(LinkState::Detached, reason);

        if (sleepOrStop(policy_.retryDelayMs)) {
            break;
        }
    }

    state_.store(LinkState::Detached, std::memory_order_release);
}

} // namespace aip::ipc
