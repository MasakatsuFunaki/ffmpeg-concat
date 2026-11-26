#pragma once

// ---------------------------------------------------------------------------
// core/path_util.h – C++20 UTF-8 <-> std::filesystem::path conversions.
//
// On Windows, std::filesystem::path::string() converts UTF-16 to the process
// ANSI codepage (throwing std::system_error on characters outside the active
// codepage, e.g. CJK or emoji), whereas libavformat/avio_open expect UTF-8.
// These helpers convert through C++20 char8_t / std::u8string explicitly
// without reinterpret_cast or strict-aliasing UB.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>
#include <string_view>

namespace core {

// Convert any filesystem path to its UTF-8 byte sequence (safe for libav* C
// APIs, Boost.Log char streams, and JSON/CSV diagnostics on Windows & POSIX).
[[nodiscard]] inline std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    std::string out;
    out.reserve(u8.size());
    for (const char8_t ch : u8) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

// Construct a native filesystem path from a UTF-8 byte sequence (losslessly
// decoded to UTF-16 on Windows and preserved verbatim on POSIX).
[[nodiscard]] inline std::filesystem::path from_utf8(std::string_view utf8) {
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char ch : utf8) {
        u8.push_back(static_cast<char8_t>(static_cast<unsigned char>(ch)));
    }
    return std::filesystem::path{u8};
}

}  // namespace core
