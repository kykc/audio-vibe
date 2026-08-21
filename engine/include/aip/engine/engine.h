// The control-thread half of the VST3 host (design_doc.md sec. 7.4.3).
//
// Engine is where every forbidden thing happens on purpose: loading DLLs, activating components,
// allocating scratch banks, faulting in pages, destroying retired chains. None of it is reachable
// from the audio thread, which only ever sees the ChainProcessor this object owns.
//
// The division mirrors sec. 7.4.3 exactly:
//
//   Engine (control thread)     load, instantiate, prepare, publish a chain view, destroy what
//                               it replaced, drain plugin callbacks
//   ChainProcessor (audio)      read one pointer, dispatch
//
// **The rack, not the chain, owns the plugins.** Engine holds an ordered rack of instances that
// outlives any number of published chains; a PluginChain is a borrowed view over the subset of
// them that is currently processing. Every mutation below -- insert, remove, move, bypass, and a
// re-prepare for a new stream format -- keeps the instances alive and republishes a view. That is
// what makes a parameter the user set survive the next thing they do; a chain that owned its
// plugins would silently reset the whole rack every time one plugin was added to it.
//
// Engine is not thread-safe against itself. It expects one control thread, which is the same
// assumption ValetSupervisor already makes.

#pragma once

#include "aip/engine/chain_processor.h"
#include "aip/engine/component_handler.h"
#include "aip/engine/plugin_instance.h"
#include "aip/engine/plugin_module.h"
#include "aip/engine/stream_format.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aip::engine {

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Hand this to ValetSupervisor. It is the only part of the engine the audio thread touches.
    [[nodiscard]] ipc::BlockProcessor& blockProcessor() noexcept { return processor_; }

    [[nodiscard]] ChainProcessor& chainProcessor() noexcept { return processor_; }

    // ------------------------------------------------------------------------- the rack -------
    // Positions are rack positions, bypassed entries included, and stay stable across a rebuild.

    [[nodiscard]] std::size_t pluginCount() const noexcept { return rack_.size(); }

    /// Null when `index` is out of range. The instance stays valid until it is removed or the
    /// Engine is destroyed -- notably, it survives every format change.
    [[nodiscard]] PluginInstance* pluginAt(std::size_t index) const noexcept;

    [[nodiscard]] bool bypassed(std::size_t index) const noexcept;

    // ------------------------------------------------------------------ mutating the rack -----
    // Each of these takes effect immediately: the rack is edited and a fresh view published, so
    // there is no separate commit step. Sec. 7.4.3 permits these transitions to be audible; what
    // it does not permit is any of the work happening on the audio thread, and none of it does.

    /// Appends the module's first audio-effect class.
    [[nodiscard]] bool appendPlugin(const std::string& path, std::string& error);

    /// Inserts at `index` (clamped to the end). When a stream format is already known the new
    /// plugin is prepared for it here, so a failure is reported before anything is published.
    [[nodiscard]] bool insertPlugin(std::size_t index, const std::string& path,
                                    std::string& error);

    [[nodiscard]] bool insertPlugin(std::size_t index, const std::string& path,
                                    const VST3::UID& classId, std::string& error);

    /// Removes the plugin at `index` and destroys it, once the audio thread has provably let go.
    [[nodiscard]] bool removePlugin(std::size_t index);

    /// Moves the plugin at `from` so that it ends up at `to`.
    [[nodiscard]] bool movePlugin(std::size_t from, std::size_t to);

    /// A bypassed plugin is left out of the published view entirely -- not asked to bypass
    /// itself. With no latency compensation to preserve (sec. 3.7.1 makes the whole design
    /// zero-latency), skipping it is both simpler and more honest than trusting each plugin's
    /// own bypass parameter.
    [[nodiscard]] bool setBypass(std::size_t index, bool bypass);

    void clearPlugins();

    // ------------------------------------------------------------------------ publication -----

    /// Prepares every rack entry for `format` and publishes a view over the enabled ones. The
    /// instances are re-prepared in place, so their parameters survive; that is the whole reason
    /// the rack outlives the chain.
    ///
    /// On failure nothing is published and audio passes through. It does *not* leave the previous
    /// chain running: re-preparing mutates objects the audio thread could be reading, so the
    /// chain is retracted first.
    [[nodiscard]] bool rebuild(const StreamFormat& format, std::string& error);

    /// Unpublishes the chain. The rack is untouched, so a later `rebuild` brings it back with
    /// every parameter still set.
    void teardown();

    /// Builds or rebuilds when the audio thread has reported a block geometry the published
    /// chain cannot run -- including the very first one, where there is no chain at all. Call it
    /// from the control thread's polling loop; it is cheap when nothing has changed.
    ///
    /// Returns true when a rebuild happened and succeeded. Returns false either because nothing
    /// needed doing -- `error` empty -- or because the rebuild failed, in which case `error` says
    /// why and audio keeps passing through rather than stopping.
    bool serviceFormatChange(std::string& error);

    [[nodiscard]] const StreamFormat& builtFormat() const noexcept { return builtFormat_; }

    // ------------------------------------------------------------------ plugin callbacks ------

    /// Drains what the plugins queued through IComponentHandler and applies each edit to the half
    /// of the plugin that did not originate it: an edit the processor made is pushed into the
    /// controller, and an edit the editor made is queued for the processor. That second direction
    /// is what makes a knob in a plugin's own editor change the audio at all when the component
    /// and the controller are separate objects. `maxPerPlugin` bounds the work, as sec. 7.4.2
    /// asks even of the control thread. Returns the number of edits consumed.
    std::size_t serviceParameterEdits(std::size_t maxPerPlugin = 64);

    /// Edits dropped across the whole rack because an audio-thread ring filled up. Nonzero means
    /// `serviceParameterEdits` is not being called often enough.
    [[nodiscard]] std::uint64_t droppedParameterEdits() const;

    /// Values carried across to the processors through `inputParameterChanges`, rack-wide.
    [[nodiscard]] std::uint64_t deliveredParameters() const;

    /// Values that never got there because a ring was full -- in practice because no chain is
    /// published, so nothing is draining. Rack-wide.
    [[nodiscard]] std::uint64_t droppedParameters() const;

    /// Instances that could not be destroyed when they were removed, because the audio thread had
    /// not left the chain naming them within the grace period. They are freed at teardown. A
    /// nonzero count means the valet thread was wedged, not that anything leaked silently.
    [[nodiscard]] std::size_t strandedPlugins() const noexcept { return stranded_.size(); }

private:
    struct RackEntry {
        std::unique_ptr<PluginInstance> instance;
        bool bypassed = false;
    };

    [[nodiscard]] PluginModule::Ptr moduleFor(const std::string& path, std::string& error);

    /// Publishes a view over every prepared, non-bypassed rack entry. Publishes nothing when no
    /// format is known yet. Returns false only if the audio thread failed to release the chain
    /// it replaced, which is reported rather than waited on forever.
    bool publishRack();

    /// Retracts the chain so that rack instances can be mutated safely.
    bool retract();

    // Declaration order is load-bearing -- members are destroyed in reverse, and that reverse
    // order is the only safe one. See the note in ~Engine before changing any of it.
    Steinberg::IPtr<Steinberg::Vst::HostApplication> host_;
    /// Keyed by path, so adding a second instance of a plugin does not reload the same DLL.
    std::map<std::string, PluginModule::Ptr> modules_;
    std::vector<RackEntry> rack_;
    std::vector<std::unique_ptr<PluginInstance>> stranded_;

    ChainProcessor processor_;
    StreamFormat builtFormat_;
    /// The packed geometry key the last `serviceFormatChange` acted on. See that function.
    std::uint64_t servicedFormatKey_ = 0;
};

} // namespace aip::engine
