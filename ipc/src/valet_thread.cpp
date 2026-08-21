#include "aip/ipc/valet_thread.h"

#include "aip/ipc/thread_priority.h"
#include "aip/rt/realtime_guard.h"

namespace aip::ipc {

ValetCounters::Snapshot ValetCounters::snapshot() const noexcept {
    Snapshot s;
    s.blocks = blocks.load(std::memory_order_relaxed);
    s.timeouts = timeouts.load(std::memory_order_relaxed);
    s.malformedBlocks = malformedBlocks.load(std::memory_order_relaxed);
    s.reclaims = reclaims.load(std::memory_order_relaxed);
    s.lastSampleRate = lastSampleRate.load(std::memory_order_relaxed);
    s.lastChannelCount = lastChannelCount.load(std::memory_order_relaxed);
    s.lastFrameCount = lastFrameCount.load(std::memory_order_relaxed);
    s.formatChanges = formatChanges.load(std::memory_order_relaxed);
    return s;
}

ValetThread::ValetThread(BufferValet& valet, BlockProcessor& processor,
                         ValetCounters& counters) noexcept
    : valet_(valet), processor_(processor), counters_(counters) {}

ValetThread::~ValetThread() { stop(); }

void ValetThread::start() {
    if (thread_.joinable()) {
        return;
    }
    stopRequested_.store(false, std::memory_order_relaxed);
    exitReason_.store(ValetExitReason::None, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void ValetThread::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ValetThread::run() noexcept {
    // Promotion happens once, on the thread itself, and is reverted when it leaves (sec. 4.6).
    ProAudioPriority priority;
    mmcssActive_.store(priority.mmcssActive(), std::memory_order_release);

    ValetExitReason reason = ValetExitReason::Stopped;

    // Preallocated: the loop body must not construct anything (sec. 7.4.2).
    BlockInfo block;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        // Everything below this point is a real-time section. The detector (sec. 7.4.6) will count
        // any allocation or lock reached from here, including from inside processBlock.
        const rt::RealtimeGuard rtSection;

        const BlockStatus status = valet_.acquire(kBlockWaitMs, block);

        if (status == BlockStatus::Captured) {
            if (block.sampleRate != counters_.lastSampleRate.load(std::memory_order_relaxed) ||
                block.channelCount !=
                    counters_.lastChannelCount.load(std::memory_order_relaxed) ||
                block.audio.frameCount() !=
                    counters_.lastFrameCount.load(std::memory_order_relaxed)) {
                counters_.lastSampleRate.store(block.sampleRate, std::memory_order_relaxed);
                counters_.lastChannelCount.store(block.channelCount, std::memory_order_relaxed);
                counters_.lastFrameCount.store(block.audio.frameCount(),
                                               std::memory_order_relaxed);
                counters_.formatChanges.fetch_add(1, std::memory_order_relaxed);
            }

            processor_.processBlock(block);

            counters_.blocks.fetch_add(1, std::memory_order_relaxed);
            valet_.release();
            continue;
        }

        if (status == BlockStatus::Malformed) {
            // Do not process, but do complete the rendezvous: leaving the king to time out
            // costs the audio engine's real-time thread up to 1000 ms (sec. 3.7.1).
            counters_.malformedBlocks.fetch_add(1, std::memory_order_relaxed);
            valet_.release();
            continue;
        }

        if (status == BlockStatus::Evicted) {
            // The king gave up on this block and passed the audio through unprocessed, so there
            // is nothing worth processing here. Re-claim the slot and complete the rendezvous so
            // the event pair is left in the state the next block expects.
            if (valet_.reclaimIfEvicted()) {
                counters_.reclaims.fetch_add(1, std::memory_order_relaxed);
            }
            valet_.release();
            continue;
        }

        if (status == BlockStatus::Stolen) {
            reason = ValetExitReason::Stolen;
            break;
        }

        if (status == BlockStatus::Failed) {
            reason = ValetExitReason::Failed;
            break;
        }

        // Timeout. The endpoint may simply be idle, or the king may have evicted us for missing
        // its deadline -- in which case re-claiming resumes the stream (sec. 4.4, king step 5).
        counters_.timeouts.fetch_add(1, std::memory_order_relaxed);

        const std::uint32_t published = valet_.publishedValetId();
        if (published == valet_.valetId()) {
            continue;
        }
        if (published == protocol::kNoValet) {
            if (valet_.reclaimIfEvicted()) {
                counters_.reclaims.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        // Someone else's id is in the slot: we were displaced without ever seeing a block.
        reason = ValetExitReason::Stolen;
        break;
    }

    exitReason_.store(reason, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

} // namespace aip::ipc
