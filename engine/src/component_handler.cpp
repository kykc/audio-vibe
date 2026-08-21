#include "aip/engine/component_handler.h"

using Steinberg::kOutOfMemory;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::tresult;

namespace aip::engine {

tresult PLUGIN_API ComponentHandler::beginEdit(Steinberg::Vst::ParamID id) {
    return submit(ParameterEdit{ParameterEdit::Kind::BeginEdit, id, 0.0, 0});
}

tresult PLUGIN_API ComponentHandler::performEdit(Steinberg::Vst::ParamID id,
                                                 Steinberg::Vst::ParamValue value) {
    return submit(ParameterEdit{ParameterEdit::Kind::PerformEdit, id, value, 0});
}

tresult PLUGIN_API ComponentHandler::endEdit(Steinberg::Vst::ParamID id) {
    return submit(ParameterEdit{ParameterEdit::Kind::EndEdit, id, 0.0, 0});
}

tresult PLUGIN_API ComponentHandler::restartComponent(Steinberg::int32 flags) {
    return submit(ParameterEdit{ParameterEdit::Kind::RestartComponent, 0, 0.0, flags});
}

tresult ComponentHandler::submit(const ParameterEdit& edit) noexcept {
    if (onAudioThread()) {
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
