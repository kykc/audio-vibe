// Paths across the Qt boundary, in one place.
//
// Two lines that are easy to write again slightly differently, and the difference does not show
// up on a developer machine: a `toStdString()` conversion names a *different* file, or no file at
// all, as soon as a path contains a character outside the local code page -- a user name in
// Cyrillic is enough. Wide strings are the only correct form here, and having one spelling of it
// is what keeps a second caller from quietly reintroducing the narrow one.

#pragma once

#include <QString>

#include <filesystem>

namespace aip::ui {

inline std::filesystem::path toPath(const QString& text) {
    return text.isEmpty() ? std::filesystem::path{} : std::filesystem::path(text.toStdWString());
}

inline QString fromPath(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }

} // namespace aip::ui
