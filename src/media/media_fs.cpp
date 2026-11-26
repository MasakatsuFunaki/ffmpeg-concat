#include "media/media_fs.h"

#include "core/concepts.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace media {

namespace {

constexpr double kBytesPerMiB = 1024.0 * 1024.0;
constexpr int kTmYearBase = 1900;
constexpr int kTmMonthOffset = 1;
constexpr std::size_t kTimestampLen = 19;  // "YYYY-MM-DD HH-MM-SS"
constexpr int kMinValidYear = 1900;
constexpr int kMonthsPerYear = 12;
constexpr int kMaxHour = 23;
constexpr int kMaxMinuteOrSecond = 59;

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

[[nodiscard]] constexpr int days_in_month(int year, int month) noexcept {
    constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > kMonthsPerYear) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[static_cast<std::size_t>(month - 1)];
}

[[nodiscard]] constexpr bool has_timestamp_shape(std::string_view sv) noexcept {
    if (sv.size() < kTimestampLen) {
        return false;
    }
    constexpr std::array<std::pair<std::size_t, char>, 5> kDelimiters{{
        {4, '-'}, {7, '-'}, {10, ' '}, {13, '-'}, {16, '-'},
    }};
    for (const auto& [pos, ch] : kDelimiters) {
        if (sv[pos] != ch) {
            return false;
        }
    }
    constexpr std::array<std::size_t, 14> kDigitPositions{
        0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18,
    };
    return std::ranges::all_of(kDigitPositions, [sv](std::size_t pos) noexcept {
        return is_ascii_digit(sv[pos]);
    });
}

[[nodiscard]] std::optional<int> parse_digits(std::string_view sv) noexcept {
    int value = 0;
    const char* begin = sv.data();
    const char* end = begin + sv.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::time_t> parse_embedded_timestamp(std::string_view stem) noexcept {
    if (stem.size() < kTimestampLen) {
        return std::nullopt;
    }
    const std::size_t max_start = stem.size() - kTimestampLen;
    for (std::size_t pos = 0; pos <= max_start; ++pos) {
        const std::string_view candidate = stem.substr(pos, kTimestampLen);
        if (!has_timestamp_shape(candidate)) {
            continue;
        }
        const auto year  = parse_digits(candidate.substr(0, 4));
        const auto month = parse_digits(candidate.substr(5, 2));
        const auto day   = parse_digits(candidate.substr(8, 2));
        const auto hour  = parse_digits(candidate.substr(11, 2));
        const auto min   = parse_digits(candidate.substr(14, 2));
        const auto sec   = parse_digits(candidate.substr(17, 2));
        if (!year || !month || !day || !hour || !min || !sec) {
            continue;
        }
        if (*year < kMinValidYear || *month < 1 || *month > kMonthsPerYear ||
            *day < 1 || *day > days_in_month(*year, *month) ||
            *hour < 0 || *hour > kMaxHour ||
            *min < 0 || *min > kMaxMinuteOrSecond ||
            *sec < 0 || *sec > kMaxMinuteOrSecond) {
            continue;
        }

        std::tm tm{};
        tm.tm_year  = *year - kTmYearBase;
        tm.tm_mon   = *month - kTmMonthOffset;
        tm.tm_mday  = *day;
        tm.tm_hour  = *hour;
        tm.tm_min   = *min;
        tm.tm_sec   = *sec;
        tm.tm_isdst = -1;
        if (const std::time_t parsed = std::mktime(&tm);
            parsed != static_cast<std::time_t>(-1)) {
            return parsed;
        }
    }
    return std::nullopt;
}

struct DatedPath {
    fs::path path;
    std::time_t date = 0;
};

[[nodiscard]] std::vector<fs::path> collect_by_extensions(
    const fs::path& input_dir,
    std::span<const std::string_view> exts) {
    std::error_code ec;
    if (!fs::exists(input_dir, ec) || ec || !fs::is_directory(input_dir, ec) || ec) {
        return {};
    }

    fs::directory_iterator dir_it(input_dir, ec);
    if (ec) {
        return {};
    }

    std::vector<DatedPath> dated;
    for (const auto& entry : dir_it) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) {
            continue;
        }
        const std::string ext = core::to_lower_ascii(entry.path().extension().string());
        if (core::contains(exts, std::string_view{ext})) {
            const fs::path& path = entry.path();
            dated.push_back(DatedPath{path, file_date(path)});
        }
    }

    std::ranges::sort(dated, [](const DatedPath& a, const DatedPath& b) {
        if (a.date != b.date) {
            return a.date < b.date;
        }
        return a.path < b.path;
    });

    auto projected = dated | std::views::transform(&DatedPath::path);
    std::vector<fs::path> result;
    result.reserve(dated.size());
    std::ranges::copy(projected, std::back_inserter(result));
    return result;
}

}  // namespace

std::time_t file_date(const fs::path& filepath) {
    const std::string stem = filepath.stem().string();
    if (const auto parsed = parse_embedded_timestamp(stem)) {
        return *parsed;
    }

    std::error_code ec;
    const auto ftime = fs::last_write_time(filepath, ec);
    if (ec) {
        return static_cast<std::time_t>(0);
    }
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

std::string current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
                       tm.tm_year + kTmYearBase,
                       tm.tm_mon + kTmMonthOffset,
                       tm.tm_mday,
                       tm.tm_hour,
                       tm.tm_min,
                       tm.tm_sec);
}

std::vector<fs::path> collect_video_files(const fs::path& input_dir) {
    return collect_by_extensions(input_dir, kVideoExtensions);
}

std::vector<fs::path> collect_mp4_files(const fs::path& input_dir) {
    static constexpr std::array<std::string_view, 1> kMp4Extension{".mp4"};
    return collect_by_extensions(input_dir, kMp4Extension);
}

void log_output_size(const fs::path& output_file) {
    std::error_code ec;
    const auto size = fs::file_size(output_file, ec);
    const double size_mb = ec ? 0.0 : static_cast<double>(size) / kBytesPerMiB;
    BOOST_LOG_TRIVIAL(info) << "Success! Output saved to: " << output_file;
    BOOST_LOG_TRIVIAL(info) << std::format("Output file size: {:.2f} MB", size_mb);
}

}  // namespace media
