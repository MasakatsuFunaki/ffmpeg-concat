#pragma once

// ---------------------------------------------------------------------------
// core/arch.h – Single source of truth for compile-time target architecture
// and OS predicates. Domain code uses `if constexpr (core::arch::...)` rather
// than raw preprocessor `#if defined(...)` checks.
// ---------------------------------------------------------------------------

namespace core::arch {

#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr bool kIsArm64 = true;
#else
inline constexpr bool kIsArm64 = false;
#endif

#if defined(_WIN32)
inline constexpr bool kIsWindows = true;
#else
inline constexpr bool kIsWindows = false;
#endif

}  // namespace core::arch
