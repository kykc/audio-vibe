// BufferValet -- the client half of protocol v1 (design_doc.md sec. 4.4).
//
// Split strictly by thread:
//
//   control thread   attach() / detach()      opens handles, claims the stream, prefaults pages
//   audio thread     acquire() / release()    the per-block rendezvous, and nothing else
//
// The audio-thread half is real-time safe under sec. 7.4.1: no heap, no locks, no I/O, no loader.
// The only syscalls it makes are Set/Reset/WaitForSingleObject on the KING and VALET events and
// reads and writes through the mapped view -- the exhaustive carve-out of sec. 7.4.4.

#pragma once

#include "aip/ipc/manual_event.h"
#include "aip/ipc/shared_mapping.h"
#include "aip/protocol/header_access.h"
#include "aip/protocol/layout.h"
#include "aip/protocol/planar.h"

#include <cstdint>
#include <string>

namespace aip::ipc {

/// One block as published by the king. `sampleRate` and `channelCount` are re-read from the
/// shared header for every block and deliberately not cached across blocks: the deployed APO's
/// `smartOpen` bug leaves a stale `sampleRate` after a sample-rate-only format change
/// (sec. 3.7.3, sec. 4.5), so the header is the only source of truth and it can change under us.
struct BlockInfo {
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    protocol::PlanarView audio;
};

enum class BlockStatus {
    /// A well-formed block is available in the mapped view. Process it, then `release()`.
    Captured,
    /// The king did not publish within the timeout. Not an error: it means silence, a suspended
    /// endpoint, or a king that has closed its handles and will resume later (sec. 4.5).
    Timeout,
    /// `valetId` holds *another* client's id: we have been displaced (sec. 4.4 step 2). Detach;
    /// takeover is by design and must not be fought over (sec. 4.1).
    Stolen,
    /// `valetId` is 0 -- the king evicted us for missing its 1000 ms deadline (sec. 4.4, king
    /// step 5). Sec. 4.4 step 2 lumps this in with takeover, but the two are materially
    /// different: nothing else claimed the stream, so re-claiming it resumes processing.
    /// Recoverable.
    Evicted,
    /// The header describes an impossible block. The shared objects have a null DACL (sec. 3.7.2),
    /// so this is reachable by any process on the machine. Skip processing but still
    /// `release()`, or the king stalls the audio engine for its full 1000 ms (sec. 3.7.1).
    Malformed,
    /// The wait itself failed -- the handle is gone. Detach and re-attach.
    Failed,
};

class BufferValet {
public:
    BufferValet() = default;

    BufferValet(const BufferValet&) = delete;
    BufferValet& operator=(const BufferValet&) = delete;

    // ---------------------------------------------------------------- control thread ---------

    /// Opens the KING and VALET events and the mapping for `base` (see
    /// `protocol::objectBaseName`), then performs attach steps 3-5 of sec. 4.4: generate a nonzero
    /// random id, reset VALET, publish the id. Returns false when the objects do not exist,
    /// which simply means the endpoint is not active -- retry later.
    ///
    /// Every page of the view is faulted in before returning, so the audio thread never takes a
    /// first-touch fault (sec. 7.4.2).
    [[nodiscard]] bool attach(const std::wstring& base);

    /// Clean detach from the control thread: releases our claim if it is still ours, then closes
    /// all handles. Safe to call when not attached.
    void detach() noexcept;

    [[nodiscard]] bool attached() const noexcept { return attached_; }

    /// This valet's id. Nonzero while attached (sec. 4.4 step 3).
    [[nodiscard]] std::uint32_t valetId() const noexcept { return valetId_; }

    // ------------------------------------------------------------------ audio thread ---------

    /// Waits for the king to publish a block and validates the header. `timeoutMs` may be
    /// INFINITE; the reference valet passes exactly that, but a finite timeout is conformant
    /// (sec. 4.4) and is what lets the valet thread observe a stop request.
    ///
    /// `out` is only meaningful for `Captured`; for `Malformed` the geometry fields are filled
    /// in as read so the caller can report them, but `audio` is left invalid.
    [[nodiscard]] BlockStatus acquire(DWORD timeoutMs, BlockInfo& out) noexcept;

    /// Completes the rendezvous: ResetEvent(VALET), SetEvent(KING) (sec. 4.4 step 5). Must be
    /// called exactly once for every `Captured` or `Malformed` acquire, and as promptly as
    /// possible.
    void release() noexcept;

    /// Clean detach from inside the block loop: writes 0 to `valetId` before completing the
    /// rendezvous, per the note on sec. 4.4 step 5.
    void releaseAndRelinquish() noexcept;

    /// Re-publishes our id if the slot has been zeroed. The king zeroes it when we miss its
    /// 1000 ms deadline (sec. 4.4, king step 5), which is recoverable: nothing else claimed the
    /// stream, so claiming it again resumes processing. Returns true when a claim was made.
    ///
    /// Returns false and leaves the slot alone if another valet's id is present -- that is
    /// `Stolen` and must not be fought over (sec. 4.1).
    [[nodiscard]] bool reclaimIfEvicted() noexcept;

    /// The raw `valetId` currently in the shared header.
    [[nodiscard]] std::uint32_t publishedValetId() const noexcept { return header_.valetId(); }

private:
    ManualEvent kingEvent_;
    ManualEvent valetEvent_;
    SharedMapping mapping_;
    protocol::HeaderAccess header_;
    std::uint32_t valetId_ = protocol::kNoValet;
    bool attached_ = false;
};

/// Generates a nonzero random 32-bit valet id (sec. 4.4 step 3). Control thread only -- it seeds
/// from `std::random_device`.
[[nodiscard]] std::uint32_t generateValetId();

} // namespace aip::ipc
