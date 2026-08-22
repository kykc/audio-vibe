#include "aip/config/file_stamp.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace aip::config {
namespace {

void accumulate(const fs::path& file, FileStamp& stamp, bool& ok) {
    std::error_code ec;

    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        ok = false;
        return;
    }
    stamp.size += static_cast<std::uint64_t>(size);

    const fs::file_time_type written = fs::last_write_time(file, ec);
    if (ec) {
        ok = false;
        return;
    }
    const auto ticks = static_cast<std::int64_t>(written.time_since_epoch().count());
    if (ticks > stamp.modified) {
        stamp.modified = ticks;
    }
}

} // namespace

FileStamp stampFor(const std::string& path) {
    FileStamp stamp;
    if (path.empty()) {
        return stamp;
    }

    // The path comes from the SDK's own enumeration, which produces UTF-8 on Windows. Going
    // through u8string keeps a plugin under a name we cannot spell in the local code page
    // stampable rather than silently unstampable -- and an unstampable plugin is one that gets
    // re-probed on every start.
    const fs::path root = fs::path(std::u8string(path.begin(), path.end()));

    std::error_code ec;
    bool ok = true;

    if (!fs::is_directory(root, ec)) {
        accumulate(root, stamp, ok);
        return ok ? stamp : FileStamp{};
    }

    // Deliberately not `recursive_directory_iterator(root)` with the throwing overload: a bundle
    // with one unreadable file in it must produce an invalid stamp, not an exception on a path
    // that runs at every start.
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return {};
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return {};
        }
        if (it->is_regular_file(ec) && !ec) {
            accumulate(it->path(), stamp, ok);
            if (!ok) {
                return {};
            }
        }
    }

    return stamp;
}

} // namespace aip::config
