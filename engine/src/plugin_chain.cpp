#include "aip/engine/plugin_chain.h"

#include <algorithm>
#include <cstring>

namespace Vst = Steinberg::Vst;

namespace aip::engine {

void PluginChain::Bank::allocate(std::uint32_t channelCount, std::int32_t maxFrames) {
    // assign(), not resize(): every element is written, which is what faults in every page
    // before the audio thread ever reaches it (sec. 7.4.2).
    samples.assign(static_cast<std::size_t>(channelCount) * static_cast<std::size_t>(maxFrames),
                   0.0f);
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
    banks_[0].allocate(format_.channelCount, format_.maxFrames);
    banks_[1].allocate(format_.channelCount, format_.maxFrames);
    sharedChannels_.assign(format_.channelCount, nullptr);
}

bool PluginChain::runnable() const noexcept {
    if (!format_.valid() || sharedChannels_.size() != format_.channelCount) {
        return false;
    }
    return std::all_of(plugins_.begin(), plugins_.end(), [this](const PluginInstance* plugin) {
        return plugin != nullptr && plugin->prepared() && plugin->format() == format_;
    });
}

void PluginChain::process(protocol::PlanarView& audio,
                          Vst::ProcessContext& context) noexcept {
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
        float** destination = banks_[bank].channels.data();
        plugins_[i]->process(source, destination, frames, context);
        source = destination;
        bank ^= 1u;
    }

    // `source` now names the bank the last plugin wrote. Publish it back into the shared
    // mapping, which is where the king expects to read it (sec. 4.3).
    const std::size_t bytes = static_cast<std::size_t>(frames) * sizeof(float);
    for (std::uint32_t c = 0; c < channels; ++c) {
        std::memcpy(sharedChannels_[c], source[c], bytes);
    }
}

} // namespace aip::engine
