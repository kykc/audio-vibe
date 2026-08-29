// Enforces the ASCII-only rule of design_doc.md sec. 6.6.
//
// Sec. 6.6 notes that the rule is mechanically checkable and that the check is cheap. This is
// that check, wired into ctest so a stray typographic character fails the build rather than
// surfacing later as a mangled test filter or as mojibake in a diagnostic.
//
// It walks the source tree rather than taking a file list, so a newly added file is covered
// without anyone remembering to register it.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Directories that are not ours: version control metadata, the pixi environment (thousands of
// third-party headers), and build output.
const std::set<std::string> kSkippedDirectories = {".git", ".pixi", "build", "out"};

// `.ui` is Qt Designer's XML. It is tracked text like any other and Designer will happily write a
// typographic character into a label -- which then reaches the user through a generated header
// that nothing else here inspects.
const std::set<std::string> kCheckedExtensions = {
    ".h", ".hpp", ".cpp", ".md", ".txt", ".json", ".toml", ".rc", ".ui"};

// Files with no extension that are still ours. `LICENSE` is here because a licence is pasted from
// somewhere else more often than it is typed, which is exactly how a non-ASCII byte arrives.
const std::set<std::string> kCheckedNames = {".clang-format", ".gitignore", ".gitattributes", "LICENSE"};

bool isChecked(const fs::path& path) {
    const std::string name = path.filename().string();
    if (kCheckedNames.count(name) != 0) {
        return true;
    }
    return kCheckedExtensions.count(path.extension().string()) != 0;
}

struct Offence {
    std::string file;
    std::size_t line;
    std::size_t column;
    unsigned char byte;
};

/// Returns the first non-ASCII byte in `path`, if any. One is enough to fail, and reporting the
/// first keeps the failure message readable.
bool firstNonAscii(const fs::path& path, const fs::path& root, Offence& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::size_t line = 1;
    std::size_t column = 1;
    char raw = 0;
    while (file.get(raw)) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte >= 0x80) {
            out = Offence{fs::relative(path, root).generic_string(), line, column, byte};
            return true;
        }
        if (byte == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return false;
}

} // namespace

TEST_CASE("every tracked text file is ASCII-only", "[hygiene][ascii]") {
    const fs::path root = AIP_SOURCE_DIR;
    REQUIRE(fs::exists(root / "design_doc.md")); // the source tree is where we think it is

    std::vector<Offence> offences;
    std::size_t checked = 0;

    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (kSkippedDirectories.count(it->path().filename().string()) != 0) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file() || !isChecked(it->path())) {
            continue;
        }

        ++checked;
        Offence offence;
        if (firstNonAscii(it->path(), root, offence)) {
            offences.push_back(offence);
        }
    }

    // A guard against the walk silently finding nothing -- an empty pass is not a pass.
    CHECK(checked > 20);
    INFO("files checked: " << checked);

    for (const Offence& offence : offences) {
        char message[512];
        std::snprintf(message, sizeof(message),
            "%s:%zu:%zu has byte 0x%02X -- see design_doc.md sec. 6.6 for the ASCII "
            "substitution table",
            offence.file.c_str(), offence.line, offence.column, static_cast<unsigned>(offence.byte));
        FAIL_CHECK(message);
    }

    CHECK(offences.empty());
}
