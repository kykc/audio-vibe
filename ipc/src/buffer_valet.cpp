#include "aip/ipc/buffer_valet.h"

#include <limits>
#include <random>

namespace aip::ipc {

std::uint32_t generateValetId() {
    std::random_device device;
    std::mt19937 engine(device());
    constexpr std::uint32_t kMaxId = std::numeric_limits<std::uint32_t>::max();
    std::uniform_int_distribution<std::uint32_t> distribution(1u, kMaxId);
    std::uint32_t id = protocol::kNoValet;
    while (id == protocol::kNoValet) {
        id = distribution(engine);
    }
    return id;
}

bool BufferValet::attach(const std::wstring& base) {
    detach();

    // Sec. 4.4 step 1. Absence of either event means the endpoint is not active; that is an
    // ordinary outcome, not an error.
    if (!ManualEvent::open(protocol::kingEventName(base), kingEvent_)) {
        return false;
    }
    if (!ManualEvent::open(protocol::valetEventName(base), valetEvent_)) {
        kingEvent_.close();
        return false;
    }

    // Sec. 4.4 step 2. Same name and size as the king used, so this attaches to its section.
    if (!SharedMapping::openOrCreate(base, protocol::kMappingSize, mapping_)) {
        kingEvent_.close();
        valetEvent_.close();
        return false;
    }

    // Commit every page while we are still on the control thread (sec. 7.4.2).
    mapping_.prefault();

    header_ = protocol::HeaderAccess(mapping_.data());
    valetId_ = generateValetId(); // sec. 4.4 step 3
    valetEvent_.reset(); // sec. 4.4 step 4
    header_.setValetId(valetId_); // sec. 4.4 step 5 -- attached from here on
    attached_ = true;
    return true;
}

void BufferValet::detach() noexcept {
    if (attached_) {
        // Only relinquish a claim that is still ours: if we were stolen from, the id belongs to
        // the incumbent and zeroing it would evict an innocent third party (sec. 4.1).
        if (header_.valid() && header_.valetId() == valetId_) {
            header_.setValetId(protocol::kNoValet);
        }
    }
    attached_ = false;
    valetId_ = protocol::kNoValet;
    header_ = protocol::HeaderAccess();
    mapping_.close();
    valetEvent_.close();
    kingEvent_.close();
}

BlockStatus BufferValet::acquire(DWORD timeoutMs, BlockInfo& out) noexcept {
    switch (valetEvent_.wait(timeoutMs)) { // sec. 4.4 step 1
    case WaitResult::Signaled:
        break;
    case WaitResult::Timeout:
        return BlockStatus::Timeout;
    case WaitResult::Failed:
    default:
        return BlockStatus::Failed;
    }

    // Sec. 4.4 step 2. A zero here is an eviction we can recover from, not a takeover; see the
    // BlockStatus comments.
    if (const std::uint32_t published = header_.valetId(); published != valetId_) {
        return published == protocol::kNoValet ? BlockStatus::Evicted : BlockStatus::Stolen;
    }

    // Sec. 4.4 step 3 -- re-read the geometry every block; never cache it (sec. 4.5).
    out.sampleRate = header_.sampleRate();
    out.channelCount = header_.channelCount();
    const std::int32_t size = header_.size();

    if (protocol::validateBlock(out.channelCount, size) != protocol::HeaderStatus::Ok) {
        out.audio = protocol::PlanarView();
        return BlockStatus::Malformed;
    }

    out.audio = protocol::PlanarView(header_.payload(), out.channelCount, size);
    return BlockStatus::Captured;
}

void BufferValet::release() noexcept {
    valetEvent_.reset(); // sec. 4.4 step 5
    kingEvent_.set();
}

void BufferValet::releaseAndRelinquish() noexcept {
    header_.setValetId(protocol::kNoValet);
    release();
}

bool BufferValet::reclaimIfEvicted() noexcept {
    if (header_.valetId() != protocol::kNoValet) {
        return false;
    }
    header_.setValetId(valetId_);
    return true;
}

} // namespace aip::ipc
