// ValetSupervisor -- the control-thread half of the client's IPC link (design_doc.md sec. 7.4.3).
//
// Attaching, retrying, re-attaching and tearing down are all control-plane work, so they live
// here and never on the valet thread. The supervisor owns the BufferValet, builds a ValetThread
// once the valet is attached, and rebuilds it after a recoverable failure.
//
// Policy on takeover: sec. 4.1 makes displacement intentional -- a newly arriving valet *displaces*
// the incumbent. A displaced supervisor therefore does not re-attach by default; doing so would
// make two clients ping-pong the stream indefinitely. Set `reattachAfterSteal` only if you
// genuinely want to fight for it.

#pragma once

#include "aip/ipc/buffer_valet.h"
#include "aip/ipc/manual_event.h"
#include "aip/ipc/valet_thread.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace aip::ipc {

enum class LinkState {
    /// Not attached. Either not started, or waiting for the endpoint to become active.
    Detached,
    /// Attached, valet thread running.
    Attached,
    /// Stopped for good: displaced by another client and `reattachAfterSteal` is false.
    Relinquished,
};

struct SupervisorPolicy {
    /// How long to wait before retrying after a failed attach or a recoverable exit.
    unsigned retryDelayMs = 500;
    /// See the note above -- off by default, deliberately.
    bool reattachAfterSteal = false;
};

/// Tag wrapper for a fully-built object base name, as distinct from an endpoint GUID. Used by
/// the conformance harness, which drives synthetic endpoints that have no GUID at all.
struct ObjectBaseName {
    std::wstring value;
};

class ValetSupervisor {
public:
    /// `processor` must outlive the supervisor. `endpointGuid` is `PKEY_AudioEndpoint_GUID`
    /// verbatim, braces included (sec. 4.2) -- see `ipc/endpoints.h`.
    ValetSupervisor(std::wstring endpointGuid, BlockProcessor& processor,
                    SupervisorPolicy policy = {});

    /// Attaches to an explicit object base name rather than deriving one from an endpoint GUID.
    ValetSupervisor(ObjectBaseName base, BlockProcessor& processor,
                    SupervisorPolicy policy = {});
    ~ValetSupervisor();

    ValetSupervisor(const ValetSupervisor&) = delete;
    ValetSupervisor& operator=(const ValetSupervisor&) = delete;

    /// Invoked on the supervisor thread on every state transition. Control-plane only, so it may
    /// allocate, log and take locks freely. Set before `start()`.
    using StateCallback = std::function<void(LinkState, ValetExitReason)>;

    void setStateCallback(StateCallback callback) { stateCallback_ = std::move(callback); }

    void start();
    void stop();

    [[nodiscard]] LinkState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const ValetCounters& counters() const noexcept { return counters_; }

    [[nodiscard]] const std::wstring& objectBaseName() const noexcept { return base_; }

    /// Number of completed attach cycles. Useful in tests to observe re-attach behaviour.
    [[nodiscard]] std::uint32_t attachCount() const noexcept {
        return attachCount_.load(std::memory_order_relaxed);
    }

private:
    void run();
    void publish(LinkState state, ValetExitReason reason);
    /// Interruptible sleep -- returns true if a stop was requested while waiting.
    bool sleepOrStop(unsigned ms);

    std::wstring base_;
    BlockProcessor& processor_;
    SupervisorPolicy policy_;
    StateCallback stateCallback_;

    BufferValet valet_;
    ValetCounters counters_;
    ManualEvent stopEvent_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<LinkState> state_{LinkState::Detached};
    std::atomic<std::uint32_t> attachCount_{0};
};

} // namespace aip::ipc
