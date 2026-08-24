#include "synthetic_king.h"

#include "aip/protocol/planar.h"

#include <atomic>
#include <cstring>

namespace aip::harness {

namespace {

std::atomic<std::uint32_t> gTestNameCounter{0};

} // namespace

std::wstring uniqueTestObjectBase(std::wstring_view tag) {
    std::wstring name = L"Local\\TOMATL.AUDIO.IPC.TEST.";
    name.append(std::to_wstring(::GetCurrentProcessId()));
    name.push_back(L'.');
    name.append(std::to_wstring(gTestNameCounter.fetch_add(1)));
    name.push_back(L'.');
    name.append(tag);
    return name;
}

bool SyntheticKing::open(std::uint32_t sampleRate, std::uint32_t channelCount) {
    close();

    if (!ipc::SharedMapping::openOrCreate(base_, protocol::kMappingSize, mapping_)) {
        return false;
    }
    // KING is created signaled, VALET non-signaled (sec. 4.2).
    if (!ipc::ManualEvent::create(protocol::kingEventName(base_), true, kingEvent_)) {
        return false;
    }
    if (!ipc::ManualEvent::create(protocol::valetEventName(base_), false, valetEvent_)) {
        return false;
    }

    header_ = protocol::HeaderAccess(mapping_.data());
    sampleRate_ = sampleRate;
    channelCount_ = channelCount;

    // Sec. 4.5: on open, set KING and clear VALET.
    kingEvent_.set();
    valetEvent_.reset();

    // Only zero the claim if we created the section; an existing valet keeps its claim across a
    // king restart, which is what makes the stream resume by itself.
    if (mapping_.createdNew()) {
        header_.setValetId(protocol::kNoValet);
    }

    opened_ = true;
    return true;
}

void SyntheticKing::smartOpen(std::uint32_t sampleRate, std::uint32_t channelCount) {
    if (!opened_) {
        (void)open(sampleRate, channelCount);
        return;
    }
    // Faithful to the deployed APO, bug included: `&&` where `||` was meant (sec. 3.7.3).
    if (sampleRate != sampleRate_ && channelCount != channelCount_) {
        close();
        (void)open(sampleRate, channelCount);
    }
}

void SyntheticKing::smartClose() {
    if (opened_) {
        close();
    }
}

void SyntheticKing::close() {
    if (opened_) {
        kingEvent_.reset();
        valetEvent_.reset();
    }
    kingEvent_.close();
    valetEvent_.close();
    mapping_.close();
    header_ = protocol::HeaderAccess();
    sampleRate_ = 0;
    channelCount_ = 0;
    opened_ = false;
}

DispatchResult SyntheticKing::dispatch(const float* interleavedIn, float* interleavedOut, std::int32_t size) {
    // Sec. 4.4 king steps 1-2.
    if (header_.valetId() == protocol::kNoValet) {
        if (interleavedOut != interleavedIn) {
            std::memcpy(interleavedOut, interleavedIn, static_cast<std::size_t>(size) * sizeof(float));
        }
        return DispatchResult::NoValet;
    }

    // Step 3: publish geometry and the de-interleaved payload.
    header_.setSampleRate(sampleRate_);
    header_.setChannelCount(channelCount_);
    header_.setSize(size);
    protocol::deinterleave(interleavedIn, header_.payload(), size, channelCount_);

    // Step 4.
    kingEvent_.reset();
    valetEvent_.set();

    // Step 5.
    if (kingEvent_.wait(valetTimeoutMs_) == ipc::WaitResult::Signaled) {
        protocol::reinterleave(header_.payload(), interleavedOut, size, channelCount_);
        return DispatchResult::Processed;
    }

    header_.setValetId(protocol::kNoValet); // evict
    if (interleavedOut != interleavedIn) {
        std::memcpy(interleavedOut, interleavedIn, static_cast<std::size_t>(size) * sizeof(float));
    }
    return DispatchResult::ValetTimedOut;
}

DispatchResult SyntheticKing::dispatchRawHeader(
    std::uint32_t sampleRate, std::uint32_t channelCount, std::int32_t size) {
    if (header_.valetId() == protocol::kNoValet) {
        return DispatchResult::NoValet;
    }

    header_.setSampleRate(sampleRate);
    header_.setChannelCount(channelCount);
    header_.setSize(size);

    kingEvent_.reset();
    valetEvent_.set();

    if (kingEvent_.wait(valetTimeoutMs_) == ipc::WaitResult::Signaled) {
        return DispatchResult::Processed;
    }
    header_.setValetId(protocol::kNoValet);
    return DispatchResult::ValetTimedOut;
}

} // namespace aip::harness
