// Loading a VST3 module and enumerating what it offers (design_doc.md sec. 7.1).
//
// Control thread only. `LoadLibrary` and COM activation are explicitly forbidden on the audio
// thread (sec. 7.4.1), so nothing in this header may ever be reached from block processing.
//
// A module stays loaded for as long as any component created from it lives -- unloading the DLL
// out from under a live IComponent is a use-after-free -- so PluginInstance holds a shared
// reference to the module it came from and this class is handed around by shared_ptr.

#pragma once

#include "public.sdk/source/vst/hosting/module.h"

#include <memory>
#include <string>
#include <vector>

namespace aip::engine {

/// One class the module's factory exposes, narrowed to what a host cares about.
struct PluginClass {
    VST3::UID id;
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;
    std::string subCategories;
};

class PluginModule {
public:
    using Ptr = std::shared_ptr<PluginModule>;

    /// `path` is a `.vst3` bundle directory or file. Returns null and fills `error` on failure;
    /// the SDK's error text is the only useful diagnostic there, so it is passed through
    /// verbatim rather than replaced.
    [[nodiscard]] static Ptr load(const std::string& path, std::string& error);

    [[nodiscard]] const std::string& path() const noexcept { return module_->getPath(); }

    [[nodiscard]] const std::string& name() const noexcept { return module_->getName(); }

    /// Every class in the factory whose category is `kVstAudioEffectClass`. This is what can be
    /// instantiated as a processor; the rest (controllers named separately, and so on) are
    /// reached through the component, not through the factory.
    [[nodiscard]] const std::vector<PluginClass>& audioEffects() const noexcept {
        return audioEffects_;
    }

    [[nodiscard]] const VST3::Hosting::PluginFactory& factory() const noexcept {
        return module_->getFactory();
    }

    /// Handed to the factory before any instantiation, so a plugin can query IHostApplication
    /// during `createInstance`. Must stay alive for as long as the module does.
    void setHostContext(Steinberg::FUnknown* context) const noexcept;

private:
    explicit PluginModule(VST3::Hosting::Module::Ptr module);

    VST3::Hosting::Module::Ptr module_;
    std::vector<PluginClass> audioEffects_;
};

} // namespace aip::engine
