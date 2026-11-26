#pragma once

// ---------------------------------------------------------------------------
// platform/locale_guard.h – Thread-safe RAII guard pinning LC_NUMERIC to "C"
// on the calling thread only, restoring the previous thread locale on exit.
//
// PIMPL-insulated from OS locale headers; platform implementations live in
// locale_guard_{win32,posix}.cpp and are selected by CMake.
// ---------------------------------------------------------------------------

#include <memory>

namespace platform {

class ScopedCNumericLocale final {
public:
    ScopedCNumericLocale();
    ~ScopedCNumericLocale() noexcept;

    ScopedCNumericLocale(const ScopedCNumericLocale&) = delete;
    ScopedCNumericLocale& operator=(const ScopedCNumericLocale&) = delete;
    ScopedCNumericLocale(ScopedCNumericLocale&&) noexcept;
    ScopedCNumericLocale& operator=(ScopedCNumericLocale&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace platform
