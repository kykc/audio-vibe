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

namespace {

/// Case-insensitive equality over ASCII, which is what a class name someone typed is compared
/// with. Deliberately not `_stricmp` or a locale-aware fold: a plugin name is third-party text
/// under no obligation to be ASCII at all (scan_record.h), and a fold that changed the meaning of
/// the bytes above 0x7F would make matching depend on the machine's locale.
[[nodiscard]] bool equalNoCase(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    const auto lower = [](char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::equal(a.begin(), a.end(), b.begin(), [&lower](char x, char y) { return lower(x) == lower(y); });
}

} // namespace

const PluginClass* findAudioEffect(const std::vector<PluginClass>& classes, const std::string& ref) {
    if (const VST3::Optional<VST3::UID> id = VST3::UID::fromString(ref)) {
        for (const PluginClass& info : classes) {
            if (info.id == *id) {
                return &info;
            }
        }
    }
    for (const PluginClass& info : classes) {
        if (info.name == ref) {
            return &info;
        }
    }
    for (const PluginClass& info : classes) {
        if (equalNoCase(info.name, ref)) {
            return &info;
        }
    }
    return nullptr;
}

PluginReference parsePluginReference(const std::string& text) {
    const std::size_t separator = text.find('?');
    if (separator == std::string::npos) {
        return PluginReference{text, std::string()};
    }
    return PluginReference{text.substr(0, separator), text.substr(separator + 1)};
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
