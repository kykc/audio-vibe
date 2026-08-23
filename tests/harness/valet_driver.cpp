#include "harness/valet_driver.h"

#include "aip/ipc/thread_priority.h"

#include <windows.h>

namespace aip::harness {

bool ValetDriver::start(const std::wstring& objectBase) {
    if (running_.load(std::memory_order_relaxed)) {
        return true;
    }
    if (!valet_.attach(objectBase)) {
        return false;
    }
    stopRequested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
    return true;
}

void ValetDriver::stop() noexcept {
    stopRequested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_relaxed);
    valet_.detach();
}

void ValetDriver::run() {
    // Promoted like the real thing (sec. 4.6). A king under test is measuring how long the
    // rendezvous takes; running the other end at ordinary priority would make eviction depend on
    // what else the machine happens to be doing.
    const ipc::ProAudioPriority promotion;
    (void)promotion;

    // 100 ms rather than INFINITE, for the reason the client uses the same figure: it is what
    // lets the loop notice `stopRequested_` (status.md sec. 7 item 3).
    constexpr DWORD kWaitMs = 100;

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        ipc::BlockInfo block;
        const ipc::BlockStatus status = valet_.acquire(kWaitMs, block);

        switch (status) {
        case ipc::BlockStatus::Captured: {
            lastSampleRate_.store(block.sampleRate, std::memory_order_relaxed);
            lastChannelCount_.store(block.channelCount, std::memory_order_relaxed);
            lastFrameCount_.store(block.audio.frameCount(), std::memory_order_relaxed);

            if (transform_) {
                transform_(block.audio);
            }

            // Scripted lateness. Counted before the sleep so that a test can see the block was
            // reached even when the king gives up on it.
            captured_.fetch_add(1, std::memory_order_relaxed);
            if (stallBlocks_.load(std::memory_order_relaxed) > 0) {
                stallBlocks_.fetch_sub(1, std::memory_order_relaxed);
                ::Sleep(stallMs_.load(std::memory_order_relaxed));
            }

            valet_.release();
            break;
        }
        case ipc::BlockStatus::Malformed:
            // Still release: not doing so stalls the king for its full timeout, which is the
            // exact denial of service sec. 3.7.1 warns about.
            valet_.release();
            break;
        case ipc::BlockStatus::Evicted:
            if (valet_.reclaimIfEvicted()) {
                reclaims_.fetch_add(1, std::memory_order_relaxed);
            }
            valet_.release();
            break;
        case ipc::BlockStatus::Stolen:
            stolen_.fetch_add(1, std::memory_order_relaxed);
            // Takeover is by design (sec. 4.1) and must not be fought over. Release so the king
            // is not left waiting, then stop -- which is what the real supervisor does.
            valet_.release();
            stopRequested_.store(true, std::memory_order_relaxed);
            break;
        case ipc::BlockStatus::Timeout: {
            timeouts_.fetch_add(1, std::memory_order_relaxed);

            // **This, not `Evicted`, is how a real eviction is recovered from**, and getting it
            // wrong the first time is what this comment is for.
            //
            // `BlockStatus::Evicted` can only be returned from `acquire`, which only returns at
            // all when the king has published a block. But a king that has just evicted its valet
            // sees `valetId == 0` on every subsequent block and *stops publishing* -- so it never
            // signals VALET again, `acquire` never returns anything but `Timeout`, and a valet
            // that waited for `Evicted` would wait forever. The stream would be dead with both
            // sides behaving exactly as written.
            //
            // So the slot is inspected on the idle path, which is the one that keeps running.
            // `ValetThread` does the same thing for the same reason (`valet_thread.cpp`); this
            // driver had to learn it the hard way, from a king test that evicted correctly and
            // then hung.
            const std::uint32_t published = valet_.publishedValetId();
            if (published == protocol::kNoValet && valet_.reclaimIfEvicted()) {
                reclaims_.fetch_add(1, std::memory_order_relaxed);
            } else if (published != protocol::kNoValet && published != valet_.valetId()) {
                stolen_.fetch_add(1, std::memory_order_relaxed);
                stopRequested_.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case ipc::BlockStatus::Failed:
            stopRequested_.store(true, std::memory_order_relaxed);
            break;
        }
    }
}

} // namespace aip::harness
