#include "aip/engine/engine.h"

#include <algorithm>

namespace Vst = Steinberg::Vst;

namespace aip::engine {

Engine::Engine() : host_(Steinberg::owned(new Vst::HostApplication())) {}

Engine::~Engine() {
    // Only the chain is retracted here. Everything else is left to member destruction, which
    // already unwinds in the one order that is safe -- and which clearing by hand would get
    // wrong, because the members are destroyed after this body runs:
    //
    //   processor_  first, so no chain (published or parked) still names an instance
    //   stranded_, rack_  the instances themselves
    //   modules_    last but one: unloading a DLL under a live component is a use-after-free
    //   host_       last, because the instances were initialised against it
    //
    // The declaration order in engine.h encodes that. Do not reorder it.
    teardown();
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

PluginInstance* Engine::pluginAt(std::size_t index) const noexcept {
    return index < rack_.size() ? rack_[index].instance.get() : nullptr;
}

bool Engine::bypassed(std::size_t index) const noexcept {
    return index < rack_.size() && rack_[index].bypassed;
}

// --------------------------------------------------------------------------------- publication

bool Engine::publishRack() {
    if (!builtFormat_.valid()) {
        return processor_.publish(nullptr);
    }

    std::vector<PluginInstance*> view;
    view.reserve(rack_.size());
    for (const RackEntry& entry : rack_) {
        if (!entry.bypassed && entry.instance && entry.instance->prepared()) {
            view.push_back(entry.instance.get());
        }
    }

    auto chain = std::make_unique<PluginChain>(builtFormat_, std::move(view));
    if (!chain->runnable()) {
        // Only reachable if a rack entry disagrees with builtFormat_, which would be a bug here
        // rather than a plugin's fault. Publish nothing rather than something inconsistent.
        return processor_.publish(nullptr);
    }
    return processor_.publish(std::move(chain));
}

bool Engine::retract() { return processor_.publish(nullptr); }

bool Engine::rebuild(const StreamFormat& format, std::string& error) {
    error.clear();
    if (!format.valid() || format.channelCount > kMaxChannels) {
        error = "invalid stream format";
        return false;
    }

    // Re-preparing mutates objects the audio thread may be inside, so the chain comes down first
    // and stays down until every instance is ready again (sec. 7.4.3).
    if (!retract()) {
        error = "the audio thread did not release the running chain";
        return false;
    }

    for (RackEntry& entry : rack_) {
        // prepare() re-prepares in place -- same component, same parameters, new geometry. That
        // is the whole point of the rack outliving the chain.
        if (!entry.instance->prepare(format, error)) {
            builtFormat_ = StreamFormat{};
            return false;
        }
    }

    builtFormat_ = format;
    if (!publishRack()) {
        error = "the audio thread did not release the previous chain";
        return false;
    }
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

// ------------------------------------------------------------------------------ rack mutation

bool Engine::appendPlugin(const std::string& path, std::string& error) {
    return insertPlugin(rack_.size(), path, error);
}

bool Engine::insertPlugin(std::size_t index, const std::string& path, std::string& error) {
    PluginModule::Ptr module = moduleFor(path, error);
    if (!module) {
        return false;
    }
    return insertPlugin(index, path, module->audioEffects().front().id, error);
}

bool Engine::insertPlugin(std::size_t index, const std::string& path, const VST3::UID& classId,
                          std::string& error) {
    error.clear();
    PluginModule::Ptr module = moduleFor(path, error);
    if (!module) {
        return false;
    }

    const auto& classes = module->audioEffects();
    if (std::none_of(classes.begin(), classes.end(),
                     [&](const PluginClass& c) { return c.id == classId; })) {
        error = path + " exposes no audio effect with that class id";
        return false;
    }

    std::unique_ptr<PluginInstance> instance =
        PluginInstance::create(module, classId, host_, error);
    if (!instance) {
        return false;
    }

    // Prepared before it is inserted, and inserted before anything is published: a plugin that
    // cannot take the current format is reported without disturbing what is already running.
    if (builtFormat_.valid() && !instance->prepare(builtFormat_, error)) {
        return false;
    }

    rack_.insert(rack_.begin() + static_cast<std::ptrdiff_t>(std::min(index, rack_.size())),
                 RackEntry{std::move(instance), false});
    return publishRack();
}

bool Engine::removePlugin(std::size_t index) {
    if (index >= rack_.size()) {
        return false;
    }

    std::unique_ptr<PluginInstance> removed = std::move(rack_[index].instance);
    rack_.erase(rack_.begin() + static_cast<std::ptrdiff_t>(index));

    // publishRack() waits for the audio thread to leave the chain that still named this instance,
    // so by the time it returns true the instance is provably unreferenced.
    if (publishRack()) {
        removed.reset();
        return true;
    }

    // It did not leave in time. Destroying now would be a use-after-free on the audio thread;
    // holding the instance costs memory until teardown and is reported by strandedPlugins().
    stranded_.push_back(std::move(removed));
    return true;
}

bool Engine::movePlugin(std::size_t from, std::size_t to) {
    if (from >= rack_.size() || to >= rack_.size()) {
        return false;
    }
    if (from == to) {
        return true;
    }

    RackEntry moved = std::move(rack_[from]);
    rack_.erase(rack_.begin() + static_cast<std::ptrdiff_t>(from));
    rack_.insert(rack_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));

    // No instance was touched -- only the order they are named in -- so there is nothing to wait
    // for beyond the ordinary chain swap.
    return publishRack();
}

bool Engine::setBypass(std::size_t index, bool bypass) {
    if (index >= rack_.size()) {
        return false;
    }
    if (rack_[index].bypassed == bypass) {
        return true;
    }
    rack_[index].bypassed = bypass;
    return publishRack();
}

void Engine::clearPlugins() {
    processor_.publish(nullptr);
    // The chain is down and publish() waited for the audio thread to leave it, so the instances
    // are unreferenced and can go.
    rack_.clear();
}

// --------------------------------------------------------------------------- plugin callbacks

std::size_t Engine::serviceParameterEdits(std::size_t maxPerPlugin) {
    std::size_t consumed = 0;
    // The whole rack, not just what is published: a bypassed plugin's editor is still live, and
    // its queue would otherwise fill up and start dropping.
    for (const RackEntry& entry : rack_) {
        if (!entry.instance) {
            continue;
        }
        Vst::IEditController* controller = entry.instance->controller();
        ComponentHandler* handler = entry.instance->handler();
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
    std::uint64_t dropped = 0;
    for (const RackEntry& entry : rack_) {
        if (entry.instance) {
            if (const ComponentHandler* handler = entry.instance->handler()) {
                dropped += handler->droppedEdits();
            }
        }
    }
    return dropped;
}

} // namespace aip::engine
