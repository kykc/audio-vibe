#include "aip/config/session.h"

namespace aip::config {
namespace {

/// What to call an entry in a problem report. The name is informational and may be absent in a
/// hand-edited file, so fall back to the path -- never to nothing.
std::string describe(const RackEntry& entry) {
    return entry.name.empty() ? entry.path : entry.name;
}

} // namespace

void capture(const engine::Engine& engine, Session& session) {
    session.rack.clear();
    session.rack.reserve(engine.pluginCount());
    session.chainBypassed = engine.chainBypassed();

    for (std::size_t i = 0; i < engine.pluginCount(); ++i) {
        const engine::PluginInstance* instance = engine.pluginAt(i);
        if (instance == nullptr) {
            continue;
        }

        RackEntry entry;
        entry.path = instance->path();
        entry.classId = instance->classIdString();
        entry.name = instance->name();
        entry.bypassed = engine.bypassed(i);
        // A plugin with nothing to save is not a failure -- an effect with no parameters has no
        // state, and the entry is still worth writing so the plugin comes back at all.
        (void)instance->saveState(entry.state);
        session.rack.push_back(std::move(entry));
    }
}

std::size_t blockUnsafeEntries(Session& session, const std::string& casualty,
                               const std::vector<scanner::ScannedModule>& catalog,
                               std::vector<std::string>& notes) {
    std::size_t blocked = 0;

    for (RackEntry& entry : session.rack) {
        if (entry.blocked) {
            ++blocked;
            continue;
        }

        if (!casualty.empty() && entry.path == casualty) {
            entry.blocked = true;
            entry.blockedReason = "it stopped the previous start from finishing";
            notes.push_back(describe(entry) +
                            ": not loaded -- the last start did not survive it. Clear `blocked` in"
                            " the session file to try it again.");
            ++blocked;
            continue;
        }

        for (const scanner::ScannedModule& module : catalog) {
            if (module.path != entry.path || module.usable()) {
                continue;
            }
            entry.blocked = true;
            entry.blockedReason =
                std::string("the plugin scan reports it as ") + scanner::toString(module.status);
            notes.push_back(describe(entry) + ": not loaded -- the scan reports it as " +
                            scanner::toString(module.status));
            ++blocked;
            break;
        }
    }

    return blocked;
}

ReattachDecision shouldReattach(const Session& session, bool endpointPresent,
                                const UncleanAttach& lastRun) {
    ReattachDecision decision;
    if (!session.attached) {
        // Nothing to act on and nothing to explain: the user closed the shell detached, so it
        // starts detached. Any mark the last run left behind is the window's to report.
        return decision;
    }

    if (lastRun.present) {
        decision.reason = "not reattaching: the last run was attached";
        if (!lastRun.endpointName.empty()) {
            decision.reason += " to " + lastRun.endpointName;
        }
        decision.reason += " and did not shut down cleanly. A plugin that faults while processing"
                           " takes the machine's audio with it every time, so this start stays"
                           " detached -- press Attach when you are ready to try again.";
        return decision;
    }

    if (!endpointPresent) {
        decision.reason = "not reattaching: the endpoint this session was using";
        if (!session.endpointName.empty()) {
            decision.reason += " (" + session.endpointName + ")";
        }
        decision.reason += " is gone";
        return decision;
    }

    decision.attach = true;
    return decision;
}

std::size_t apply(const Session& session, engine::Engine& engine,
                  std::vector<std::string>& problems, LoadGuard* guard) {
    std::size_t restored = 0;

    for (const RackEntry& entry : session.rack) {
        if (entry.blocked) {
            problems.push_back(describe(entry) + ": not loaded -- " +
                               (entry.blockedReason.empty() ? std::string("blocked")
                                                            : entry.blockedReason));
            continue;
        }

        // Read the position back from the engine each time rather than counting: an entry that
        // fails to load does not advance it, and an index computed from the loop would then put
        // every plugin after it in the wrong place.
        const std::size_t index = engine.pluginCount();

        // The breadcrumb brackets the load and nothing else. It is dropped again whether the
        // plugin loaded or refused: a plugin that fails cleanly is not the one this is for.
        if (guard != nullptr) {
            guard->mark(entry.path);
        }
        std::string error;
        const bool inserted =
            engine.insertPluginWithState(index, entry.path, entry.classId, entry.state, error);
        if (guard != nullptr) {
            guard->clear();
        }

        if (!inserted) {
            problems.push_back(describe(entry) + ": " + error);
            continue;
        }
        // Non-empty on success means the plugin loaded but would not take its state back
        // (Engine::insertPluginWithState). The plugin is in the rack; it starts from defaults.
        if (!error.empty()) {
            problems.push_back(error);
        }

        if (entry.bypassed && !engine.setBypass(index, true)) {
            problems.push_back(describe(entry) + ": could not be bypassed");
        }
        ++restored;
    }

    // Last, and unconditionally -- including for an empty rack, which is a chain someone can
    // perfectly well have left switched out of the path. It is set after the plugins rather than
    // before only for tidiness; nothing above reads it.
    engine.setChainBypass(session.chainBypassed);

    return restored;
}

} // namespace aip::config
