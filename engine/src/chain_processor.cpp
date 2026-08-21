#include "aip/engine/chain_processor.h"

#include "aip/engine/audio_thread.h"

#include <windows.h>

namespace Vst = Steinberg::Vst;

namespace aip::engine {
namespace {

// The geometry fields, squeezed into one 64-bit atomic so the control thread reads a triple that
// actually occurred rather than a mix of two. Everything protocol v1 can carry fits: `size` is
// capped at 262,140 samples (sec. 4.3), so a frame count needs 18 bits, and a channel count
// beyond 255 could never be built for anyway.
constexpr std::uint64_t kChannelShift = 24;
constexpr std::uint64_t kRateShift = 32;
constexpr std::uint64_t kFrameMask = 0xffffffu;
constexpr std::uint64_t kChannelMask = 0xffu;

std::uint64_t packFormat(std::uint32_t sampleRate, std::uint32_t channelCount,
                         std::int32_t frames) noexcept {
    if (channelCount > kChannelMask || frames < 0 ||
        static_cast<std::uint64_t>(frames) > kFrameMask) {
        return 0;
    }
    return (static_cast<std::uint64_t>(sampleRate) << kRateShift) |
           (static_cast<std::uint64_t>(channelCount) << kChannelShift) |
           static_cast<std::uint64_t>(frames);
}

// Raised for the duration of one processBlock, so the control thread can tell whether the audio
// thread is inside the chain.
class EpochScope {
public:
    explicit EpochScope(std::atomic<std::uint64_t>& epoch) noexcept : epoch_(epoch) {
        epoch_.fetch_add(1, std::memory_order_seq_cst);
    }

    ~EpochScope() noexcept { epoch_.fetch_add(1, std::memory_order_seq_cst); }

    EpochScope(const EpochScope&) = delete;
    EpochScope& operator=(const EpochScope&) = delete;

private:
    std::atomic<std::uint64_t>& epoch_;
};

} // namespace

ChainProcessor::ChainProcessor() {
    // A plugin that reads no transport still gets a coherent context. There is no transport in a
    // system-wide processor -- Windows is always "playing" -- so the tempo and time signature are
    // declared valid and conventional rather than left absent, which some plugins handle badly.
    context_.state = Vst::ProcessContext::kPlaying | Vst::ProcessContext::kContTimeValid |
                     Vst::ProcessContext::kProjectTimeMusicValid |
                     Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid;
    context_.tempo = 120.0;
    context_.timeSigNumerator = 4;
    context_.timeSigDenominator = 4;
}

ChainProcessor::~ChainProcessor() {
    // Nothing may still be running: the valet thread is stopped before the engine is destroyed.
    current_.store(nullptr, std::memory_order_seq_cst);
    owned_.reset();
    parked_.clear();
}

void ChainProcessor::processBlock(ipc::BlockInfo& block) noexcept {
    const AudioThreadMarker onAudio;
    const EpochScope inChain(epoch_);

    // Recorded unconditionally, and before anything else can return early: the control thread
    // needs the geometry to build the *first* chain as much as to rebuild a stale one, and on a
    // fresh attach there is no chain to mismatch against. One compare, one conditional store.
    const std::int32_t frames = block.audio.frameCount();
    const std::uint64_t observed = packFormat(block.sampleRate, block.channelCount, frames);
    if (observed != observedFormat_.load(std::memory_order_relaxed)) {
        observedFormat_.store(observed, std::memory_order_relaxed);
    }

    PluginChain* chain = current_.load(std::memory_order_seq_cst);
    if (chain == nullptr) {
        blocksPassedThrough_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const StreamFormat& built = chain->format();
    if (block.sampleRate != built.sampleRate || block.channelCount != built.channelCount ||
        frames > built.maxFrames) {
        // Every plugin in the chain was set up for a different geometry. Running it anyway would
        // read past a scratch bank or address the wrong channels, so the block goes back to the
        // king exactly as it arrived and the control thread is left to rebuild.
        formatMismatches_.fetch_add(1, std::memory_order_relaxed);
        blocksPassedThrough_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    context_.sampleRate = static_cast<double>(block.sampleRate);
    context_.projectTimeSamples = samplePosition_;
    context_.continousTimeSamples = samplePosition_;
    samplePosition_ += frames;

    chain->process(block.audio, context_);
    blocksProcessed_.fetch_add(1, std::memory_order_relaxed);
}

bool ChainProcessor::publish(std::unique_ptr<PluginChain> chain, unsigned graceMs) {
    PluginChain* raw = chain.get();
    std::unique_ptr<PluginChain> previous = std::move(owned_);
    owned_ = std::move(chain);
    current_.store(raw, std::memory_order_seq_cst);

    if (!previous) {
        return true;
    }

    // The store above is already visible. An even epoch now means the audio thread is outside
    // the chain, and its next entry raises the epoch before loading the pointer -- so it cannot
    // still pick up the old one. An odd epoch means it is inside; any change to the counter then
    // means that call has returned.
    const std::uint64_t entered = epoch_.load(std::memory_order_seq_cst);
    bool quiescent = (entered % 2) == 0;
    for (unsigned waited = 0; !quiescent && waited < graceMs; ++waited) {
        ::Sleep(1);
        quiescent = epoch_.load(std::memory_order_seq_cst) != entered;
    }

    if (!quiescent) {
        // The valet thread is wedged or the king is holding it. Freeing here would be a
        // use-after-free on the audio thread; parking costs memory until teardown.
        parked_.push_back(std::move(previous));
        return false;
    }

    previous.reset();
    return true;
}

StreamFormat ChainProcessor::observedFormat() const noexcept {
    const std::uint64_t packed = observedFormat_.load(std::memory_order_relaxed);
    StreamFormat format;
    format.sampleRate = static_cast<std::uint32_t>(packed >> kRateShift);
    format.channelCount = static_cast<std::uint32_t>((packed >> kChannelShift) & kChannelMask);
    format.maxFrames = static_cast<std::int32_t>(packed & kFrameMask);
    return format;
}

} // namespace aip::engine
