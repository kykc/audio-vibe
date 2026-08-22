// The session file (status.md sec. 5): the rack, and everything else the shell remembers.
//
// Three layers, tested separately because they fail separately:
//
//   base64        the only lossy-looking step, and the one a hand-edit can break
//   the file      does a session survive being written and read back, byte for byte
//   the engine    does a *plugin* come back holding what it held, which is the actual point
//
// The third is the one worth having. `tests/fixtures/aip_test_plugin` carries real state and
// refuses a blob it does not recognise, so both the restore and the refusal are observable here
// rather than inferred from a plugin nobody controls.

#include "aip/config/attach_guard.h"
#include "aip/config/base64.h"
#include "aip/config/file_stamp.h"
#include "aip/config/load_guard.h"
#include "aip/config/session.h"
#include "aip/config/session_file.h"
#include "aip/engine/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace aip;

namespace {

/// Written by CMake; there is no way for the test to work the bundle path out at run time.
const std::string kTestPluginPath = AIP_TEST_PLUGIN_PATH;

constexpr Steinberg::Vst::ParamID kGainParam = 0;
constexpr Steinberg::Vst::ParamID kOffsetParam = 3;

/// Every byte value, so nothing in the chain can be quietly text-only: a NUL, a byte that is not
/// valid UTF-8, and a newline are all in here, and all three are things a YAML scalar or a
/// narrow-string conversion could eat.
std::vector<char> everyByte() {
    std::vector<char> data(256);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(i);
    }
    return data;
}

/// A directory of its own per test, so nothing here depends on the order tests run in or on
/// whether ctest was given -j.
class TempDir {
public:
    explicit TempDir(const char* name)
        : path_(std::filesystem::temp_directory_path() / "aip_config_test" / name) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::filesystem::path file() const { return path_ / config::kSessionFileName; }

private:
    std::filesystem::path path_;
};

} // namespace

// ------------------------------------------------------------------------------------- base64

TEST_CASE("base64 round-trips every length and every byte value", "[config]") {
    const std::vector<char> source = everyByte();

    // Every length past a multiple of three is a different amount of padding, so the loop covers
    // all three tails many times over rather than only at the end.
    for (std::size_t length = 0; length <= source.size(); ++length) {
        const std::vector<char> data(source.begin(),
                                     source.begin() + static_cast<std::ptrdiff_t>(length));
        const std::string encoded = config::base64Encode(data);
        std::vector<char> decoded;
        REQUIRE(config::base64Decode(encoded, decoded));
        REQUIRE(decoded == data);
    }
}

TEST_CASE("base64 wraps its output and reads the wrapping back", "[config]") {
    const std::vector<char> data(1000, 'x');

    const std::string wrapped = config::base64Encode(data, config::kBase64LineLength);
    REQUIRE(wrapped.find('\n') != std::string::npos);
    for (std::size_t start = 0; start < wrapped.size();) {
        const std::size_t end = wrapped.find('\n', start);
        const std::size_t length =
            (end == std::string::npos ? wrapped.size() : end) - start;
        REQUIRE(length <= config::kBase64LineLength);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::vector<char> decoded;
    REQUIRE(config::base64Decode(wrapped, decoded));
    REQUIRE(decoded == data);

    // A line length of zero means one line, which is what a caller wanting a compact form asks
    // for -- and it has to decode identically.
    const std::string unwrapped = config::base64Encode(data, 0);
    REQUIRE(unwrapped.find('\n') == std::string::npos);
    REQUIRE(config::base64Decode(unwrapped, decoded));
    REQUIRE(decoded == data);
}

TEST_CASE("base64 rejects what cannot have come from an encoder", "[config]") {
    std::vector<char> decoded;

    // A character outside the alphabet.
    REQUIRE_FALSE(config::base64Decode("AA*A", decoded));
    REQUIRE(decoded.empty());
    // A quantum that stops half way: base64 cannot express one, so accepting it would mean
    // inventing the missing bits.
    REQUIRE_FALSE(config::base64Decode("AAA", decoded));
    // Padding in the middle is two messages, not one.
    REQUIRE_FALSE(config::base64Decode("AA==AA==", decoded));
    // More padding than a quantum can carry.
    REQUIRE_FALSE(config::base64Decode("A===", decoded));

    // Whitespace anywhere is not corruption -- it is what wrapping and hand-editing produce.
    REQUIRE(config::base64Decode("  A A E C \n A w == \t", decoded));
    REQUIRE(decoded == std::vector<char>{0x00, 0x01, 0x02, 0x03});
}

// --------------------------------------------------------------------------------- the file

TEST_CASE("a session survives being written and read back", "[config]") {
    const TempDir dir("round_trip");

    config::Session written;
    written.endpointId = "{0.0.0.00000000}.{deadbeef-0000-1111-2222-333344445555}";
    written.endpointName = "Speakers (Test Device)";
    written.attached = true;
    written.window = {120, 80, 1024, 768, true};

    config::RackEntry first;
    first.path = "C:/Program Files/Common Files/VST3/Something.vst3";
    first.classId = "56535441494E4B4F4C4F52204558414D";
    first.name = "Something";
    first.bypassed = true;
    first.state.component = everyByte();
    first.state.controller = {'u', 'i'};
    written.rack.push_back(first);

    config::RackEntry second;
    second.path = "C:/Program Files/Common Files/VST3/Other.vst3";
    second.name = "Other";
    written.rack.push_back(second);

    std::string error;
    REQUIRE(config::writeSession(dir.file(), written, error));
    REQUIRE(error.empty());

    config::Session read;
    REQUIRE(config::readSession(dir.file(), read, error));
    REQUIRE(error.empty());

    REQUIRE(read.endpointId == written.endpointId);
    REQUIRE(read.endpointName == written.endpointName);
    REQUIRE(read.attached);
    REQUIRE(read.window.x == 120);
    REQUIRE(read.window.y == 80);
    REQUIRE(read.window.width == 1024);
    REQUIRE(read.window.height == 768);
    REQUIRE(read.window.maximized);

    REQUIRE(read.rack.size() == 2);
    // Order is the rack's whole meaning: a chain read back backwards is a different chain.
    REQUIRE(read.rack[0].name == "Something");
    REQUIRE(read.rack[0].path == first.path);
    REQUIRE(read.rack[0].classId == first.classId);
    REQUIRE(read.rack[0].bypassed);
    REQUIRE(read.rack[0].state.component == first.state.component);
    REQUIRE(read.rack[0].state.controller == first.state.controller);

    REQUIRE(read.rack[1].name == "Other");
    REQUIRE_FALSE(read.rack[1].bypassed);
    REQUIRE(read.rack[1].state.empty());
}

TEST_CASE("the session file is text a person can read", "[config]") {
    const TempDir dir("readable");

    config::Session session;
    config::RackEntry entry;
    entry.path = "C:/plugins/Readable.vst3";
    entry.name = "Readable";
    entry.state.component = std::vector<char>(400, '\x01');
    session.rack.push_back(entry);

    std::string error;
    REQUIRE(config::writeSession(dir.file(), session, error));

    std::ifstream file(dir.file(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    // The point of YAML over a binary blob: the path and the name are legible, and the one part
    // that cannot be is wrapped instead of being a single enormous line.
    REQUIRE(text.find("C:/plugins/Readable.vst3") != std::string::npos);
    REQUIRE(text.find("Readable") != std::string::npos);
    for (std::size_t start = 0, end = 0; end != std::string::npos; start = end + 1) {
        end = text.find('\n', start);
        const std::size_t length = (end == std::string::npos ? text.size() : end) - start;
        REQUIRE(length <= 120);
    }
}

TEST_CASE("a session that says nothing about attaching does not ask to attach", "[config]") {
    const TempDir dir("no_attach_key");
    {
        std::ofstream file(dir.file(), std::ios::binary);
        file << "version: 1\nendpoint:\n  id: \"{some-endpoint}\"\n";
    }

    config::Session session;
    std::string error;
    REQUIRE(config::readSession(dir.file(), session, error));
    REQUIRE(session.endpointId == "{some-endpoint}");
    // The default has to be false, and not only for tidiness: a file written before the key
    // existed must not make the shell take over the machine's audio on the next start.
    REQUIRE_FALSE(session.attached);
}

TEST_CASE("an empty session file is portable mode, not a failure", "[config]") {
    const TempDir dir("empty");
    { std::ofstream file(dir.file(), std::ios::binary); }

    config::Session session;
    std::string error;
    REQUIRE(config::readSession(dir.file(), session, error));
    REQUIRE(error.empty());
    REQUIRE(session.rack.empty());
}

TEST_CASE("a session from a future version is refused rather than half-read", "[config]") {
    const TempDir dir("future");
    {
        std::ofstream file(dir.file(), std::ios::binary);
        file << "version: 99\nrack:\n  - path: C:/plugins/Future.vst3\n";
    }

    config::Session session;
    std::string error;
    REQUIRE_FALSE(config::readSession(dir.file(), session, error));
    REQUIRE(error.find("99") != std::string::npos);
    REQUIRE(session.rack.empty());
}

TEST_CASE("a malformed session file is reported, not guessed at", "[config]") {
    const TempDir dir("malformed");
    {
        std::ofstream file(dir.file(), std::ios::binary);
        file << "version: 1\nrack:\n  - path: [unterminated\n";
    }

    config::Session session;
    std::string error;
    REQUIRE_FALSE(config::readSession(dir.file(), session, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("a missing session file is an error, an absent one is not", "[config]") {
    const TempDir dir("missing");

    config::Session session;
    std::string error;
    REQUIRE_FALSE(config::readSession(dir.file(), session, error));
    REQUIRE_FALSE(error.empty());
}

// ------------------------------------------------------------------------------- the catalog

TEST_CASE("the scan report survives the file", "[config]") {
    const TempDir dir("catalog");

    config::Session written;
    config::CatalogEntry ok;
    ok.module.path = "C:/plugins/Good.vst3";
    ok.module.name = "Good";
    ok.module.status = scanner::ScanStatus::Ok;
    ok.stamp = config::FileStamp{4096, 1234567890};
    scanner::ScannedClass info;
    info.id = "56535441494E4B4F4C4F52204558414D";
    info.name = "Good EQ";
    info.vendor = "Somebody";
    info.version = "1.3.1";
    info.subCategories = "Fx|EQ";
    info.singleComponent = true;
    info.hasEditor = true;
    info.parameterCount = 610;
    info.mainInputChannels = 2;
    info.mainOutputChannels = 2;
    info.prepared = true;
    ok.module.classes.push_back(info);
    written.catalog.push_back(ok);

    // A module that hung. Keeping the failures is the point of the cache as much as keeping the
    // successes: re-probing a plugin that hangs costs the 60-second deadline every time.
    config::CatalogEntry hung;
    hung.module.path = "C:/plugins/Hangs.vst3";
    hung.module.name = "Hangs";
    hung.module.status = scanner::ScanStatus::TimedOut;
    hung.module.error = "no progress for 60000 ms";
    hung.stamp = config::FileStamp{99, 5};
    written.catalog.push_back(hung);

    std::string error;
    REQUIRE(config::writeSession(dir.file(), written, error));

    config::Session read;
    REQUIRE(config::readSession(dir.file(), read, error));
    REQUIRE(read.catalog.size() == 2);

    REQUIRE(read.catalog[0].module.path == ok.module.path);
    REQUIRE(read.catalog[0].module.status == scanner::ScanStatus::Ok);
    REQUIRE(read.catalog[0].stamp == ok.stamp);
    REQUIRE(read.catalog[0].module.classes.size() == 1);
    REQUIRE(read.catalog[0].module.classes[0].id == info.id);
    REQUIRE(read.catalog[0].module.classes[0].name == info.name);
    REQUIRE(read.catalog[0].module.classes[0].vendor == info.vendor);
    REQUIRE(read.catalog[0].module.classes[0].parameterCount == 610);
    REQUIRE(read.catalog[0].module.classes[0].singleComponent);
    REQUIRE(read.catalog[0].module.classes[0].hasEditor);
    REQUIRE(read.catalog[0].module.classes[0].prepared);
    REQUIRE_FALSE(read.catalog[0].module.classes[0].noController);

    REQUIRE(read.catalog[1].module.status == scanner::ScanStatus::TimedOut);
    REQUIRE(read.catalog[1].module.error == hung.module.error);
}

TEST_CASE("a cached entry with no stamp is dropped rather than trusted", "[config]") {
    const TempDir dir("unstamped");
    {
        std::ofstream file(dir.file(), std::ios::binary);
        file << "version: 1\ncatalog:\n";
        file << "  - path: C:/plugins/Unstamped.vst3\n    status: ok\n";
    }

    config::Session session;
    std::string error;
    REQUIRE(config::readSession(dir.file(), session, error));
    // An entry that cannot be checked against the file system would be believed forever, which is
    // the one failure a cache must not have.
    REQUIRE(session.catalog.empty());
}

TEST_CASE("a bundle stamps the same twice and differently after a change", "[config]") {
    const config::FileStamp original = config::stampFor(kTestPluginPath);
    REQUIRE(original.valid());
    // Stable: a stamp that varied between calls would re-probe every plugin at every start.
    REQUIRE(config::stampFor(kTestPluginPath) == original);

    const TempDir dir("stamp");
    const std::filesystem::path copy =
        dir.file().parent_path() / "aip_test_plugin.vst3";
    std::error_code ec;
    std::filesystem::copy(std::filesystem::path(kTestPluginPath), copy,
                          std::filesystem::copy_options::recursive, ec);
    REQUIRE_FALSE(ec);

    const config::FileStamp before = config::stampFor(copy.string());
    REQUIRE(before.valid());

    // One byte more in one file inside the bundle -- the smallest thing an update could be.
    std::filesystem::path victim;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(copy)) {
        if (entry.is_regular_file()) {
            victim = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(victim.empty());
    {
        std::ofstream file(victim, std::ios::binary | std::ios::app);
        file.put('x');
    }

    REQUIRE_FALSE(config::stampFor(copy.string()) == before);
}

TEST_CASE("a path that is not there does not stamp", "[config]") {
    const config::FileStamp stamp = config::stampFor("C:/no/such/bundle.vst3");
    REQUIRE_FALSE(stamp.valid());
    // And an invalid stamp never compares equal, not even to itself -- an entry that could not be
    // verified must always be re-probed.
    REQUIRE_FALSE(stamp == stamp);
}

// ----------------------------------------------------------------------------- where it lives

TEST_CASE("saving goes back to the file that was loaded, and to AppData otherwise", "[config]") {
    const config::SessionPaths paths = config::sessionPaths();
    REQUIRE_FALSE(paths.portable.empty());
    REQUIRE_FALSE(paths.appData.empty());
    REQUIRE(paths.portable.filename() == config::kSessionFileName);
    REQUIRE(paths.appData.filename() == config::kSessionFileName);

    // A clean install has nothing to go back to, and the default is AppData.
    REQUIRE(config::resolveSavePath({}) == paths.appData);
    // Anything that was loaded is written back where it came from -- including the portable copy,
    // which is the whole of what portable mode is.
    REQUIRE(config::resolveSavePath(paths.portable) == paths.portable);
    REQUIRE(config::resolveSavePath(paths.appData) == paths.appData);
}

TEST_CASE("a config next to the executable wins over the one in AppData", "[config]") {
    const config::SessionPaths paths = config::sessionPaths();
    std::error_code ec;
    if (std::filesystem::exists(paths.portable, ec)) {
        // Someone is running this build in portable mode. Overwriting their config to test that
        // it takes precedence would be a poor trade.
        SKIP("a portable config already exists next to the test executable");
    }

    {
        std::ofstream file(paths.portable, std::ios::binary);
        REQUIRE(file.good());
        file << "version: 1\n";
    }
    const std::filesystem::path resolved = config::resolveLoadPath();
    std::filesystem::remove(paths.portable, ec);

    REQUIRE(resolved == paths.portable);
}

// -------------------------------------------------------------------------------- the engine

TEST_CASE("a plugin comes back holding what it held", "[config][engine]") {
    const TempDir dir("engine_round_trip");
    std::string error;

    config::Session session;
    {
        engine::Engine engine;
        REQUIRE(engine.appendPlugin(kTestPluginPath, error));
        REQUIRE(engine.appendPlugin(kTestPluginPath, error));

        // Values that are not the defaults and are not each other's, so a restore that mixed the
        // two entries up would be visible rather than plausible.
        engine.pluginAt(0)->controller()->setParamNormalized(kGainParam, 0.75);
        engine.pluginAt(1)->controller()->setParamNormalized(kOffsetParam, 0.25);
        REQUIRE(engine.setBypass(1, true));

        config::capture(engine, session);
    }

    REQUIRE(session.rack.size() == 2);
    REQUIRE_FALSE(session.rack[0].state.component.empty());
    REQUIRE(session.rack[0].name == "AIP Test Plugin");
    REQUIRE_FALSE(session.rack[0].classId.empty());
    REQUIRE(session.rack[1].bypassed);

    // Through the file as well as through memory: a state blob that survives capture but not
    // base64 and YAML has not survived anything a user would notice.
    REQUIRE(config::writeSession(dir.file(), session, error));
    config::Session reloaded;
    REQUIRE(config::readSession(dir.file(), reloaded, error));

    engine::Engine restored;
    std::vector<std::string> problems;
    REQUIRE(config::apply(reloaded, restored, problems) == 2);
    REQUIRE(problems.empty());
    REQUIRE(restored.pluginCount() == 2);

    REQUIRE(restored.pluginAt(0)->controller()->getParamNormalized(kGainParam) == 0.75);
    REQUIRE(restored.pluginAt(1)->controller()->getParamNormalized(kOffsetParam) == 0.25);
    // The second one's gain was never touched, so it must still be the default -- otherwise this
    // test would pass just as well if every parameter were being set to the same value.
    REQUIRE(restored.pluginAt(1)->controller()->getParamNormalized(kGainParam) == 0.5);
    REQUIRE(restored.bypassed(1));
    REQUIRE_FALSE(restored.bypassed(0));
}

TEST_CASE("a plugin that refuses its state is still loaded, and says so", "[config][engine]") {
    config::Session session;
    config::RackEntry entry;
    entry.path = kTestPluginPath;
    entry.name = "AIP Test Plugin";
    // Not this plugin's state. The fixture checks a magic number before it trusts a blob, which
    // is what a real plugin does after its format changes between versions.
    entry.state.component = {'n', 'o', 't', ' ', 'm', 'i', 'n', 'e', '!', '!', '!', '!'};
    session.rack.push_back(entry);

    engine::Engine engine;
    std::vector<std::string> problems;
    REQUIRE(config::apply(session, engine, problems) == 1);

    // Loaded -- the rack is the thing the user set up, and one unreadable blob is not a reason to
    // throw it away -- but not silently.
    REQUIRE(engine.pluginCount() == 1);
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].find("rejected its saved state") != std::string::npos);
    REQUIRE(engine.pluginAt(0)->controller()->getParamNormalized(kGainParam) == 0.5);
}

TEST_CASE("a session outlives a plugin that has been uninstalled", "[config][engine]") {
    config::Session session;

    config::RackEntry missing;
    missing.path = "C:/plugins/NoSuchPlugin.vst3";
    missing.name = "No Such Plugin";
    session.rack.push_back(missing);

    config::RackEntry present;
    present.path = kTestPluginPath;
    present.name = "AIP Test Plugin";
    session.rack.push_back(present);

    engine::Engine engine;
    std::vector<std::string> problems;
    // The interesting half: the entry after the failed one is still built, and it is built at
    // position 0 rather than leaving a hole where the missing plugin was.
    REQUIRE(config::apply(session, engine, problems) == 1);
    REQUIRE(engine.pluginCount() == 1);
    REQUIRE(engine.pluginAt(0)->name() == "AIP Test Plugin");
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].find("No Such Plugin") != std::string::npos);
}

// -------------------------------------------------------------- surviving a hostile plugin

TEST_CASE("the breadcrumb names what was being loaded, and only until it returns", "[config]") {
    const TempDir dir("breadcrumb");

    // Nothing was in flight, so nothing is blamed.
    REQUIRE(config::LoadGuard::takePreviousCasualty(dir.file()).empty());

    {
        config::LoadGuard guard(dir.file());
        guard.mark("C:/plugins/Dangerous.vst3");
        // Read from disk, not from the object: this is what the *next process* would see, and the
        // whole mechanism rests on the breadcrumb being on the file system before the load runs
        // rather than in a stream buffer belonging to a process that is about to stop existing.
        REQUIRE(config::LoadGuard::takePreviousCasualty(dir.file()) ==
                "C:/plugins/Dangerous.vst3");
        // And taking it consumed it. Otherwise the entry it names could never be tried again:
        // clearing `blocked` by hand would be undone by the same file at the next start.
        REQUIRE(config::LoadGuard::takePreviousCasualty(dir.file()).empty());
    }

    // A guard destroyed with a mark outstanding clears it: a clean shutdown mid-load is not a
    // crash, and blaming a plugin for it would block one that never misbehaved.
    {
        config::LoadGuard guard(dir.file());
        guard.mark("C:/plugins/Innocent.vst3");
    }
    REQUIRE(config::LoadGuard::takePreviousCasualty(dir.file()).empty());
}

TEST_CASE("what killed the last start is blocked, and nothing else is", "[config]") {
    config::Session session;
    config::RackEntry first;
    first.path = "C:/plugins/Fine.vst3";
    first.name = "Fine";
    session.rack.push_back(first);

    config::RackEntry killer;
    killer.path = "C:/plugins/Killer.vst3";
    killer.name = "Killer";
    session.rack.push_back(killer);

    std::vector<std::string> notes;
    REQUIRE(config::blockUnsafeEntries(session, "C:/plugins/Killer.vst3", {}, notes) == 1);

    REQUIRE_FALSE(session.rack[0].blocked);
    REQUIRE(session.rack[1].blocked);
    REQUIRE_FALSE(session.rack[1].blockedReason.empty());
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].find("Killer") != std::string::npos);
}

TEST_CASE("a module the scan reports as broken is never loaded", "[config]") {
    config::Session session;
    for (const char* path : {"C:/plugins/Crashes.vst3", "C:/plugins/Hangs.vst3",
                             "C:/plugins/Works.vst3"}) {
        config::RackEntry entry;
        entry.path = path;
        entry.name = path;
        session.rack.push_back(entry);
    }

    std::vector<scanner::ScannedModule> catalog;
    scanner::ScannedModule crashes;
    crashes.path = "C:/plugins/Crashes.vst3";
    crashes.status = scanner::ScanStatus::Crashed;
    catalog.push_back(crashes);
    scanner::ScannedModule hangs;
    hangs.path = "C:/plugins/Hangs.vst3";
    hangs.status = scanner::ScanStatus::TimedOut;
    catalog.push_back(hangs);
    scanner::ScannedModule works;
    works.path = "C:/plugins/Works.vst3";
    works.status = scanner::ScanStatus::Ok;
    catalog.push_back(works);

    std::vector<std::string> notes;
    // No breadcrumb: this is knowledge the scanner paid for in a child process, and it is worth
    // acting on before anything has had a chance to take the shell down.
    REQUIRE(config::blockUnsafeEntries(session, {}, catalog, notes) == 2);
    REQUIRE(session.rack[0].blocked);
    REQUIRE(session.rack[1].blocked);
    REQUIRE_FALSE(session.rack[2].blocked);
    REQUIRE(session.rack[0].blockedReason.find("crashed") != std::string::npos);
    REQUIRE(session.rack[1].blockedReason.find("timed-out") != std::string::npos);
}

TEST_CASE("a blocked entry is skipped, reported, and kept in the file", "[config][engine]") {
    const TempDir dir("blocked");

    config::Session session;
    config::RackEntry blocked;
    blocked.path = kTestPluginPath;
    blocked.name = "AIP Test Plugin";
    blocked.blocked = true;
    blocked.blockedReason = "it stopped the previous start from finishing";
    session.rack.push_back(blocked);

    config::RackEntry loadable;
    loadable.path = kTestPluginPath;
    loadable.name = "AIP Test Plugin";
    session.rack.push_back(loadable);

    engine::Engine engine;
    std::vector<std::string> problems;
    // The entry after the blocked one is still built, and the blocked one is not touched at all --
    // which is the point: touching it is what crashes.
    REQUIRE(config::apply(session, engine, problems) == 1);
    REQUIRE(engine.pluginCount() == 1);
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].find("previous start") != std::string::npos);

    // And it survives the file, with its reason, so the user can read why and clear it.
    std::string error;
    REQUIRE(config::writeSession(dir.file(), session, error));
    config::Session read;
    REQUIRE(config::readSession(dir.file(), read, error));
    REQUIRE(read.rack.size() == 2);
    REQUIRE(read.rack[0].blocked);
    REQUIRE(read.rack[0].blockedReason == blocked.blockedReason);
    REQUIRE_FALSE(read.rack[1].blocked);

    std::ifstream file(dir.file(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    // Written once, for the entry that is blocked. A `blocked: false` on every other entry would
    // bury the one line a user is looking for.
    REQUIRE(text.find("blocked: true") != std::string::npos);
    REQUIRE(text.find("blocked: false") == std::string::npos);
}

// ------------------------------------------------------- surviving a plugin that faults later

TEST_CASE("the attach mark outlives the process that wrote it, and only that", "[config]") {
    const TempDir dir("attach_mark");

    // Nothing was attached, so nothing is suspected.
    REQUIRE_FALSE(config::AttachGuard::takePrevious(dir.file()).present);

    {
        config::AttachGuard guard(dir.file());
        guard.mark("Speakers (Realtek)");
        // Read off the file system rather than out of the object: this is what the *next process*
        // sees, and the whole mechanism rests on the mark being there when this one is not.
        const config::UncleanAttach seen = config::AttachGuard::takePrevious(dir.file());
        REQUIRE(seen.present);
        REQUIRE(seen.endpointName == "Speakers (Realtek)");
        // Taking it consumed it. A mark that survived being acted on would mean a shell that
        // never attached on its own again, with nothing to say why.
        REQUIRE_FALSE(config::AttachGuard::takePrevious(dir.file()).present);
    }

    // Detaching is an end, and so is destruction: both mean the shell was not processing audio
    // when it stopped, which is the only thing the next start is asking about.
    {
        config::AttachGuard guard(dir.file());
        guard.mark("Speakers (Realtek)");
        guard.clear();
        REQUIRE_FALSE(config::AttachGuard::takePrevious(dir.file()).present);
        guard.mark("Speakers (Realtek)");
    }
    REQUIRE_FALSE(config::AttachGuard::takePrevious(dir.file()).present);
}

TEST_CASE("a run that vanished while attached is not reattached", "[config]") {
    config::Session session;
    session.attached = true;
    session.endpointId = "{0.0.0.00000000}.{guid}";
    session.endpointName = "Speakers (Realtek)";

    config::UncleanAttach clean;
    config::UncleanAttach unclean;
    unclean.present = true;
    unclean.endpointName = "Speakers (Realtek)";

    SECTION("an ordinary previous run attaches again, and says nothing about it") {
        const config::ReattachDecision decision = config::shouldReattach(session, true, clean);
        REQUIRE(decision.attach);
        REQUIRE(decision.reason.empty());
    }

    SECTION("a previous run that stopped existing does not, and says why") {
        const config::ReattachDecision decision = config::shouldReattach(session, true, unclean);
        REQUIRE_FALSE(decision.attach);
        REQUIRE(decision.reason.find("did not shut down cleanly") != std::string::npos);
        REQUIRE(decision.reason.find("Speakers (Realtek)") != std::string::npos);
    }

    SECTION("an endpoint that has gone does not, and says something else") {
        const config::ReattachDecision decision = config::shouldReattach(session, false, clean);
        REQUIRE_FALSE(decision.attach);
        REQUIRE(decision.reason.find("is gone") != std::string::npos);
    }

    SECTION("a session that was detached is left alone, with nothing to report") {
        session.attached = false;
        const config::ReattachDecision decision = config::shouldReattach(session, true, unclean);
        REQUIRE_FALSE(decision.attach);
        REQUIRE(decision.reason.empty());
    }
}
