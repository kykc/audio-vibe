#include "aip/engine/engine.h"

#include <algorithm>
#include <string>

namespace Vst = Steinberg::Vst;

namespace aip::engine {

namespace {

/// The restart flags that mean "deactivate me and bring me back", which is exactly what a
/// re-prepare of the rack does. `kLatencyChanged` belongs here and not with the informational
/// flags for a reason the SDK spells out: `getLatencySamples` returns the new figure only after
/// `setActive(true)`, so a host that re-read the number without reactivating would be reporting
/// the latency of the configuration the plugin had just left.
constexpr std::int32_t kReconfigurationFlags = Vst::kReloadComponent | Vst::kIoChanged | Vst::kLatencyChanged;

/// Names the flags this host does nothing about, for a log line that admits it. Each of these
/// describes something the system does not model at all -- there are no parameter titles cached
/// anywhere, no MIDI, no note expression, no routing graph -- so the useful response is to say
/// which one arrived and leave it at that. Returns an empty string when there was none.
std::string nameUnhandledFlags(std::int32_t flags) {
    struct Named {
        std::int32_t flag;
        const char* name;
    };

    static constexpr Named kNames[] = {
        {Vst::kParamTitlesChanged, "parameter titles"},
        {Vst::kMidiCCAssignmentChanged, "MIDI CC assignment"},
        {Vst::kNoteExpressionChanged, "note expression"},
        {Vst::kIoTitlesChanged, "bus titles"},
        {Vst::kPrefetchableSupportChanged, "prefetchable support"},
        {Vst::kRoutingInfoChanged, "routing info"},
        {Vst::kKeyswitchChanged, "keyswitches"},
    };

    std::string named;
    for (const Named& entry : kNames) {
        if ((flags & entry.flag) == 0) {
            continue;
        }
        if (!named.empty()) {
            named += ", ";
        }
        named += entry.name;
    }
    return named;
}

} // namespace

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

bool Engine::bypassed(std::size_t index) const noexcept { return index < rack_.size() && rack_[index].bypassed; }

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

void Engine::warmUpInstance(PluginInstance& instance) {
    const PluginInstance::WarmUpResult result = instance.warmUp(kWarmUpBlocks);
    ++lastWarmUp_.plugins;
    lastWarmUp_.blocks += result.blocksRun;
    lastWarmUp_.blocksFailed += result.blocksFailed;
    lastWarmUp_.violations.allocations += result.violations.allocations;
    lastWarmUp_.violations.deallocations += result.violations.deallocations;
    lastWarmUp_.violations.locks += result.violations.locks;
}

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

    lastWarmUp_ = WarmUpReport{};
    for (RackEntry& entry : rack_) {
        // prepare() re-prepares in place -- same component, same parameters, new geometry. That
        // is the whole point of the rack outliving the chain.
        if (!entry.instance->prepare(format, channelMask_, error)) {
            builtFormat_ = StreamFormat{};
            return false;
        }
        // Safe here and only here: `retract()` above waited for the audio thread to leave, so
        // nothing else is inside any of these instances.
        warmUpInstance(*entry.instance);
    }

    builtFormat_ = format;
    // Any ordinary rebuild is either driven by an observed block or is a caller asserting the
    // format outright. `prepareSpeculatively` sets the flag back after calling through here.
    speculative_ = false;
    if (!publishRack()) {
        error = "the audio thread did not release the previous chain";
        return false;
    }
    return true;
}

bool Engine::prepareSpeculatively(std::uint32_t sampleRate, std::uint32_t channelCount, std::string& error) {
    error.clear();

    StreamFormat guess;
    guess.sampleRate = sampleRate;
    guess.channelCount = channelCount;
    guess.maxFrames = kDefaultMaxFrames;
    if (!guess.valid() || guess.channelCount > kMaxChannels) {
        error = "the endpoint reports no usable format to prepare for";
        return false;
    }

    // Nothing to do when this is already what is built -- including when a real block built it,
    // which must not be downgraded to a guess.
    if (builtFormat_ == guess) {
        return true;
    }

    if (!rebuild(guess, error)) {
        return false;
    }
    speculative_ = true;
    return true;
}

void Engine::teardown() {
    processor_.publish(nullptr);
    builtFormat_ = StreamFormat{};
    speculative_ = false;
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
        observed.channelCount == builtFormat_.channelCount && observed.maxFrames <= builtFormat_.maxFrames) {
        // Only the block size moved, and it still fits. Nothing to do; remember it so the next
        // block of the same size does not walk this path again.
        //
        // This is also where a guess stops being one: a block has now been observed that the
        // built chain matches, which is the only confirmation available.
        speculative_ = false;
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
    return insertPluginImpl(index, path, std::string(), nullptr, error);
}

bool Engine::insertPluginByClassId(
    std::size_t index, const std::string& path, const std::string& classId, std::string& error) {
    return insertPluginImpl(index, path, classId, nullptr, error);
}

bool Engine::appendPluginByClassId(const std::string& path, const std::string& classId, std::string& error) {
    return insertPluginByClassId(rack_.size(), path, classId, error);
}

bool Engine::insertPluginWithState(std::size_t index, const std::string& path, const std::string& classId,
    const PluginState& state, std::string& error) {
    return insertPluginImpl(index, path, classId, &state, error);
}

bool Engine::insertPlugin(std::size_t index, const std::string& path, const VST3::UID& classId, std::string& error) {
    // Through the same text form every other caller uses. The round trip is exact -- it is the
    // property `VST3::UID::toString` exists for -- and one resolution path beats two.
    return insertPluginImpl(index, path, classId.toString(), nullptr, error);
}

bool Engine::insertPluginImpl(std::size_t index, const std::string& path, const std::string& classRef,
    const PluginState* state, std::string& error) {
    error.clear();
    PluginModule::Ptr module = moduleFor(path, error);
    if (!module) {
        return false;
    }

    // Which of the module's classes are on the table. Naming one narrows this to that one and
    // makes not finding it the error; naming none puts all of them up, because a bundle is not a
    // plugin and a path on its own has not said which effect was meant (engine.h).
    const std::vector<PluginClass>& classes = module->audioEffects();
    std::vector<VST3::UID> candidates;
    if (classRef.empty()) {
        candidates.reserve(classes.size());
        for (const PluginClass& info : classes) {
            candidates.push_back(info.id);
        }
    } else {
        const PluginClass* named = findAudioEffect(classes, classRef);
        if (named == nullptr) {
            error = path + " exposes no audio effect matching " + classRef;
            return false;
        }
        candidates.push_back(named->id);
    }

    // The first refusal, kept for the report. A later one says less: it is the diagnostic of a
    // class nobody asked for, reached only because the ones before it were unusable.
    std::string firstRefusal;
    const auto refuse = [&firstRefusal](const std::string& why) {
        if (firstRefusal.empty()) {
            firstRefusal = why;
        }
    };

    for (const VST3::UID& candidate : candidates) {
        std::string attempt;
        std::unique_ptr<PluginInstance> instance = PluginInstance::create(module, candidate, host_, attempt);
        if (!instance) {
            refuse(attempt);
            continue;
        }

        // State first, prepare second. That order is the plugin's to expect, not ours to choose
        // (PluginInstance::loadState). A refusal is carried out to the caller and otherwise ignored:
        // the plugin is loaded and simply starts from its defaults.
        std::string stateWarning;
        if (state != nullptr && !state->empty() && !instance->loadState(*state)) {
            stateWarning = instance->name() + ": rejected its saved state";
        }

        // Prepared before it is inserted, and inserted before anything is published: a plugin that
        // cannot take the current format is reported without disturbing what is already running.
        //
        // It is also what decides between the classes of a multi-effect bundle. The mono half of
        // lsp-plugins.vst3 refuses a stereo stream here and the stereo half does not, and that --
        // not a guess made from names or bus counts -- is the whole of how one is chosen.
        if (builtFormat_.valid()) {
            if (!instance->prepare(builtFormat_, channelMask_, attempt)) {
                refuse(attempt);
                continue;
            }
            // No retract needed for this one: the instance is not in the rack yet, so no published
            // chain can name it and the audio thread has no way to reach it.
            lastWarmUp_ = WarmUpReport{};
            warmUpInstance(*instance);
        }

        rack_.insert(rack_.begin() + static_cast<std::ptrdiff_t>(std::min(index, rack_.size())),
            RackEntry{std::move(instance), false});
        const bool published = publishRack();
        if (published) {
            error = stateWarning;
        }
        return published;
    }

    // Only reachable with every candidate refused. One candidate is the ordinary case and its own
    // diagnostic is the whole story; several means a bundle none of whose effects would load, and
    // saying how many were tried is what separates that from one broken plugin.
    error = candidates.size() == 1
        ? firstRefusal
        : path + ": none of its " + std::to_string(candidates.size()) + " audio effects could be loaded -- " +
            firstRefusal;
    return false;
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

Engine::DrainTally Engine::drainRack(std::size_t maxPerPlugin) {
    DrainTally tally;
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
        PluginInstance* instance = entry.instance.get();
        const auto apply = [controller, instance, &tally](const ParameterEdit& edit) {
            if (edit.kind == ParameterEdit::Kind::RestartComponent) {
                ++tally.restartRequests;
                tally.flags |= edit.flags;
                if ((edit.flags & kReconfigurationFlags) != 0) {
                    ++tally.reconfigurationRequests;
                }
                return;
            }
            // begin/end bracket a gesture and matter only to a host that keeps its own automation
            // state, which this one does not yet.
            if (edit.kind != ParameterEdit::Kind::PerformEdit) {
                return;
            }
            // Each half is told what the *other* half did, and never what it did itself. Sending
            // an edit back to its author is redundant at best; for a control-origin edit it is
            // actively harmful, because pushing a value into the controller in the middle of the
            // user's own gesture is how a knob ends up fighting the mouse.
            if (edit.origin == ParameterEdit::Origin::Audio) {
                controller->setParamNormalized(edit.paramId, edit.value);
            } else {
                (void)instance->queueParameter(edit.paramId, edit.value);
            }
        };
        tally.edits += handler->drain(maxPerPlugin, apply);
    }
    return tally;
}

std::size_t Engine::serviceParameterEdits(std::size_t maxPerPlugin) {
    lastRestart_ = RestartReport{};

    const DrainTally first = drainRack(maxPerPlugin);
    std::size_t consumed = first.edits;
    if (first.restartRequests == 0) {
        return consumed;
    }

    lastRestart_.requests = first.restartRequests;
    lastRestart_.flags = first.flags;
    lastRestart_.parameterValues = (first.flags & Vst::kParamValuesChanged) != 0;
    lastRestart_.unhandled = nameUnhandledFlags(first.flags);

    if (first.reconfigurationRequests == 0) {
        return consumed;
    }
    if (!builtFormat_.valid()) {
        // Nothing is prepared, so there is nothing to deactivate and nothing the plugin can be
        // waiting for. The next `prepare` is the reconfiguration it asked for, and it will read
        // the new latency on its way out of it.
        return consumed;
    }

    // The format-change path at the format already built. `rebuild` retracts, re-prepares every
    // instance -- which is the deactivate/reactivate the plugin asked for -- warms them and
    // republishes. Rebuilding the whole rack for one plugin's request is the honest reading of
    // `kIoChanged`: a bus count that moved changes the shape of the chain, not of one link in it.
    std::string error;
    if (!rebuild(builtFormat_, error)) {
        lastRestart_.error = error;
        return consumed;
    }
    lastRestart_.reconfigured = true;

    // The second pass, and the reason it exists: a plugin that announces its new latency from
    // inside its own reactivation is ordinary rather than broken -- JUCE wrappers do it from
    // `prepareToPlay` -- and a rebuild that acts on the request its own rebuild produced never
    // stops. So the requests queued during the rebuild above are drained here and the
    // reconfiguration ones are counted and dropped. The informational flags are kept: a plugin
    // that reset its parameters while reactivating is telling the truth and something on screen
    // still needs to know.
    const DrainTally echoed = drainRack(maxPerPlugin);
    consumed += echoed.edits;
    lastRestart_.requests += echoed.restartRequests;
    lastRestart_.suppressed = echoed.reconfigurationRequests;
    lastRestart_.flags |= echoed.flags;
    lastRestart_.parameterValues = lastRestart_.parameterValues || (echoed.flags & Vst::kParamValuesChanged) != 0;
    lastRestart_.unhandled = nameUnhandledFlags(lastRestart_.flags);
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

std::uint64_t Engine::deliveredParameters() const {
    std::uint64_t delivered = 0;
    for (const RackEntry& entry : rack_) {
        if (entry.instance) {
            delivered += entry.instance->deliveredParameters();
        }
    }
    return delivered;
}

std::uint64_t Engine::droppedParameters() const {
    std::uint64_t dropped = 0;
    for (const RackEntry& entry : rack_) {
        if (entry.instance) {
            dropped += entry.instance->droppedParameters();
        }
    }
    return dropped;
}

} // namespace aip::engine
