// Scanner tests (design_doc.md sec. 7.2).
//
// The two that matter are the ones pointed at `aip_crash_plugin` and `aip_hang_plugin`: they are
// the difference between claiming crash isolation and having it. Everything else here is the
// wire format, which is only interesting because those two depend on it surviving a writer that
// stops mid-sentence.
//
// These tests start real processes and load real DLLs, so they are slower than the rest of the
// suite by a wide margin. They are still not tagged out of the default run -- a test that does
// not run proves nothing, which this project has now learned twice (status.md sec. 8 items 4
// and 19).

#include "aip/scanner/scan_record.h"
#include "aip/scanner/scanner.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace aip;

namespace {

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

[[nodiscard]] scanner::ScannedModule feed(const std::string& encoded, bool& completed) {
    scanner::RecordReader reader;
    completed = false;
    scanner::ScannedModule out;
    for (const std::string& line : splitLines(encoded)) {
        if (reader.consumeLine(line)) {
            completed = true;
            out = reader.release();
        }
    }
    return out;
}

} // namespace

TEST_CASE("a record field survives every byte a plugin might put in a name", "[scanner]") {
    // Plugin names, vendors and paths are third-party text. None of it is obliged to be ASCII, and
    // a newline or a stray backslash in one would end or corrupt the record carrying it.
    std::string raw = "Plug \\ In";
    raw += '\n';
    raw += '\r';
    raw += '\t';
    raw += static_cast<char>(0x80);
    raw += static_cast<char>(0xFF);
    raw += '\0';
    raw += "tail";

    const std::string escaped = scanner::escapeField(raw);

    for (const char ch : escaped) {
        const auto byte = static_cast<unsigned char>(ch);
        CAPTURE(escaped);
        REQUIRE(byte >= 0x20);
        REQUIRE(byte <= 0x7E);
    }
    REQUIRE(scanner::unescapeField(escaped) == raw);
}

TEST_CASE("an encoded module round-trips through the reader", "[scanner]") {
    scanner::ScannedModule original;
    original.path = "C:/Program Files/Common Files/VST3/Something.vst3";
    original.name = "Something.vst3";
    original.status = scanner::ScanStatus::Ok;

    scanner::ScannedClass first;
    first.id = "ABCDEF019182FAEB5A6C697545717532";
    first.name = "Something";
    first.vendor = "A Vendor";
    first.version = "1.3.1";
    first.subCategories = "Fx|EQ";
    first.singleComponent = false;
    first.hasEditor = true;
    first.parameterCount = 610;
    first.latencySamples = 128;
    first.audioInputBusses = 2;
    first.audioOutputBusses = 1;
    first.mainInputChannels = 2;
    first.mainOutputChannels = 2;
    first.prepared = true;
    first.fullBusNegotiation = true;

    scanner::ScannedClass second;
    second.id = "00000000000000000000000000000001";
    second.name = "Something Else";
    second.noController = true;
    second.error = "refused the probe format";

    original.classes = {first, second};

    bool completed = false;
    const scanner::ScannedModule decoded = feed(scanner::encodeModule(original), completed);

    REQUIRE(completed);
    REQUIRE(decoded.path == original.path);
    REQUIRE(decoded.name == original.name);
    REQUIRE(decoded.status == scanner::ScanStatus::Ok);
    REQUIRE(decoded.classes.size() == 2);

    REQUIRE(decoded.classes[0].id == first.id);
    REQUIRE(decoded.classes[0].name == first.name);
    REQUIRE(decoded.classes[0].vendor == first.vendor);
    REQUIRE(decoded.classes[0].subCategories == first.subCategories);
    REQUIRE(decoded.classes[0].parameterCount == 610);
    REQUIRE(decoded.classes[0].latencySamples == 128u);
    REQUIRE(decoded.classes[0].hasEditor);
    REQUIRE_FALSE(decoded.classes[0].singleComponent);
    REQUIRE(decoded.classes[0].prepared);
    REQUIRE(decoded.classes[0].fullBusNegotiation);

    REQUIRE(decoded.classes[1].name == second.name);
    REQUIRE(decoded.classes[1].noController);
    REQUIRE(decoded.classes[1].error == second.error);
}

TEST_CASE("a truncated record stream names the module that was in flight", "[scanner]") {
    // Exactly what a parent is left holding when its child faults: the announcement, some of the
    // findings, and no terminator.
    scanner::RecordReader reader;
    REQUIRE_FALSE(reader.consumeLine("begin C:/plugins/Doomed.vst3"));
    REQUIRE_FALSE(reader.consumeLine("module.name Doomed.vst3"));
    REQUIRE_FALSE(reader.consumeLine("class.begin 0123"));
    REQUIRE_FALSE(reader.consumeLine("class.name Doomed"));

    REQUIRE(reader.inFlight());
    REQUIRE(reader.inFlightPath() == "C:/plugins/Doomed.vst3");

    const scanner::ScannedModule abandoned = reader.abandon(scanner::ScanStatus::Crashed, "it died");

    REQUIRE(abandoned.path == "C:/plugins/Doomed.vst3");
    REQUIRE(abandoned.status == scanner::ScanStatus::Crashed);
    REQUIRE(abandoned.error == "it died");
    // Half-gathered findings describe a plugin we have just decided not to trust. Keeping them
    // would put a usable-looking entry in front of the user.
    REQUIRE(abandoned.classes.empty());
    REQUIRE_FALSE(reader.inFlight());
}

TEST_CASE("scanning a working plugin reports what the host would see", "[scanner]") {
    const scanner::ScanReport report = scanner::scanModules({AIP_TEST_PLUGIN_PATH});

    REQUIRE(report.modules.size() == 1);
    CAPTURE(report.modules[0].error);
    REQUIRE(report.modules[0].status == scanner::ScanStatus::Ok);
    REQUIRE(report.modules[0].classes.size() == 3);
    REQUIRE(report.modules[0].classes[0].name == "AIP Test Plugin");
    REQUIRE(report.modules[0].classes[0].prepared);
    REQUIRE(report.modules[0].classes[0].parameterCount > 0);
    CHECK_FALSE(report.modules[0].classes[0].padded);

    // The second class takes eight channels or nothing, and the probe format is stereo. It is
    // still reported as prepared, because the host pads it rather than refusing it -- and
    // `padded` is what tells a shell the difference, which matters for any plugin whose
    // behaviour depends on the whole bus rather than on each channel separately.
    REQUIRE(report.modules[0].classes[1].name == "AIP Wide Plugin");
    CAPTURE(report.modules[0].classes[1].error);
    REQUIRE(report.modules[0].classes[1].prepared);
    CHECK(report.modules[0].classes[1].padded);
    // Read before the negotiation, so this is what the class declares, not what it settled for.
    CHECK(report.modules[0].classes[1].mainInputChannels == 8);

    // The third class is the one that will not name its output arrangement at all -- a JUCE
    // discrete-channel bus, and the shape that used to be reported as unloadable. It has to come
    // back prepared and padded like any other fixed-width plugin, because that is what it is; a
    // scan that lists it with an error sends the user to look for a broken plugin that works.
    REQUIRE(report.modules[0].classes[2].name == "AIP Nameless Bus Plugin");
    CAPTURE(report.modules[0].classes[2].error);
    REQUIRE(report.modules[0].classes[2].prepared);
    CHECK(report.modules[0].classes[2].padded);
    CHECK(report.modules[0].classes[2].mainInputChannels == 2);
    CHECK(report.modules[0].classes[2].mainOutputChannels == 15);

    // One process for a clean list, which is sec. 7.2's "one short-lived scanner process per
    // scan". The per-plugin cost only appears when a plugin makes it appear.
    REQUIRE(report.childProcesses == 1);
}

TEST_CASE("a plugin that faults costs one entry, not the scan", "[scanner]") {
    // The claim the whole component exists to make. aip_crash_plugin dereferences null inside
    // GetPluginFactory, in the loader's own call.
    const scanner::ScanReport report = scanner::scanModules({AIP_CRASH_PLUGIN_PATH, AIP_TEST_PLUGIN_PATH});

    REQUIRE(report.modules.size() == 2);

    REQUIRE(report.modules[0].path == AIP_CRASH_PLUGIN_PATH);
    REQUIRE(report.modules[0].status == scanner::ScanStatus::Crashed);
    REQUIRE_FALSE(report.modules[0].error.empty());

    // The evidence that the scan *continued* rather than merely finished: the plugin behind the
    // crash was probed properly, by a second child.
    REQUIRE(report.modules[1].path == AIP_TEST_PLUGIN_PATH);
    REQUIRE(report.modules[1].status == scanner::ScanStatus::Ok);
    REQUIRE(report.modules[1].classes.size() == 3);
    REQUIRE(report.modules[1].classes[0].name == "AIP Test Plugin");

    REQUIRE(report.childProcesses == 2);
}

TEST_CASE("a plugin that hangs costs one entry, not the scan", "[scanner]") {
    // Distinct from a crash in cause and identical in consequence: aip_hang_plugin never returns
    // from GetPluginFactory, so the child has to be given up on rather than mourned.
    scanner::ScanOptions options;
    options.moduleTimeoutMs = 1500;

    const scanner::ScanReport report = scanner::scanModules({AIP_HANG_PLUGIN_PATH, AIP_TEST_PLUGIN_PATH}, options);

    REQUIRE(report.modules.size() == 2);
    REQUIRE(report.modules[0].status == scanner::ScanStatus::TimedOut);
    REQUIRE_FALSE(report.modules[0].error.empty());

    REQUIRE(report.modules[1].status == scanner::ScanStatus::Ok);
    REQUIRE(report.modules[1].classes.size() == 3);

    REQUIRE(report.childProcesses == 2);
}

TEST_CASE("a path that is not a plugin is a clean answer, not a crash", "[scanner]") {
    const scanner::ScanReport report = scanner::scanModules({"C:/nowhere/NotAPlugin.vst3", AIP_TEST_PLUGIN_PATH});

    REQUIRE(report.modules.size() == 2);
    REQUIRE(report.modules[0].status == scanner::ScanStatus::LoadFailed);
    REQUIRE_FALSE(report.modules[0].error.empty());
    REQUIRE(report.modules[1].status == scanner::ScanStatus::Ok);
    // Nothing died, so nothing had to be restarted.
    REQUIRE(report.childProcesses == 1);
}

TEST_CASE("every requested path gets an entry", "[scanner]") {
    // A caller that has to special-case a short vector will eventually forget to. This also
    // covers the case where the child cannot be started at all.
    scanner::ScanOptions options;
    options.childExecutable = "C:/nowhere/aip_scan.exe";

    const scanner::ScanReport report = scanner::scanModules({AIP_TEST_PLUGIN_PATH, AIP_CRASH_PLUGIN_PATH}, options);

    REQUIRE(report.modules.size() == 2);
    for (const scanner::ScannedModule& module : report.modules) {
        REQUIRE_FALSE(module.usable());
        REQUIRE_FALSE(module.error.empty());
    }
}

TEST_CASE("progress is reported as entries land", "[scanner]") {
    std::vector<std::string> seen;
    std::size_t lastTotal = 0;

    const scanner::ScanReport report =
        scanner::scanModules({AIP_TEST_PLUGIN_PATH, AIP_CRASH_PLUGIN_PATH, AIP_TEST_PLUGIN_PATH}, {},
            [&](const scanner::ScannedModule& module, std::size_t done, std::size_t total) {
                seen.push_back(module.path);
                REQUIRE(done == seen.size());
                lastTotal = total;
            });

    REQUIRE(seen.size() == 3);
    REQUIRE(lastTotal == 3);
    REQUIRE(report.modules.size() == 3);
    // A shell showing a progress bar needs the crashed entry to arrive like any other, not to be
    // silently skipped and leave the bar short.
    REQUIRE(report.modules[1].status == scanner::ScanStatus::Crashed);
    REQUIRE(report.modules[2].status == scanner::ScanStatus::Ok);
}
