// Base64, because a plugin's state is a binary blob and the session file is text.
//
// The blob is the one thing in the file a human cannot read, and there is no way round that: it
// is whatever the plugin chose to write, and only the plugin can interpret it. Encoding it is
// still the better trade -- the alternative is either a second file per plugin or a binary
// config, and both cost more than one unreadable field (project owner, 2026-08-22).
//
// Wrapped at a fixed column on the way out, whitespace-tolerant on the way back in, so a state
// blob is a block of short lines rather than one line thousands of characters long. That is what
// keeps the file diffable and keeps an editor from choking on it.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace aip::config {

/// Line length for the wrapped form. 76 is the MIME convention and fits any terminal.
inline constexpr std::size_t kBase64LineLength = 76;

/// `lineLength` 0 emits one unbroken line.
[[nodiscard]] std::string base64Encode(const std::vector<char>& data,
                                       std::size_t lineLength = kBase64LineLength);

/// Ignores any whitespace, so it reads back what `base64Encode` wrapped and also whatever a
/// hand-edit turned it into. False means the text is not valid base64 -- a wrong character, or a
/// length that cannot have come from an encoder -- and `out` is left empty.
[[nodiscard]] bool base64Decode(const std::string& text, std::vector<char>& out);

} // namespace aip::config
