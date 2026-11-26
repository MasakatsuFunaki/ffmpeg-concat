#pragma once

// ---------------------------------------------------------------------------
// platform/time_util.h – Thread-safe local calendar time conversion.
//
// Platform implementations live in time_util_{win32,posix}.cpp and are
// selected by CMake so domain code stays free of OS #ifdef directives.
// ---------------------------------------------------------------------------

#include <ctime>
#include <optional>

namespace platform {

// Converts `t` to local calendar time via the platform's thread-safe API
// (localtime_s on Windows, localtime_r on POSIX). Returns std::nullopt if
// the timestamp cannot be represented.
[[nodiscard]] std::optional<std::tm> local_time(std::time_t t) noexcept;

}  // namespace platform
