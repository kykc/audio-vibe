#include "aip/scanner/probe_worker.h"

#include "aip/engine/plugin_instance.h"
#include "aip/engine/plugin_module.h"
#include "aip/engine/stream_format.h"

#include <windows.h>

#include "public.sdk/source/vst/hosting/hostclasses.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <crtdbg.h>
#include <cstdlib>

namespace aip::scanner {

namespace Vst = Steinberg::Vst;

namespace {

/// Channels on the first bus reported as `kMain` in `direction`. A plugin whose main output is
/// not bus 0 is rare but legal, and taking bus 0 blindly would mislabel it.
[[nodiscard]] std::int32_t mainBusChannels(Vst::IComponent& component,
                                           Vst::BusDirection direction) noexcept {
    const Steinberg::int32 count = component.getBusCount(Vst::kAudio, direction);
    for (Steinberg::int32 index = 0; index < count; ++index) {
        Vst::BusInfo bus{};
        if (component.getBusInfo(Vst::kAudio, direction, index, bus) != Steinberg::kResultOk) {
            continue;
        }
        if (bus.busType == Vst::kMain) {
            return bus.channelCount;
        }
    }
    return 0;
}

/// One class of one module, from instantiation to teardown.
///
/// Everything is read *before* `prepare`, because negotiating the bus arrangement changes the
/// answers -- and, when it fails, has already destroyed the state that would explain why
/// (status.md sec. 8 item 14).
[[nodiscard]] ScannedClass probeClass(const engine::PluginModule::Ptr& module,
                                      const engine::PluginClass& classInfo,
                                      Steinberg::FUnknown* hostContext,
                                      const ProbeOptions& options) {
    ScannedClass out;
    out.id = classInfo.id.toString();
    out.name = classInfo.name;
    out.vendor = classInfo.vendor;
    out.version = classInfo.version;
    out.subCategories = classInfo.subCategories;

    std::string error;
    std::unique_ptr<engine::PluginInstance> instance =
        engine::PluginInstance::create(module, classInfo.id, hostContext, error);
    if (!instance) {
        out.error = error.empty() ? "failed to instantiate" : error;
        return out;
    }

    Vst::IComponent* component = instance->component();
    Vst::IEditController* controller = instance->controller();
    const auto processor = Steinberg::U::cast<Vst::IAudioProcessor>(component);

    out.noController = controller == nullptr;
    out.singleComponent = Steinberg::U::cast<Vst::IEditController>(component) != nullptr;
    out.parameterCount = controller != nullptr ? controller->getParameterCount() : 0;
    out.latencySamples = processor ? processor->getLatencySamples() : 0u;
    out.audioInputBusses = component->getBusCount(Vst::kAudio, Vst::kInput);
    out.audioOutputBusses = component->getBusCount(Vst::kAudio, Vst::kOutput);
    out.mainInputChannels = mainBusChannels(*component, Vst::kInput);
    out.mainOutputChannels = mainBusChannels(*component, Vst::kOutput);

    if (options.queryEditor && controller != nullptr) {
        // Created and dropped without ever being attached: no HWND is involved, so nothing here
        // needs a UI thread. Declared after `instance` so it is released before the plugin that
        // made it -- a view outliving its controller is a use-after-free.
        const Steinberg::IPtr<Steinberg::IPlugView> view =
            Steinberg::owned(controller->createView(Vst::ViewType::kEditor));
        out.hasEditor = view != nullptr;
    }

    if (options.prepare) {
        const engine::StreamFormat format{options.sampleRate, options.channelCount,
                                          engine::kDefaultMaxFrames};
        out.prepared = instance->prepare(format, error);
        if (out.prepared) {
            out.fullBusNegotiation = instance->fullBusNegotiation();
        } else {
            // A refusal is an answer, not a defect: a mono-only plugin has every right to decline
            // two channels. It is recorded on the class so the shell can grey it out and say why.
            out.error = error.empty() ? "refused the probe format" : error;
        }
    }
    return out;
}

} // namespace

ScannedModule probeModule(const std::string& path, const ProbeOptions& options) {
    ScannedModule out;
    out.path = path;
    out.status = ScanStatus::LoadFailed;

    // One host context per module rather than one per scan. A plugin that stashes the pointer
    // cannot then be handed one whose owner has moved on, and it costs nothing next to a
    // LoadLibrary. It must outlive every instance made against it, hence the scope.
    const Steinberg::IPtr<Vst::HostApplication> hostContext =
        Steinberg::owned(new Vst::HostApplication());

    std::string error;
    const engine::PluginModule::Ptr module = engine::PluginModule::load(path, error);
    if (!module) {
        out.error = error.empty() ? "failed to load" : error;
        return out;
    }
    module->setHostContext(hostContext);
    out.name = module->name();

    for (const engine::PluginClass& classInfo : module->audioEffects()) {
        out.classes.push_back(probeClass(module, classInfo, hostContext, options));
    }

    out.status = ScanStatus::Ok;
    return out;
}

void suppressCrashDialogs() noexcept {
    // The GPF box and the "there is no disk in the drive" box, both of which block on a click.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // WER's own dialog is separate from the above and has to be turned off separately.
    (void)SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        // Not TerminateProcess: the parent reads the exit code, and taking the process down here
        // rather than letting WER have it is the difference between a millisecond and a timeout.
        // Nothing is flushed on the way out -- records were flushed as they were produced, which
        // is the point of the format (scan_record.h).
        ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
        return EXCEPTION_EXECUTE_HANDLER;
    });

    // abort() from a plugin's assert, and the CRT's invalid-parameter handler, are two more paths
    // to a modal box. `_set_abort_behavior` is a no-op in a release CRT but harmless there.
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)_set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
            ::TerminateProcess(::GetCurrentProcess(), 0xC000000Du);
        });
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif
}

} // namespace aip::scanner
