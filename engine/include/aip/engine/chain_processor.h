// The BlockProcessor the valet thread calls (design_doc.md sec. 7.4.3).
//
// This class is the entire audio-thread surface of `engine/`, and it is deliberately tiny. Per
// block it does: mark the thread, bump an epoch, read one atomic pointer, compare the block's
// geometry against the chain's, and dispatch. Sec. 7.4.3 says that if what runs on the audio
// thread amounts to more than a pointer read, the work was not pushed far enough upstream -- so
// everything else lives on the control thread, in Engine.
//
// Retirement. Publishing a new chain leaves the old one possibly in use: the audio thread may be
// inside `process` at that instant. The epoch counter resolves it exactly rather than by
// guesswork -- see `publish` -- so a replaced chain is destroyed on the control thread, as
// sec. 7.4.3 step 4 requires, and provably not before the audio thread has let go of it.

#pragma once

#include "aip/engine/plugin_chain.h"
#include "aip/engine/stream_format.h"
#include "aip/ipc/valet_thread.h"

#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace aip::engine {

class ChainProcessor final : public ipc::BlockProcessor {
public:
    ChainProcessor();
    ~ChainProcessor() override;

    ChainProcessor(const ChainProcessor&) = delete;
    ChainProcessor& operator=(const ChainProcessor&) = delete;

    // ------------------------------------------------------------------ audio thread ---------

    /// Runs the published chain over the block, or passes the block through untouched when the
    /// chain is bypassed, when there is no chain, or when the block's geometry is not the one the
    /// chain was built for. Passing through is the only safe response to a format change:
    /// rebuilding is control-plane work (sec. 7.4.3) and the change is reported through
    /// `observedFormat`.
    void processBlock(ipc::BlockInfo& block) noexcept override;

    // ------------------------------------------------------------------- chain bypass --------
    //
    // Either thread. The whole-chain bypass is one atomic bool rather than a retracted chain, and
    // that is the point of it: the rack stays loaded, every plugin stays prepared and keeps its
    // parameters, and switching back costs the audio thread the same load it already does. A
    // bypass that unpublished the chain would deactivate the plugins and pay for a full rebuild
    // to come back, which is not what a user comparing processed against unprocessed is asking
    // for.
    //
    // What a bypassed block gets is the king's own samples, handed back exactly as they arrived:
    // `processBlock` returns before the chain is even read.

    void setBypassed(bool bypassed) noexcept {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }

    [[nodiscard]] bool bypassed() const noexcept {
        return bypassed_.load(std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- control thread ---------

    /// Publishes `chain` and destroys whatever it replaced. Returns true when the old chain was
    /// destroyed here; false when the audio thread did not leave it within `graceMs`, in which
    /// case it is parked and destroyed when this object is. A parked chain is a leak until then,
    /// not a crash -- the alternative, freeing memory the audio thread is reading, is worse.
    ///
    /// Passing a null chain is how processing is switched off.
    bool publish(std::unique_ptr<PluginChain> chain, unsigned graceMs = 1000);

    /// The currently published chain, or null. Valid to dereference only from the control
    /// thread, which is the only thread that can replace it.
    [[nodiscard]] PluginChain* current() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    /// The geometry of the most recent block, whether or not a chain could run it. Zeroed until
    /// the first block arrives. `maxFrames` reports the frame count actually observed, not a
    /// bound. This is how the control thread learns what to build for in the first place, not
    /// only what to rebuild for -- on a fresh attach there is no chain and so no mismatch, but
    /// the format still has to come from somewhere, and the header is the only source (sec. 4.5).
    [[nodiscard]] StreamFormat observedFormat() const noexcept;

    /// The same value in the packed form the audio thread stores, so the control thread can tell
    /// "unchanged since I last looked" from "changed back" without three separate comparisons.
    [[nodiscard]] std::uint64_t observedFormatKey() const noexcept {
        return observedFormat_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t blocksProcessed() const noexcept {
        return blocksProcessed_.load(std::memory_order_relaxed);
    }

    /// Blocks handed straight back to the king untouched: no chain published, or a geometry the
    /// chain was not built for.
    [[nodiscard]] std::uint64_t blocksPassedThrough() const noexcept {
        return blocksPassedThrough_.load(std::memory_order_relaxed);
    }

    /// Blocks the published chain could not run because their geometry was not the one it was
    /// built for. Distinct from `blocksPassedThrough`, which also counts "no chain at all".
    [[nodiscard]] std::uint64_t formatMismatches() const noexcept {
        return formatMismatches_.load(std::memory_order_relaxed);
    }

    /// Blocks handed back untouched because the chain was bypassed. A subset of
    /// `blocksPassedThrough`, in the same way `formatMismatches` is: both say *why* a block was
    /// passed through, and a chain that is doing nothing on purpose has to be distinguishable
    /// from one that is doing nothing because it could not run.
    [[nodiscard]] std::uint64_t blocksBypassed() const noexcept {
        return blocksBypassed_.load(std::memory_order_relaxed);
    }

private:
    /// Even means the audio thread is outside `processBlock`; odd means it is inside. The audio
    /// thread raises it *before* loading `current_`, which is what makes an even reading taken
    /// after a store to `current_` a proof that the next entry will see the new pointer.
    /// Both sides are sequentially consistent on purpose: a release store followed by an acquire
    /// load is exactly the pattern x86 is allowed to reorder.
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<PluginChain*> current_{nullptr};
    /// See setBypassed(). Relaxed on both sides: it orders nothing and guards nothing -- the
    /// worst a stale read can cost is one block processed either side of the user's click, which
    /// is well inside what sec. 7.4.3 already permits a user-initiated transition to sound like.
    std::atomic<bool> bypassed_{false};

    std::unique_ptr<PluginChain> owned_;
    std::vector<std::unique_ptr<PluginChain>> parked_;

    /// Rebuilt per block from the shared header, which is the only source of truth for format
    /// and may change under us at any time (sec. 4.5).
    Steinberg::Vst::ProcessContext context_{};
    std::int64_t samplePosition_ = 0;

    std::atomic<std::uint64_t> observedFormat_{0};
    std::atomic<std::uint64_t> blocksProcessed_{0};
    std::atomic<std::uint64_t> blocksPassedThrough_{0};
    std::atomic<std::uint64_t> formatMismatches_{0};
    std::atomic<std::uint64_t> blocksBypassed_{0};
};

} // namespace aip::engine
