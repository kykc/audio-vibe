#include "aip/engine/plugin_module.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <algorithm>

namespace aip::engine {

PluginModule::PluginModule(VST3::Hosting::Module::Ptr module) : module_(std::move(module)) {
    for (const VST3::Hosting::ClassInfo& info : module_->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass) {
            continue;
        }
        audioEffects_.push_back(PluginClass{
            info.ID(), info.name(), info.vendor(), info.version(), info.category(), info.subCategoriesString()});
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

std::vector<std::string> PluginModule::installedModulePaths() {
    std::vector<std::string> paths = VST3::Hosting::Module::getModulePaths();
    // The SDK returns them in directory-walk order, which is neither stable nor meaningful to a
    // person. Sorted by the bundle's own name rather than by the whole path, so that plugins from
    // different install locations still interleave alphabetically.
    std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
        const auto name = [](const std::string& path) {
            const std::size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        };
        const std::string left = name(a);
        const std::string right = name(b);
        return left == right ? a < b : left < right;
    });
    return paths;
}

void PluginModule::setHostContext(Steinberg::FUnknown* context) const noexcept {
    module_->getFactory().setHostContext(context);
}

} // namespace aip::engine
