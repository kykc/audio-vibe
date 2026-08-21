// Our IComponentHandler (design_doc.md sec. 7.4.5).
//
// The interface documents every one of these methods as "must be called in the UI-Thread
// context", and plugins nevertheless call `performEdit` from their processing thread -- the SDK
// itself notes this, and sec. 7.4.5 makes tolerating it our obligation rather than theirs. So
// the handler routes by *caller*, not by contract:
//
//   audio thread    push onto a lock-free SPSC ring and return; drop on overflow, never block
//   any other       ordinary control-plane work: take a lock, append to a vector
//
// The two paths cannot be merged. `rt::SpscQueue` has exactly one producer, and the audio thread
// is it; a UI thread pushing to the same ring would corrupt it. Splitting by caller is what
// keeps the ring single-producer while still accepting edits from the editor.
//
// Which caller it was is *recorded*, not just used for routing, because it decides where the edit
// has to go afterwards. An edit the processor made is news to the controller; an edit the editor
// made is news to the processor. Sending each one back where it came from is at best redundant
// and at worst a feedback loop that makes a knob fight the mouse -- see `ParameterEdit::Origin`
// and `Engine::serviceParameterEdits`.
//
// `restartComponent` is never acted on here. It can demand a full reconfiguration -- exactly the
// work sec. 7.4.3 says must happen on the control thread -- so it is queued like everything else.

#pragma once

#include "aip/engine/audio_thread.h"
#include "aip/rt/mutex.h"
#include "aip/rt/spsc_queue.h"

#include "base/source/fobject.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace aip::engine {

/// One callback from a plugin, flattened into something trivially copyable so it can live in a
/// fixed-capacity ring (sec. 7.4.2).
struct ParameterEdit {
    enum class Kind : std::uint8_t {
        BeginEdit,
        PerformEdit,
        EndEdit,
        RestartComponent,
    };

    /// Which side of the plugin the call came from, decided by the thread that made it. The
    /// contract says every one of these is a UI-thread call, so `Audio` is the case the contract
    /// says cannot happen and sec. 7.4.5 says we must expect anyway.
    enum class Origin : std::uint8_t {
        /// Called from the plugin's processing thread: the processor moved the parameter itself.
        Audio,
        /// Called from anywhere else -- in practice the plugin's editor, which is the case that
        /// makes a user's mouse gesture something the processor has to be told about.
        Control,
    };

    Kind kind = Kind::PerformEdit;
    Origin origin = Origin::Control;
    Steinberg::Vst::ParamID paramId = 0;
    Steinberg::Vst::ParamValue value = 0.0;
    /// RestartFlags, for `RestartComponent`. Unused otherwise.
    Steinberg::int32 flags = 0;
};

static_assert(std::is_trivially_copyable_v<ParameterEdit>);

class ComponentHandler final : public Steinberg::FObject, public Steinberg::Vst::IComponentHandler {
public:
    /// One block at 48 kHz / 480 frames is 10 ms. A plugin automating every parameter of a
    /// large synth every block still fits; anything past this is a runaway and is dropped and
    /// counted rather than allowed to grow the ring.
    static constexpr std::size_t kAudioQueueSlots = 1024;

    ComponentHandler() = default;

    // --- IComponentHandler, callable from any thread ------------------------------------------

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue value) override;
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

    // --- control thread -----------------------------------------------------------------------

    /// Applies `fn` to at most `maxItems` queued edits from the audio thread, then to everything
    /// the control-plane path collected. Returns the number consumed.
    template <typename Fn>
    std::size_t drain(std::size_t maxItems, Fn&& fn) {
        std::size_t consumed = audioQueue_.drain(maxItems, fn);

        std::vector<ParameterEdit> pending;
        {
            const rt::ScopedLock lock(controlMutex_);
            pending.swap(controlEdits_);
        }
        for (const ParameterEdit& edit : pending) {
            fn(edit);
            ++consumed;
        }
        return consumed;
    }

    /// Edits dropped because the audio-thread ring was full. Nonzero means a plugin is calling
    /// back faster than the control thread drains, which is worth surfacing.
    [[nodiscard]] std::uint64_t droppedEdits() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    /// Total accepted edits, both paths. Test and diagnostic aid.
    [[nodiscard]] std::uint64_t acceptedEdits() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

    OBJ_METHODS(ComponentHandler, Steinberg::FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(Steinberg::Vst::IComponentHandler)
    END_DEFINE_INTERFACES(Steinberg::FObject)
    REFCOUNT_METHODS(Steinberg::FObject)

private:
    /// By value: `submit` stamps the origin from the thread it is running on, which is the one
    /// piece of information the caller cannot supply.
    Steinberg::tresult submit(ParameterEdit edit) noexcept;

    rt::SpscQueue<ParameterEdit, kAudioQueueSlots> audioQueue_;
    rt::Mutex controlMutex_;
    std::vector<ParameterEdit> controlEdits_;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> accepted_{0};
};

} // namespace aip::engine
