#include "aip/config/base64.h"

#include <array>
#include <cstdint>

namespace aip::config {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kPad = '=';

/// Character -> 6-bit value, with 0xFF for "not a base64 character at all". Built once at
/// compile time so decoding is a table lookup rather than a search.
constexpr std::array<std::uint8_t, 256> makeReverseTable() {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = 0xFF;
    }
    for (std::uint8_t i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = i;
    }
    return table;
}

constexpr std::array<std::uint8_t, 256> kReverse = makeReverseTable();

[[nodiscard]] constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

} // namespace

std::string base64Encode(const std::vector<char>& data, std::size_t lineLength) {
    std::string out;
    if (data.empty()) {
        return out;
    }
    out.reserve(((data.size() + 2) / 3) * 4 + (lineLength > 0 ? data.size() / lineLength + 2 : 0));

    std::size_t column = 0;
    const auto emit = [&](char c) {
        if (lineLength > 0 && column == lineLength) {
            out.push_back('\n');
            column = 0;
        }
        out.push_back(c);
        ++column;
    };

    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const auto b0 = static_cast<std::uint8_t>(data[i]);
        const auto b1 = static_cast<std::uint8_t>(data[i + 1]);
        const auto b2 = static_cast<std::uint8_t>(data[i + 2]);
        emit(kAlphabet[b0 >> 2]);
        emit(kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        emit(kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)]);
        emit(kAlphabet[b2 & 0x3F]);
    }

    // One or two bytes left over. Padded to a whole quantum, so the decoder can tell how many of
    // them were real without being told the length separately.
    const std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        const auto b0 = static_cast<std::uint8_t>(data[i]);
        emit(kAlphabet[b0 >> 2]);
        emit(kAlphabet[(b0 & 0x03) << 4]);
        emit(kPad);
        emit(kPad);
    } else if (remaining == 2) {
        const auto b0 = static_cast<std::uint8_t>(data[i]);
        const auto b1 = static_cast<std::uint8_t>(data[i + 1]);
        emit(kAlphabet[b0 >> 2]);
        emit(kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        emit(kAlphabet[(b1 & 0x0F) << 2]);
        emit(kPad);
    }

    return out;
}

bool base64Decode(const std::string& text, std::vector<char>& out) {
    out.clear();

    // One quantum at a time. `quantum` accumulates up to four 6-bit groups; `filled` counts how
    // many of them are real characters rather than padding.
    std::uint32_t quantum = 0;
    int filled = 0;
    int padding = 0;

    for (const char c : text) {
        if (isSpace(c)) {
            continue;
        }
        if (c == kPad) {
            // Padding only ever ends a message, and only two characters of it can be meaningful.
            ++padding;
            if (padding > 2) {
                out.clear();
                return false;
            }
            quantum <<= 6;
            ++filled;
        } else {
            // A character after the padding means this is not one encoded message.
            if (padding > 0) {
                out.clear();
                return false;
            }
            const std::uint8_t value = kReverse[static_cast<unsigned char>(c)];
            if (value == 0xFF) {
                out.clear();
                return false;
            }
            quantum = (quantum << 6) | value;
            ++filled;
        }

        if (filled == 4) {
            out.push_back(static_cast<char>((quantum >> 16) & 0xFF));
            out.push_back(static_cast<char>((quantum >> 8) & 0xFF));
            out.push_back(static_cast<char>(quantum & 0xFF));
            quantum = 0;
            filled = 0;
        }
    }

    // A partial quantum at the end is a truncated message: base64 has no way to represent one,
    // so accepting it would mean inventing bytes.
    if (filled != 0) {
        out.clear();
        return false;
    }
    out.resize(out.size() - static_cast<std::size_t>(padding));
    return true;
}

} // namespace aip::config
