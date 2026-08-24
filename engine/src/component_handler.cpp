#include "aip/engine/component_handler.h"

using Steinberg::kOutOfMemory;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::tresult;

namespace aip::engine {

// `origin` is deliberately left out of every one of these: only `submit` can know it, because
// only `submit` runs on the calling thread.

tresult PLUGIN_API ComponentHandler::beginEdit(Steinberg::Vst::ParamID id) {
    return submit(ParameterEdit{.kind = ParameterEdit::Kind::BeginEdit, .paramId = id});
}

tresult PLUGIN_API ComponentHandler::performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) {
    return submit(ParameterEdit{.kind = ParameterEdit::Kind::PerformEdit, .paramId = id, .value = value});
}

tresult PLUGIN_API ComponentHandler::endEdit(Steinberg::Vst::ParamID id) {
    return submit(ParameterEdit{.kind = ParameterEdit::Kind::EndEdit, .paramId = id});
}

tresult PLUGIN_API ComponentHandler::restartComponent(Steinberg::int32 flags) {
    return submit(ParameterEdit{.kind = ParameterEdit::Kind::RestartComponent, .flags = flags});
}

tresult ComponentHandler::submit(ParameterEdit edit) noexcept {
    if (onAudioThread()) {
        edit.origin = ParameterEdit::Origin::Audio;
        // The whole audio-thread path, in full: one ring push and a relaxed counter. No lock, no
        // allocation, no syscall (sec. 7.4.5). A full ring drops the edit rather than blocking --
        // stalling here would stall audiodg.exe (sec. 3.7.1).
        if (!audioQueue_.push(edit)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return kResultFalse;
        }
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return kResultOk;
    }

    // Any other thread is control plane by definition, and control plane may lock and allocate.
    edit.origin = ParameterEdit::Origin::Control;
    try {
        const rt::ScopedLock lock(controlMutex_);
        controlEdits_.push_back(edit);
    } catch (...) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return kOutOfMemory;
    }
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return kResultOk;
}

} // namespace aip::engine
