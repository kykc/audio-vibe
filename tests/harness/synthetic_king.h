// Synthetic king -- the producer half of the protocol conformance harness (design_doc.md sec. 4.7).
//
// This is a faithful reimplementation of the deployed APO's king side (`BufferKing` in
// `TomatlAudioIpc/FastStream.h` at commit 1a1d3ea), against the normative text of sec. 4.4/sec. 4.5
// rather than against the reference source. It is a first-class deliverable: it replaces the
// predecessor's manual interactive `DebugStream` and is what lets the client be developed and
// regression-tested without `audiodg.exe` in the loop.
//
// It reproduces the known defects of v1 on purpose -- see `smartOpen` below. A harness that
// silently fixed them would hide exactly the behaviour the client has to tolerate (sec. 4.5).

#pragma once

#include "aip/ipc/manual_event.h"
#include "aip/ipc/shared_mapping.h"
#include "aip/protocol/header_access.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aip::harness {

/// Outcome of one publish/collect cycle, mirroring the three branches of sec. 4.4 king steps 2-5.
enum class DispatchResult {
    /// `valetId` was 0: no client attached, input copied to output unchanged.
    NoValet,
    /// The valet completed the rendezvous; the output holds what it wrote.
    Processed,
    /// The valet missed the deadline. It has been evicted (`valetId` set to 0) and the input was
    /// copied through unchanged.
    ValetTimedOut,
};

class SyntheticKing {
public:
    /// Sec. 4.4 king step 5 passes 1000 ms. Tests that deliberately provoke eviction override it,
    /// because a real 1000 ms wait per block makes for a slow suite.
    static constexpr DWORD kDefaultValetTimeoutMs = 1000;

    explicit SyntheticKing(std::wstring objectBase) : base_(std::move(objectBase)) {}

    ~SyntheticKing() { close(); }

    SyntheticKing(const SyntheticKing&) = delete;
    SyntheticKing& operator=(const SyntheticKing&) = delete;

    /// Creates the mapping and both events, per sec. 4.2 and sec. 4.5: KING signaled, VALET
    /// cleared, and `valetId` zeroed only if this call created the section fresh.
    [[nodiscard]] bool open(std::uint32_t sampleRate, std::uint32_t channelCount);

    /// What `LockForProcess` calls on the deployed APO (sec. 4.5) -- **including its bug**.
    ///
    /// The condition below is `&&` where `||` was meant (sec. 3.7.3). A format change that alters
    /// only the sample rate therefore does not reopen the stream, and the king keeps publishing
    /// the *previous* `sampleRate` in the header while the audio is really at the new one.
    /// Reproduced verbatim: this is the case the client must tolerate by re-reading the header
    /// every block and never caching it (sec. 4.5).
    void smartOpen(std::uint32_t sampleRate, std::uint32_t channelCount);

    void smartClose();

    /// Clears both events and releases all handles (sec. 4.5). The valet then sees its waits fail
    /// or time out and must re-attach.
    void close();

    [[nodiscard]] bool opened() const noexcept { return opened_; }

    /// One block. `size` is the **total** sample count, `frames * channelCount` (sec. 4.3).
    /// `interleavedIn` and `interleavedOut` may alias -- the APO declares `APO_FLAG_INPLACE`.
    DispatchResult dispatch(const float* interleavedIn, float* interleavedOut,
                            std::int32_t size);

    /// Publishes a block whose header deliberately violates sec. 4.3, to exercise the valet's
    /// tolerance of a hostile or broken writer (any process can write here -- sec. 3.7.2).
    DispatchResult dispatchRawHeader(std::uint32_t sampleRate, std::uint32_t channelCount,
                                     std::int32_t size);

    void setValetTimeoutMs(DWORD ms) noexcept { valetTimeoutMs_ = ms; }

    [[nodiscard]] std::uint32_t valetIdInHeader() const noexcept { return header_.valetId(); }

    /// Impersonates a second client claiming the stream -- the `Stolen` path of sec. 4.1/sec. 4.4.
    void forceValetId(std::uint32_t id) noexcept { header_.setValetId(id); }

    /// The sample rate this king *believes* it is running at, i.e. what it publishes. After a
    /// sample-rate-only `smartOpen` this diverges from reality; that divergence is the defect.
    [[nodiscard]] std::uint32_t publishedSampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] std::uint32_t publishedChannelCount() const noexcept { return channelCount_; }

    [[nodiscard]] const std::wstring& objectBase() const noexcept { return base_; }

private:
    std::wstring base_;
    ipc::ManualEvent kingEvent_;
    ipc::ManualEvent valetEvent_;
    ipc::SharedMapping mapping_;
    protocol::HeaderAccess header_;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t channelCount_ = 0;
    DWORD valetTimeoutMs_ = kDefaultValetTimeoutMs;
    bool opened_ = false;
};

/// A unique object base name for one test.
///
/// Tests use the `Local\` (per-session) namespace rather than the `Global\` prefix that sec. 4.2
/// mandates for real endpoints: creating a `Global\` named object requires
/// SeCreateGlobalPrivilege, which a non-elevated test runner does not hold. Only the *creating*
/// side needs it, so this affects the harness and never the shipping client -- the valet only
/// ever opens objects that `audiodg.exe` created. The exact sec. 4.2 name construction is covered
/// separately by the layout tests.
[[nodiscard]] std::wstring uniqueTestObjectBase(std::wstring_view tag);

} // namespace aip::harness
