#include "aip/engine/engine.h"

#include <algorithm>

namespace Vst = Steinberg::Vst;

namespace aip::engine {

Engine::Engine() : host_(Steinberg::owned(new Vst::HostApplication())) {}

Engine::~Engine() {
    teardown();
    desired_.clear();
    modules_.clear();
}

PluginModule::Ptr Engine::moduleFor(const std::string& path, std::string& error) {
    const auto cached = modules_.find(path);
    if (cached != modules_.end()) {
        return cached->second;
    }

    PluginModule::Ptr module = PluginModule::load(path, error);
    if (!module) {
        return nullptr;
    }
    // Before any instantiation: a plugin is entitled to query IHostApplication from inside
    // createInstance, and a null context there is a common cause of a plugin refusing to load.
    module->setHostContext(host_);
    modules_.emplace(path, module);
    return module;
}

bool Engine::appendPlugin(const std::string& path, std::string& error) {
    PluginModule::Ptr module = moduleFor(path, error);
    if (!module) {
        return false;
    }
    return appendPlugin(path, module->audioEffects().front().id, error);
}

bool Engine::appendPlugin(const std::string& path, const VST3::UID& classId, std::string& error) {
    error.clear();
    PluginModule::Ptr module = moduleFor(path, error);
    if (!module) {
        return false;
    }

    const auto& classes = module->audioEffects();
    const bool known = std::any_of(classes.begin(), classes.end(),
                                   [&](const PluginClass& c) { return c.id == classId; });
    if (!known) {
        error = path + " exposes no audio effect with that class id";
        return false;
    }

    desired_.push_back(PluginSpec{std::move(module), classId});
    return true;
}

bool Engine::rebuild(const StreamFormat& format, std::string& error) {
    error.clear();
    if (!format.valid() || format.channelCount > kMaxChannels) {
        error = "invalid stream format";
        return false;
    }

    std::vector<std::unique_ptr<PluginInstance>> plugins;
    plugins.reserve(desired_.size());

    for (const PluginSpec& spec : desired_) {
        std::unique_ptr<PluginInstance> instance =
            PluginInstance::create(spec.module, spec.classId, host_, error);
        if (!instance) {
            return false;
        }
        if (!instance->prepare(format, error)) {
            return false;
        }
        plugins.push_back(std::move(instance));
    }

    auto chain = std::make_unique<PluginChain>(format, std::move(plugins));
    if (!chain->runnable()) {
        error = "the assembled chain is not runnable";
        return false;
    }

    processor_.publish(std::move(chain));
    builtFormat_ = format;
    return true;
}

void Engine::teardown() {
    processor_.publish(nullptr);
    builtFormat_ = StreamFormat{};
}

bool Engine::serviceFormatChange(std::string& error) {
    error.clear();

    // Keyed on the observed geometry rather than on a mismatch count, because the first attach
    // has no chain to mismatch against and still needs a build. Remembering the key is what stops
    // a format the plugins refuse from being retried on every poll tick, forever.
    const std::uint64_t key = processor_.observedFormatKey();
    if (key == 0 || key == servicedFormatKey_) {
        return false;
    }

    const StreamFormat observed = processor_.observedFormat();
    if (processor_.current() != nullptr && observed.sampleRate == builtFormat_.sampleRate &&
        observed.channelCount == builtFormat_.channelCount &&
        observed.maxFrames <= builtFormat_.maxFrames) {
        // Only the block size moved, and it still fits. Nothing to do; remember it so the next
        // block of the same size does not walk this path again.
        servicedFormatKey_ = key;
        return false;
    }

    servicedFormatKey_ = key;

    if (observed.channelCount == 0 || observed.channelCount > kMaxChannels) {
        // The king is producing a channel count we will not size buffers for. Passing through is
        // the honest outcome.
        error = "observed an unusable block geometry";
        return false;
    }

    StreamFormat target;
    target.sampleRate = observed.sampleRate;
    target.channelCount = observed.channelCount;
    // `observed.maxFrames` is the frame count of one block, not a bound. Keep the standing
    // headroom so ordinary jitter in block size does not force another rebuild.
    target.maxFrames = std::max(kDefaultMaxFrames, observed.maxFrames);

    return rebuild(target, error);
}

std::size_t Engine::serviceParameterEdits(std::size_t maxPerPlugin) {
    PluginChain* chain = processor_.current();
    if (chain == nullptr) {
        return 0;
    }

    std::size_t consumed = 0;
    for (std::size_t i = 0; i < chain->size(); ++i) {
        PluginInstance& plugin = chain->at(i);
        Vst::IEditController* controller = plugin.controller();
        ComponentHandler* handler = plugin.handler();
        if (controller == nullptr || handler == nullptr) {
            continue;
        }
        consumed += handler->drain(maxPerPlugin, [controller](const ParameterEdit& edit) {
            // begin/end bracket a gesture and matter only to a UI that is not built yet;
            // restartComponent is deliberately not acted on until there is a policy for it.
            if (edit.kind == ParameterEdit::Kind::PerformEdit) {
                controller->setParamNormalized(edit.paramId, edit.value);
            }
        });
    }
    return consumed;
}

std::uint64_t Engine::droppedParameterEdits() const {
    PluginChain* chain = processor_.current();
    if (chain == nullptr) {
        return 0;
    }
    std::uint64_t dropped = 0;
    for (std::size_t i = 0; i < chain->size(); ++i) {
        if (const ComponentHandler* handler = chain->at(i).handler()) {
            dropped += handler->droppedEdits();
        }
    }
    return dropped;
}

} // namespace aip::engine
