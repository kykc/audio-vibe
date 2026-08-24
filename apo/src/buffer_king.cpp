#include "aip/apo/buffer_king.h"

#include "aip/protocol/planar.h"

#include <cstring>

namespace aip::apo {

bool BufferKing::open(const std::wstring& objectBase, std::uint32_t sampleRate, std::uint32_t channelCount) {
    close();

    if (!ipc::SharedMapping::openOrCreate(objectBase, protocol::kMappingSize, mapping_)) {
        return false;
    }
    if (!ipc::ManualEvent::create(protocol::kingEventName(objectBase), /*initiallySignaled=*/true, kingEvent_)) {
        close();
        return false;
    }
    if (!ipc::ManualEvent::create(protocol::valetEventName(objectBase),
            /*initiallySignaled=*/false, valetEvent_)) {
        close();
        return false;
    }

    // Every page of the view, touched here on the control thread. The audio thread must never
    // take a first-touch fault (sec. 7.4.1), and a fault inside `audiodg.exe` is paid for by
    // every application on the machine, not just by us.
    mapping_.prefault();

    header_ = protocol::HeaderAccess(mapping_.data());

    // Sec. 4.5: KING signaled, VALET cleared. A valet that was mid-rendezvous when the format
    // changed is released rather than left blocked on an event nobody will ever set.
    kingEvent_.set();
    valetEvent_.reset();

    // Only on a section we created. Reopening an existing one -- which is what a format change
    // does -- must leave an attached valet's claim alone.
    if (mapping_.createdNew()) {
        header_.setValetId(protocol::kNoValet);
    }

    base_ = objectBase;
    sampleRate_ = sampleRate;
    channelCount_ = channelCount;
    header_.setSampleRate(sampleRate);
    header_.setChannelCount(channelCount);
    opened_ = true;
    blocks_ = 0;
    evictions_ = 0;
    return true;
}

bool BufferKing::smartOpen(const std::wstring& objectBase, std::uint32_t sampleRate, std::uint32_t channelCount) {
    if (!opened_) {
        return open(objectBase, sampleRate, channelCount);
    }
    // `||`, not the deployed APO's `&&` (sec. 3.7.3). A 44.1 -> 48 kHz change at a constant
    // channel count is the case the bug misses, and it is the common one: it is what happens
    // when the user changes the endpoint's shared-mode format.
    if (sampleRate != sampleRate_ || channelCount != channelCount_ || objectBase != base_) {
        return open(objectBase, sampleRate, channelCount);
    }
    return true;
}

void BufferKing::close() noexcept {
    if (kingEvent_.valid()) {
        kingEvent_.reset();
    }
    if (valetEvent_.valid()) {
        valetEvent_.reset();
    }
    kingEvent_.close();
    valetEvent_.close();
    mapping_.close();
    header_ = protocol::HeaderAccess();
    base_.clear();
    sampleRate_ = 0;
    channelCount_ = 0;
    opened_ = false;
}

DispatchResult BufferKing::dispatch(const float* interleavedIn, float* interleavedOut, std::int32_t size) noexcept {
    if (!opened_ || !header_.valid()) {
        return DispatchResult::Unusable;
    }

    // The geometry the audio engine handed us, checked against what protocol v1 can carry
    // (sec. 4.3). `channelCount_` comes from `LockForProcess` and `size` from the block, so a
    // failure here means the two disagree -- which the engine should never do, and which would
    // otherwise index the payload out of the mapping.
    if (protocol::validateBlock(channelCount_, size) != protocol::HeaderStatus::Ok) {
        return DispatchResult::Unusable;
    }

    ++blocks_;

    // Step 1-2. No valet: the caller passes through. Nothing is published, so an endpoint with
    // no client costs a single relaxed load per block.
    if (header_.valetId() == protocol::kNoValet) {
        return DispatchResult::NoValet;
    }

    // Step 3. Format first, then the payload. Both are re-read by the valet on every block
    // (sec. 4.5), so a format change needs no announcement beyond this.
    header_.setSampleRate(sampleRate_);
    header_.setChannelCount(channelCount_);
    header_.setSize(size);
    protocol::deinterleave(interleavedIn, header_.payload(), size, channelCount_);

    // Step 4. Order matters: clearing KING before setting VALET is what stops the valet from
    // observing a KING left signaled by the previous block and returning immediately.
    kingEvent_.reset();
    valetEvent_.set();

    // Step 5. The one unbounded-ish operation on this thread, and the whole of v1's risk.
    if (kingEvent_.wait(kValetTimeoutMs) != ipc::WaitResult::Signaled) {
        // Evict. The valet finds its id gone and re-claims (the client tells that case apart
        // from a takeover -- status.md sec. 7 item 2), and meanwhile audio keeps flowing.
        header_.setValetId(protocol::kNoValet);
        ++evictions_;
        return DispatchResult::ValetTimedOut;
    }

    protocol::reinterleave(header_.payload(), interleavedOut, size, channelCount_);
    return DispatchResult::Processed;
}

} // namespace aip::apo
