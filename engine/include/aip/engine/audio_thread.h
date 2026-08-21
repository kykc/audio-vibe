// Audio-thread identity (design_doc.md sec. 7.4.5).
//
// A VST3 plugin may call back into our IComponentHandler from either the processing thread or
// the UI thread, and the two have opposite obligations: the first must enqueue lock-free and
// return, the second is ordinary control-plane work. The handler cannot tell them apart from
// the call itself, so the audio thread marks itself.
//
// This is deliberately *not* `rt::RealtimeGuard`. That guard answers "are the sec. 7.4.1 rules
// in force here", is a debugging instrument, and is compiled out of Release; this answers "which
// thread am I on", is load-bearing for correctness, and is compiled in unconditionally.

#pragma once

namespace aip::engine {

namespace detail {
inline thread_local bool tlsOnAudioThread = false;
} // namespace detail

/// True when the calling thread is the promoted valet thread, inside block processing.
[[nodiscard]] inline bool onAudioThread() noexcept { return detail::tlsOnAudioThread; }

/// RAII marker. One is placed at the top of ChainProcessor::processBlock, which is the only
/// entry point the audio thread has into `engine/`.
class AudioThreadMarker {
public:
    AudioThreadMarker() noexcept : previous_(detail::tlsOnAudioThread) {
        detail::tlsOnAudioThread = true;
    }

    ~AudioThreadMarker() noexcept { detail::tlsOnAudioThread = previous_; }

    AudioThreadMarker(const AudioThreadMarker&) = delete;
    AudioThreadMarker& operator=(const AudioThreadMarker&) = delete;

private:
    bool previous_;
};

} // namespace aip::engine
