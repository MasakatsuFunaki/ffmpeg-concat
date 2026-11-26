#pragma once

// ---------------------------------------------------------------------------
// av/bgr_converter.h – Architecture-isolated, libav*-free BGR24 pixel
// conversion policies and BGRA->BGR24 alpha-stripper.
//
// Uses C++20 concepts, Policy Templates, if constexpr, and [[no_unique_address]]
// keyed off core::arch::kIsArm64 so zero #ifdef / #if defined(...) directives
// appear in domain code and BOTH DirectBgr24Policy and BgraBridgePolicy compile
// and remain unit-testable on every CPU architecture.
// ---------------------------------------------------------------------------

#include "core/arch.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace av {

inline constexpr std::size_t kBgr24BytesPerPixel = 3;
inline constexpr std::size_t kBgraBytesPerPixel = 4;

// Strips tightly packed 32-bit BGRA pixels into 24-bit BGR24 pixels.
// Returns false if bgra.size() is not a multiple of 4 or bgr24.size() does not
// equal (bgra.size() / 4) * 3. Empty buffers (0 -> 0) succeed.
[[nodiscard]] inline bool strip_bgra_to_bgr24(
    std::span<const std::uint8_t> bgra,
    std::span<std::uint8_t> bgr24) noexcept {
    if ((bgra.size() % kBgraBytesPerPixel) != 0U) {
        return false;
    }
    const std::size_t pixel_count = bgra.size() / kBgraBytesPerPixel;
    if (bgr24.size() != pixel_count * kBgr24BytesPerPixel) {
        return false;
    }
    if (pixel_count == 0U) {
        return true;
    }

    const std::uint8_t* src = bgra.data();
    std::uint8_t* dst = bgr24.data();
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::size_t s = i * kBgraBytesPerPixel;
        const std::size_t d = i * kBgr24BytesPerPixel;
        dst[d] = src[s];
        dst[d + 1] = src[s + 1];
        dst[d + 2] = src[s + 2];
    }
    return true;
}

template <typename Policy>
concept BgrConvertPolicy = requires(Policy& policy, std::size_t pixel_count) {
    { Policy::kCanUseBgraBridge } -> std::convertible_to<bool>;
    { policy.acquire_bgra_scratch(pixel_count) } -> std::same_as<std::span<std::uint8_t>>;
    { policy.clear() } noexcept;
};

// Direct BGR24 policy (x86_64): stateless and zero-overhead via [[no_unique_address]].
struct DirectBgr24Policy {
    static constexpr bool kCanUseBgraBridge = false;

    [[nodiscard]] static std::span<std::uint8_t> acquire_bgra_scratch(
        std::size_t /*pixel_count*/) noexcept {
        return {};
    }

    static constexpr void clear() noexcept {}
};

// BGRA-bridge policy (ARM64): routes YUV420P through 32-bit BGRA into a reusable
// scratch buffer, then strips alpha into tightly packed BGR24.
class BgraBridgePolicy {
public:
    static constexpr bool kCanUseBgraBridge = true;

    [[nodiscard]] std::span<std::uint8_t> acquire_bgra_scratch(std::size_t pixel_count) {
        const std::size_t bytes = pixel_count * kBgraBytesPerPixel;
        if (scratch_.size() < bytes) {
            scratch_.resize(bytes);
        }
        return std::span<std::uint8_t>{scratch_.data(), bytes};
    }

    void clear() noexcept { scratch_.clear(); }

private:
    std::vector<std::uint8_t> scratch_;
};

using DefaultBgrConvertPolicy =
    std::conditional_t<core::arch::kIsArm64, BgraBridgePolicy, DirectBgr24Policy>;

static_assert(BgrConvertPolicy<DirectBgr24Policy>);
static_assert(BgrConvertPolicy<BgraBridgePolicy>);
static_assert(BgrConvertPolicy<DefaultBgrConvertPolicy>);

// Orchestrates direct BGR24 or BGRA-bridge pixel conversion via a caller-supplied
// scale callable: bool scale_fn(bool to_bgra, std::uint8_t* dst, int dst_stride).
template <BgrConvertPolicy Policy = DefaultBgrConvertPolicy>
class BgrConverter {
public:
    template <typename ScaleFn>
        requires std::invocable<ScaleFn&, bool, std::uint8_t*, int> &&
                 std::convertible_to<std::invoke_result_t<ScaleFn&, bool, std::uint8_t*, int>, bool>
    [[nodiscard]] bool convert(int width,
                               int height,
                               bool is_yuv420p,
                               std::span<std::uint8_t> out_bgr24,
                               ScaleFn&& scale_fn) {
        if (width <= 0 || height <= 0) {
            return false;
        }
        if (width > std::numeric_limits<int>::max() / static_cast<int>(kBgraBytesPerPixel)) {
            return false;
        }

        const std::size_t pixel_count =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (out_bgr24.size() != pixel_count * kBgr24BytesPerPixel) {
            return false;
        }

        if constexpr (Policy::kCanUseBgraBridge) {
            if (is_yuv420p) {
                const auto bgra = policy_.acquire_bgra_scratch(pixel_count);
                const int bgra_stride = width * static_cast<int>(kBgraBytesPerPixel);
                if (!std::invoke(scale_fn, true, bgra.data(), bgra_stride)) {
                    return false;
                }
                return strip_bgra_to_bgr24(bgra, out_bgr24);
            }
        } else {
            (void)is_yuv420p;
        }

        const int bgr_stride = width * static_cast<int>(kBgr24BytesPerPixel);
        return std::invoke(scale_fn, false, out_bgr24.data(), bgr_stride);
    }

    void clear() noexcept { policy_.clear(); }

private:
    [[no_unique_address]] Policy policy_{};
};

}  // namespace av
