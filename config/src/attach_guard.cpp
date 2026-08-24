#include "aip/config/attach_guard.h"

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace aip::config {

AttachGuard::AttachGuard(const fs::path& sessionPath) : path_(markPath(sessionPath)) {}

AttachGuard::~AttachGuard() { clear(); }

fs::path AttachGuard::markPath(const fs::path& sessionPath) {
    if (sessionPath.empty()) {
        return {};
    }
    fs::path path = sessionPath;
    path += ".attached";
    return path;
}

UncleanAttach AttachGuard::takePrevious(const fs::path& sessionPath) {
    const fs::path path = markPath(sessionPath);
    if (path.empty()) {
        return {};
    }

    UncleanAttach previous;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return {};
        }
        previous.present = true;
        std::getline(file, previous.endpointName);
    }
    // Consumed, whatever it said. See the header: a mark that survives being acted on is a shell
    // that never attaches on its own again.
    std::error_code ec;
    fs::remove(path, ec);
    // Written by us and only by us, but a truncated write is possible in principle.
    while (!previous.endpointName.empty() &&
        (previous.endpointName.back() == '\r' || previous.endpointName.back() == '\n')) {
        previous.endpointName.pop_back();
    }
    return previous;
}

void AttachGuard::mark(const std::string& endpointName) {
    if (path_.empty() || (marked_ && endpointName == endpointName_)) {
        return;
    }

    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    if (!file) {
        // Nothing to do about it, and nothing worth refusing to attach over: the cost is that
        // this particular run is unprotected, not that the shell stops working.
        return;
    }
    file << endpointName << '\n';
    file.flush();
    marked_ = true;
    endpointName_ = endpointName;
}

void AttachGuard::clear() {
    if (path_.empty() || !marked_) {
        return;
    }
    std::error_code ec;
    fs::remove(path_, ec);
    marked_ = false;
    endpointName_.clear();
}

} // namespace aip::config
