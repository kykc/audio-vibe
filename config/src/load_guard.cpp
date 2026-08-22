#include "aip/config/load_guard.h"

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace aip::config {

LoadGuard::LoadGuard(const fs::path& sessionPath) : path_(breadcrumbPath(sessionPath)) {}

LoadGuard::~LoadGuard() {
    clear();
}

fs::path LoadGuard::breadcrumbPath(const fs::path& sessionPath) {
    if (sessionPath.empty()) {
        return {};
    }
    fs::path path = sessionPath;
    path += ".loading";
    return path;
}

std::string LoadGuard::takePreviousCasualty(const fs::path& sessionPath) {
    const fs::path path = breadcrumbPath(sessionPath);
    if (path.empty()) {
        return {};
    }

    std::string line;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return {};
        }
        std::getline(file, line);
    }
    // Consumed, whatever it said. See the header: a breadcrumb that survives being acted on is a
    // plugin that can never be retried.
    std::error_code ec;
    fs::remove(path, ec);
    // Written by us and only by us, but a truncated write is possible in principle -- a trailing
    // carriage return would turn a path into one that matches nothing.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

void LoadGuard::mark(const std::string& path) {
    if (path_.empty()) {
        return;
    }

    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    if (!file) {
        // Nothing to do about it, and nothing worth failing a session restore over: the cost is
        // that this particular start has no crash protection, not that it does not happen.
        return;
    }
    file << path << '\n';
    // Explicit, and the whole point. The data has to reach the operating system before the load
    // begins, because the process may not exist afterwards to flush it -- and a breadcrumb still
    // sitting in a stream buffer names nothing.
    file.flush();
    marked_ = true;
}

void LoadGuard::clear() {
    if (path_.empty() || !marked_) {
        return;
    }
    std::error_code ec;
    fs::remove(path_, ec);
    marked_ = false;
}

} // namespace aip::config
