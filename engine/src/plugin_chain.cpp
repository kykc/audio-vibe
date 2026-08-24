#include "aip/engine/plugin_chain.h"

#include <algorithm>
#include <cstring>

namespace Vst = Steinberg::Vst;

namespace aip::engine {
namespace {

// The width every scratch bank has to be: the stream's, unless a plugin insisted on a wider bus,
// in which case the widest such bus. Control thread -- the numbers are fixed by prepare() and a
// chain is immutable, so this is computed once and never revisited.
std::uint32_t widestBus(std::uint32_t streamChannels, const std::vector<PluginInstance*>& plugins) noexcept {
    std::uint32_t widest = streamChannels;
    for (const PluginInstance* plugin : plugins) {
        if (plugin == nullptr) {
            continue;
        }
        widest = std::max({widest, plugin->inputChannelCount(), plugin->outputChannelCount()});
    }
    return widest;
}

} // namespace

void PluginChain::Bank::allocate(std::uint32_t channelCount, std::int32_t maxFrames) {
    // assign(), not resize(): every element is written, which is what faults in every page
    // before the audio thread ever reaches it (sec. 7.4.2).
    samples.assign(static_cast<std::size_t>(channelCount) * static_cast<std::size_t>(maxFrames), 0.0f);
    channels.resize(channelCount);
    for (std::uint32_t c = 0; c < channelCount; ++c) {
        channels[c] = samples.data() + static_cast<std::ptrdiff_t>(c) * maxFrames;
    }
}

PluginChain::PluginChain(StreamFormat format, std::vector<PluginInstance*> plugins)
    : format_(format), plugins_(std::move(plugins)) {
    if (!format_.valid()) {
        return;
    }
    bankChannels_ = widestBus(format_.channelCount, plugins_);
    banks_[0].allocate(bankChannels_, format_.maxFrames);
    banks_[1].allocate(bankChannels_, format_.maxFrames);
    firstPad_.allocate(bankChannels_ - format_.channelCount, format_.maxFrames);

    // The king's half of `sharedChannels_` is refilled per block; the padding half is not, and is
    // wired up once here. Nothing but a misbehaving plugin ever writes through these pointers,
    // and process() re-zeroes what it hands out regardless.
    sharedChannels_.assign(bankChannels_, nullptr);
    for (std::uint32_t c = format_.channelCount; c < bankChannels_; ++c) {
        sharedChannels_[c] = firstPad_.channels[c - format_.channelCount];
    }
}

bool PluginChain::runnable() const noexcept {
    if (!format_.valid() || sharedChannels_.size() != bankChannels_ || bankChannels_ < format_.channelCount) {
        return false;
    }
    return std::all_of(plugins_.begin(), plugins_.end(), [this](const PluginInstance* plugin) {
        return plugin != nullptr && plugin->prepared() && plugin->format() == format_ &&
            plugin->inputChannelCount() <= bankChannels_ && plugin->outputChannelCount() <= bankChannels_;
    });
}

void PluginChain::silencePadding(float* const* channels, std::uint32_t upTo, std::int32_t frames) const noexcept {
    const std::size_t bytes = static_cast<std::size_t>(frames) * sizeof(float);
    for (std::uint32_t c = format_.channelCount; c < upTo; ++c) {
        std::memset(channels[c], 0, bytes);
    }
}

void PluginChain::process(protocol::PlanarView& audio, Vst::ProcessContext& context) noexcept {
    const std::size_t count = plugins_.size();
    if (count == 0) {
        return;
    }

    const std::uint32_t channels = format_.channelCount;
    const std::int32_t frames = audio.frameCount();

    for (std::uint32_t c = 0; c < channels; ++c) {
        sharedChannels_[c] = audio.channel(c);
    }

    // The first plugin reads the king's mapping directly; after that the banks alternate, so no
    // plugin ever sees the same pointer as both input and output.
    float** source = sharedChannels_.data();
    std::size_t bank = 0;
    for (std::size_t i = 0; i < count; ++i) {
        // Only up to *this* plugin's input width: a plugin that took the stream width reads no
        // padding at all, and zeroing what nobody reads is work for its own sake.
        silencePadding(source, plugins_[i]->inputChannelCount(), frames);

        float** destination = banks_[bank].channels.data();
        plugins_[i]->process(source, destination, frames, context);
        source = destination;
        bank ^= 1u;
    }

    // `source` now names the bank the last plugin wrote. Publish it back into the shared
    // mapping, which is where the king expects to read it (sec. 4.3). Only the stream's own
    // channels: whatever the last plugin made of the padding is not ours to carry anywhere.
    const std::size_t bytes = static_cast<std::size_t>(frames) * sizeof(float);
    for (std::uint32_t c = 0; c < channels; ++c) {
        std::memcpy(sharedChannels_[c], source[c], bytes);
    }
}

} // namespace aip::engine
