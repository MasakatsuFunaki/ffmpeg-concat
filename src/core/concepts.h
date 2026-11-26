#pragma once

// ---------------------------------------------------------------------------
// core/concepts.h – shared C++20 concepts and constexpr range/string helpers.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>

namespace core {

template <typename T>
concept Arithmetic = std::integral<T> || std::floating_point<T>;

template <typename T>
concept StringLike = std::convertible_to<T, std::string_view>;

template <typename R>
concept PathRange =
    std::ranges::input_range<R> &&
    std::convertible_to<std::ranges::range_reference_t<R>, const std::filesystem::path&>;

template <typename R, typename T>
concept RangeOf =
    std::ranges::input_range<R> &&
    std::convertible_to<std::ranges::range_reference_t<R>, const T&>;

template <typename Pred, typename T>
concept PredicateFor = std::predicate<Pred, const T&>;

template <typename P>
concept ResettablePointer = requires(P& p, typename P::pointer raw) {
    typename P::pointer;
    { p.reset(raw) } noexcept;
    { p.release() } noexcept -> std::same_as<typename P::pointer>;
};

[[nodiscard]] constexpr char ascii_to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv.size(), '\0');
    std::ranges::transform(sv, out.begin(), ascii_to_lower);
    return out;
}

// C++20 equivalent of C++23 std::ranges::contains.
template <std::ranges::input_range R, typename T>
    requires std::indirect_binary_predicate<
        std::ranges::equal_to,
        std::ranges::iterator_t<R>,
        const T*>
[[nodiscard]] constexpr bool contains(R&& range, const T& value) {
    return std::ranges::find(range, value) != std::ranges::end(range);
}

}  // namespace core
