// The control-thread half of the VST3 host (design_doc.md sec. 7.4.3).
//
// Engine is where every forbidden thing happens on purpose: loading DLLs, activating components,
// allocating scratch banks, faulting in pages, destroying retired chains. None of it is reachable
// from the audio thread, which only ever sees the ChainProcessor this object owns.
//
// The division mirrors sec. 7.4.3 exactly:
//
//   Engine (control thread)     load, instantiate, prepare, build a PluginChain, publish it,
//                               destroy the chain it replaced, drain plugin callbacks
//   ChainProcessor (audio)      read one pointer, dispatch
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

    // ------------------------------------------------------------- the desired chain ---------
    // Edits here are inert until `rebuild` runs. Describing the chain and building it are
    // separate steps so that a failure to load or prepare is reported before anything reaches
    // the audio thread.

    /// Appends the module's first audio-effect class.
    [[nodiscard]] bool appendPlugin(const std::string& path, std::string& error);

    /// Appends a specific class from the module.
    [[nodiscard]] bool appendPlugin(const std::string& path, const VST3::UID& classId,
                                    std::string& error);

    void clearPlugins() noexcept { desired_.clear(); }

    [[nodiscard]] std::size_t desiredPluginCount() const noexcept { return desired_.size(); }

    // ------------------------------------------------------------------- publication ---------

    /// Instantiates and prepares a fresh chain for `format`, then publishes it and destroys the
    /// one it replaced. On failure nothing is published and the running chain keeps running --
    /// a half-built chain must never reach the audio thread.
    [[nodiscard]] bool rebuild(const StreamFormat& format, std::string& error);

    /// Unpublishes the chain and destroys it. Blocks pass through afterwards.
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

    // ---------------------------------------------------------------- plugin callbacks -------

    /// Drains what the plugins queued through IComponentHandler and applies it: a `PerformEdit`
    /// is pushed into the controller so its notion of the value follows the processor's.
    /// `maxPerPlugin` bounds the work, as sec. 7.4.2 asks even of the control thread.
    /// Returns the number of edits consumed.
    std::size_t serviceParameterEdits(std::size_t maxPerPlugin = 64);

    /// Edits dropped across every plugin in the published chain because the audio-thread ring
    /// filled up. Nonzero means `serviceParameterEdits` is not being called often enough.
    [[nodiscard]] std::uint64_t droppedParameterEdits() const;

private:
    struct PluginSpec {
        PluginModule::Ptr module;
        VST3::UID classId;
    };

    [[nodiscard]] PluginModule::Ptr moduleFor(const std::string& path, std::string& error);

    Steinberg::IPtr<Steinberg::Vst::HostApplication> host_;
    /// Keyed by path, so rebuilding a chain does not reload the same DLL over and over.
    std::map<std::string, PluginModule::Ptr> modules_;
    std::vector<PluginSpec> desired_;

    ChainProcessor processor_;
    StreamFormat builtFormat_;
    /// The packed geometry key the last `serviceFormatChange` acted on. See that function.
    std::uint64_t servicedFormatKey_ = 0;
};

} // namespace aip::engine
