// The valet thread -- the client's audio thread (design_doc.md sec. 7.4, thread list at the top).
//
// Its entire job is the sec. 4.4 per-block rendezvous plus one virtual call into a BlockProcessor.
// Everything else -- attaching, detaching, retrying, building plugin chains -- belongs to the
// control thread; see ValetSupervisor. Keeping this loop tiny is a deliberate structural
// choice: `protocol/` and `ipc/` are the only places the real-time rules are hard to see at a
// glance, so they stay minimal (sec. 7.4.6, item 4).

#pragma once

#include "aip/ipc/buffer_valet.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace aip::ipc {

/// Implemented by whatever consumes audio -- the VST3 plugin chain, later. Called on the
/// promoted valet thread, once per captured block, with the payload mapped in place.
///
/// **The implementation must be real-time safe (sec. 7.4.1).** No heap, no locks, no I/O, no
/// exceptions. Chain mutation happens on the control thread and is published by a single atomic
/// pointer store; what runs here is a pointer read and dispatch, nothing more (sec. 7.4.3).
class BlockProcessor {
public:
    virtual ~BlockProcessor() = default;

    virtual void processBlock(BlockInfo& block) noexcept = 0;
};

/// A pass-through processor. Useful as a default, and as the baseline for the soak test: it
/// touches nothing, so any allocation the detector reports comes from our own plumbing.
class PassThroughProcessor final : public BlockProcessor {
public:
    void processBlock(BlockInfo&) noexcept override {}
};

/// Why the valet loop stopped.
enum class ValetExitReason {
    /// Still running, or never started.
    None,
    /// `stop()` was requested by the control thread.
    Stopped,
    /// Another client took over the stream (sec. 4.1). The supervisor must not fight for it.
    Stolen,
    /// A wait failed -- the king's objects went away. Re-attaching is the right response.
    Failed,
};

/// Counters the control plane can poll. Monotonic, relaxed; never a source of blocking.
struct ValetCounters {
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> malformedBlocks{0};
    std::atomic<std::uint64_t> reclaims{0};
    std::atomic<std::uint32_t> lastSampleRate{0};
    std::atomic<std::uint32_t> lastChannelCount{0};
    std::atomic<std::int32_t> lastFrameCount{0};
    std::atomic<std::uint32_t> formatChanges{0};

    struct Snapshot {
        std::uint64_t blocks = 0;
        std::uint64_t timeouts = 0;
        std::uint64_t malformedBlocks = 0;
        std::uint64_t reclaims = 0;
        std::uint32_t lastSampleRate = 0;
        std::uint32_t lastChannelCount = 0;
        std::int32_t lastFrameCount = 0;
        std::uint32_t formatChanges = 0;
    };

    [[nodiscard]] Snapshot snapshot() const noexcept;
};

class ValetThread {
public:
    /// `valet` must already be attached; `valet`, `processor` and `counters` must all outlive
    /// this object. Counters are owned by the caller so that they survive the re-attach cycles
    /// the supervisor performs, each of which builds a fresh ValetThread.
    ValetThread(BufferValet& valet, BlockProcessor& processor, ValetCounters& counters) noexcept;
    ~ValetThread();

    ValetThread(const ValetThread&) = delete;
    ValetThread& operator=(const ValetThread&) = delete;

    /// Per-block wait timeout. Finite (sec. 4.4 permits any timeout; only the reference passes
    /// INFINITE) so that a stop request is observed within one tick. It also bounds how long we
    /// go without noticing a king-side eviction we could recover from.
    static constexpr DWORD kBlockWaitMs = 100;

    void start();

    /// Asks the loop to finish and joins it. The loop leaves within `kBlockWaitMs` of the
    /// request. Idempotent.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] ValetExitReason exitReason() const noexcept {
        return exitReason_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const ValetCounters& counters() const noexcept { return counters_; }

    /// True once the promoted thread has taken MMCSS "Pro Audio" characteristics (sec. 4.6).
    [[nodiscard]] bool mmcssActive() const noexcept {
        return mmcssActive_.load(std::memory_order_acquire);
    }

private:
    void run() noexcept;

    BufferValet& valet_;
    BlockProcessor& processor_;
    ValetCounters& counters_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> mmcssActive_{false};
    std::atomic<ValetExitReason> exitReason_{ValetExitReason::None};
};

} // namespace aip::ipc
