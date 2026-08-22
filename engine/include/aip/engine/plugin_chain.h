// A finished, ready-to-run plugin chain (design_doc.md sec. 7.4.3).
//
// This is the "hand over a finished object" half of sec. 7.4.3. A chain is built entirely on the
// control thread -- scratch memory allocated and every page touched -- and is *immutable*
// thereafter. The audio thread never adds, removes or reorders anything; it reads one atomic
// pointer (see ChainProcessor) and calls `process`.
//
// A chain does **not** own its plugins; Engine does, and a chain is an ordered view over them.
// That split is what makes chain mutation non-destructive: adding, removing, reordering or
// bypassing a plugin, or re-preparing the rack for a new sample rate, publishes a fresh view
// while the instances themselves survive -- and with them every parameter the user has set.
// An owning chain gets this wrong in a way that looks like a UI bug: add a second plugin, and
// the first one silently reverts to its defaults.
//
// Buffering. The king shares its payload in place: the same planar memory is both our input and
// our output (sec. 4.3). VST3 makes no promise that a plugin tolerates in-place processing, so
// the chain ping-pongs between two scratch banks instead:
//
//     plugin 0 reads the shared mapping and writes bank A
//     plugin 1 reads bank A and writes bank B, plugin 2 reads B and writes A, ...
//     the final bank is copied back over the shared mapping
//
// One copy per block, never an aliased input and output, and the same code path for every chain
// length.
//
// Width. The banks are as wide as the *widest bus any plugin settled on*, which is the stream's
// width for nearly every chain and more than that when one of the plugins insisted on a fixed
// wider bus (PluginInstance::inputChannelCount). The surplus channels are padding:
//
//     channels [0, channelCount)  the stream, and the only ones ever written back
//     channels [channelCount, W)  silence in, discarded out
//
// The padding is re-zeroed before each plugin runs rather than filled once, because each plugin
// writes its *own* full output width into the destination bank -- so by the time the next plugin
// reads that bank, what was silence holds whatever the previous plugin made of it. Filling once
// would work for a one-plugin chain and quietly feed the second plugin a reverb tail.

#pragma once

#include "aip/engine/plugin_instance.h"
#include "aip/engine/stream_format.h"
#include "aip/protocol/planar.h"

#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <cstdint>
#include <vector>

namespace aip::engine {

class PluginChain {
public:
    /// Control thread. `plugins` are borrowed, in processing order, and must outlive the chain
    /// -- Engine guarantees that by never destroying an instance a published or parked chain
    /// still names. Each must already be prepared for `format`; the chain does not prepare them,
    /// because a failure to prepare has to be reportable before anything is published.
    PluginChain(StreamFormat format, std::vector<PluginInstance*> plugins);

    PluginChain(const PluginChain&) = delete;
    PluginChain& operator=(const PluginChain&) = delete;

    [[nodiscard]] const StreamFormat& format() const noexcept { return format_; }

    [[nodiscard]] std::size_t size() const noexcept { return plugins_.size(); }

    [[nodiscard]] PluginInstance& at(std::size_t index) const noexcept {
        return *plugins_[index];
    }

    /// Channels each scratch bank holds: the stream's width, or the widest bus a plugin in this
    /// chain settled on if that is wider. Never less than `format().channelCount`.
    [[nodiscard]] std::uint32_t bankChannelCount() const noexcept { return bankChannels_; }

    /// True when every plugin is prepared for this chain's format and the scratch banks are
    /// allocated wide enough for all of them. A chain that is not runnable must not be published.
    [[nodiscard]] bool runnable() const noexcept;

    // ------------------------------------------------------------------ audio thread ---------

    /// Runs every plugin over `audio` in place, from the caller's point of view. `audio` must
    /// match this chain's format: `channelCount` exactly, `frameCount()` no greater than
    /// `maxFrames`. ChainProcessor checks that; this does not re-check it.
    ///
    /// Allocation-free and lock-free on our side. An empty chain is a no-op, so `audio` is left
    /// bit-for-bit as the king published it.
    void process(protocol::PlanarView& audio, Steinberg::Vst::ProcessContext& context) noexcept;

private:
    /// `channelCount` contiguous channels of `maxFrames` floats, plus the pointer array VST3
    /// wants. Allocated and fully written once, at construction.
    struct Bank {
        std::vector<float> samples;
        std::vector<float*> channels;

        void allocate(std::uint32_t channelCount, std::int32_t maxFrames);
    };

    /// Audio thread. Zeroes channels `[format_.channelCount, upTo)` of `channels` for `frames`
    /// samples. A memset of at most (W - channelCount) * frames floats, which is nothing beside
    /// what the plugin is about to do with them -- and skipping it is how the padding stops
    /// being silence after the first plugin in the chain.
    void silencePadding(float* const* channels, std::uint32_t upTo,
                        std::int32_t frames) const noexcept;

    StreamFormat format_;
    std::vector<PluginInstance*> plugins_;
    /// The width of everything below. See bankChannelCount().
    std::uint32_t bankChannels_ = 0;
    Bank banks_[2];
    /// Refilled per block: channels `[0, format_.channelCount)` point into the king's mapping,
    /// the rest into `firstPad_`. Preallocated so the audio thread only ever stores into it.
    std::vector<float*> sharedChannels_;
    /// Padding storage for the first plugin only, because its real channels live in the king's
    /// mapping and that is exactly `format_.channelCount` wide -- there is nothing there to
    /// borrow for the surplus. Its own storage rather than one shared zero buffer aliased across
    /// every padding channel: a plugin that writes to its input would otherwise corrupt all of
    /// them at once, and not aliasing buffers is the whole reason this class ping-pongs.
    Bank firstPad_;
};

} // namespace aip::engine
