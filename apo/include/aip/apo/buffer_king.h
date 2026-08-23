// Protocol v1, king side (design_doc.md sec. 4.4 / sec. 4.5) -- the producer half that lives
// inside `audiodg.exe`.
//
// This is the counterpart of `ipc/buffer_valet.h` and the shipping equivalent of the conformance
// harness's `SyntheticKing`. All three read the wire format out of `protocol/`, which is what
// keeps them from drifting.
//
// Two things separate this from the harness's synthetic king, and both are deliberate:
//
//  1. **The `smartOpen` bug is fixed here.** The deployed APO tests `sampleRate != _sampleRate &&
//     channelCount != _channelCount` where `||` was meant (sec. 3.7.3), so a sample-rate-only
//     format change leaves a stale `sampleRate` in the shared header. `SyntheticKing` reproduces
//     that on purpose, because the client must tolerate the deployed binary. A *new* king has no
//     reason to reproduce it: the fix is invisible to a conforming valet, which re-reads the
//     header every block anyway (sec. 4.5), and visible only to a broken one.
//
//  2. **`dispatch` is real-time safe without qualification** (sec. 7.4). It runs on the audio
//     engine's own thread, inside a process that is not ours, with every other application's
//     audio behind it. No allocation, no lock, no I/O, no fallible call whose failure path
//     allocates. The only blocking operation is the sanctioned KING/VALET rendezvous of
//     sec. 7.4.4 -- which is also v1's known weak point, since it can hold that thread for a
//     full second (sec. 3.7.1, and the reason sec. 9.1 exists).
//
// Everything that can allocate happens in `open`/`close`, which the audio engine calls from
// `LockForProcess`/`UnlockForProcess` on a control thread.

#pragma once

#include "aip/ipc/manual_event.h"
#include "aip/ipc/shared_mapping.h"
#include "aip/protocol/header_access.h"
#include "aip/protocol/layout.h"

#include <cstdint>
#include <string>

namespace aip::apo {

/// What one block's rendezvous did. Mirrors the three branches of sec. 4.4 king steps 2-5.
enum class DispatchResult {
    /// `valetId` was 0 -- no client attached. The caller must pass the input through unchanged.
    NoValet,
    /// The valet completed the rendezvous and its output is in the shared payload.
    Processed,
    /// The valet missed the deadline. It has been evicted (`valetId` set to 0) and the caller
    /// must pass the input through unchanged.
    ValetTimedOut,
    /// The stream is not open, or the block geometry is one protocol v1 cannot carry. Pass
    /// through. Not a state the audio engine can provoke on its own -- see `dispatch`.
    Unusable,
};

class BufferKing {
public:
    /// Sec. 4.4 king step 5. One second, matching the deployed APO exactly.
    ///
    /// This is the single worst number in protocol v1: it is how long a stalled userspace client
    /// can hold the audio engine's real-time thread, and therefore how long system-wide audio can
    /// stop (sec. 3.7.1). It is reproduced rather than improved because v1 is frozen and the
    /// client half of the timeout policy is deployed; cutting it is sec. 9.1's job, and needs a
    /// negotiated v2 so that a v1 valet is not evicted by a v2 king it cannot keep up with.
    static constexpr DWORD kValetTimeoutMs = 1000;

    BufferKing() = default;
    ~BufferKing() { close(); }

    BufferKing(const BufferKing&) = delete;
    BufferKing& operator=(const BufferKing&) = delete;

    /// Creates the mapping and both events for `objectBase` and publishes the format
    /// (sec. 4.2, sec. 4.5): KING signaled, VALET cleared, and `valetId` zeroed **only** if this
    /// call created the section fresh -- an existing section may already have a valet in it, and
    /// zeroing would evict a client that has done nothing wrong.
    ///
    /// Control thread only. Returns false if any object could not be created, which in
    /// `audiodg.exe` means the APO runs as a pass-through rather than failing the stream.
    [[nodiscard]] bool open(const std::wstring& objectBase, std::uint32_t sampleRate,
                            std::uint32_t channelCount);

    /// Opens on first call; on later calls reopens **if either** the sample rate or the channel
    /// count changed. The `||` is the point -- see the header comment.
    [[nodiscard]] bool smartOpen(const std::wstring& objectBase, std::uint32_t sampleRate,
                                 std::uint32_t channelCount);

    /// Clears both events and releases every handle (sec. 4.5). A valet blocked in its wait then
    /// sees the handle go away and re-attaches when the stream comes back.
    void close() noexcept;

    [[nodiscard]] bool opened() const noexcept { return opened_; }

    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] std::uint32_t channelCount() const noexcept { return channelCount_; }

    /// One block, sec. 4.4 king steps 1-5. `size` is the **total** sample count,
    /// `frames * channelCount` -- not a frame count (sec. 4.3).
    ///
    /// `interleavedIn` and `interleavedOut` may be the same pointer: the APO declares
    /// `APO_FLAG_INPLACE` and the audio engine does hand it the same buffer. On every result
    /// other than `Processed` the caller is responsible for the pass-through copy, which is left
    /// to it precisely because in the in-place case there is nothing to copy.
    ///
    /// **Audio thread. Real-time safe.** No allocation, no lock, no I/O, no failure path that
    /// takes any. The one blocking call is the sec. 7.4.4 rendezvous.
    [[nodiscard]] DispatchResult dispatch(const float* interleavedIn, float* interleavedOut,
                                          std::int32_t size) noexcept;

    /// Blocks dispatched since `open`, and evictions among them. Plain counters on the audio
    /// thread, read from anywhere; they exist so that a live APO can be asked whether it is doing
    /// anything at all, which is otherwise unanswerable from inside `audiodg.exe`.
    [[nodiscard]] std::uint64_t blockCount() const noexcept { return blocks_; }

    [[nodiscard]] std::uint64_t evictionCount() const noexcept { return evictions_; }

private:
    ipc::SharedMapping mapping_;
    ipc::ManualEvent kingEvent_;
    ipc::ManualEvent valetEvent_;
    protocol::HeaderAccess header_;

    std::wstring base_;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t channelCount_ = 0;
    bool opened_ = false;

    std::uint64_t blocks_ = 0;
    std::uint64_t evictions_ = 0;
};

} // namespace aip::apo
