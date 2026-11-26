#pragma once

// ---------------------------------------------------------------------------
// core/format_util.h – C++20 locale-independent formatting (std::format) and
// range joining helpers.
//
// std::format without the 'L' specifier always formats numbers in the classic
// "C" locale ('.' decimal separator), keeping libavfilter expressions immune
// to ambient setlocale() changes from third-party libraries.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <concepts>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace core {

[[nodiscard]] inline std::string format_fixed(double value, int precision = 6) {
    return std::format("{:.{}f}", value, precision);
}

[[nodiscard]] inline std::string format_hms(double seconds) {
    constexpr long long kSecondsPerMinute = 60;
    constexpr long long kSecondsPerHour = 3600;
    constexpr double kMaxFormatSeconds = 9.0e15;  // safely representable in long long

    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return "00:00:00";
    }
    const auto total = static_cast<long long>(std::min(seconds, kMaxFormatSeconds));
    const auto h = total / kSecondsPerHour;
    const auto m = (total % kSecondsPerHour) / kSecondsPerMinute;
    const auto s = total % kSecondsPerMinute;
    return std::format("{:02}:{:02}:{:02}", h, m, s);
}

[[nodiscard]] inline std::string format_zero_padded(std::string_view prefix,
                                                    int index,
                                                    int width,
                                                    std::string_view suffix) {
    return std::format("{}{:0{}}{}", prefix, index, width, suffix);
}

[[nodiscard]] inline std::string format_speed(double ratio) {
    return std::format("{:.3g}x", ratio);
}

template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>
[[nodiscard]] inline std::string join(R&& range, std::string_view sep) {
    std::string result;
    bool first = true;
    for (auto&& elem : range) {
        if (!first) {
            result.append(sep.data(), sep.size());
        }
        first = false;
        const std::string_view sv = elem;
        result.append(sv.data(), sv.size());
    }
    return result;
}

}  // namespace core
