#include "aip/engine/plugin_module.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"

namespace aip::engine {

PluginModule::PluginModule(VST3::Hosting::Module::Ptr module) : module_(std::move(module)) {
    for (const VST3::Hosting::ClassInfo& info : module_->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass) {
            continue;
        }
        audioEffects_.push_back(PluginClass{info.ID(), info.name(), info.vendor(), info.version(),
                                            info.category(), info.subCategoriesString()});
    }
}

PluginModule::Ptr PluginModule::load(const std::string& path, std::string& error) {
    error.clear();
    VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(path, error);
    if (!module) {
        if (error.empty()) {
            error = "failed to load " + path;
        }
        return nullptr;
    }

    // Not make_shared: the constructor is private and this is a one-off, so the extra control
    // block allocation is irrelevant next to loading a DLL.
    Ptr wrapper(new PluginModule(std::move(module)));
    if (wrapper->audioEffects_.empty()) {
        error = path + " exposes no " + kVstAudioEffectClass + " class";
        return nullptr;
    }
    return wrapper;
}

void PluginModule::setHostContext(Steinberg::FUnknown* context) const noexcept {
    module_->getFactory().setHostContext(context);
}

} // namespace aip::engine
