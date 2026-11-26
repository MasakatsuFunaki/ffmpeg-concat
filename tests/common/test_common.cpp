#include "common.h"
#include "av/av_capabilities.h"
#include "av/av_error.h"
#include "av/media_probe.h"
#include "core/concepts.h"
#include "core/format_util.h"
#include "core/scoped_handle.h"
#include "media/media_fs.h"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
namespace po = boost::program_options;

namespace {

struct NonResettableHandle {
    using pointer = int*;
    void reset(pointer) {}  // intentionally not noexcept
    pointer release() noexcept { return nullptr; }
};

struct MissingReleaseHandle {
    using pointer = int*;
    void reset(pointer) noexcept {}
};

// Compile-time concept satisfaction and rejection matrix for core/concepts.h.
static_assert(core::Arithmetic<int>);
static_assert(core::Arithmetic<double>);
static_assert(core::Arithmetic<std::uint8_t>);
static_assert(!core::Arithmetic<std::string>);
static_assert(!core::Arithmetic<int*>);
static_assert(!core::Arithmetic<NonResettableHandle>);

static_assert(core::StringLike<std::string>);
static_assert(core::StringLike<std::string_view>);
static_assert(core::StringLike<const char*>);
static_assert(core::StringLike<char[8]>);
static_assert(!core::StringLike<int>);
static_assert(!core::StringLike<fs::path>);
static_assert(!core::StringLike<void*>);

static_assert(core::PathRange<std::vector<fs::path>>);
static_assert(core::PathRange<std::span<const fs::path>>);
static_assert(core::PathRange<std::array<fs::path, 2>>);
static_assert(!core::PathRange<std::vector<int>>);
static_assert(!core::PathRange<int>);

static_assert(core::RangeOf<std::vector<int>, int>);
static_assert(core::RangeOf<std::span<const double>, double>);
static_assert(core::RangeOf<std::array<int, 3>, int>);
static_assert(!core::RangeOf<std::vector<std::string>, int>);
static_assert(!core::RangeOf<int, int>);

static_assert(core::PredicateFor<bool (*)(const int&), int>);
static_assert(core::PredicateFor<bool (*)(int), int>);
static_assert(!core::PredicateFor<void (*)(int), int>);
static_assert(!core::PredicateFor<bool (*)(const std::string&), int>);

static_assert(core::ResettablePointer<std::unique_ptr<int>>);
static_assert(!core::ResettablePointer<std::shared_ptr<int>>);
static_assert(!core::ResettablePointer<int*>);
static_assert(!core::ResettablePointer<NonResettableHandle>);
static_assert(!core::ResettablePointer<MissingReleaseHandle>);

static_assert(core::ascii_to_lower('A') == 'a');
static_assert(core::ascii_to_lower('Z') == 'z');
static_assert(core::ascii_to_lower('@') == '@');
static_assert(core::ascii_to_lower('[') == '[');
static_assert(core::ascii_to_lower(static_cast<char>(0x80)) == static_cast<char>(0x80));
static_assert(core::ascii_to_lower(static_cast<char>(0xFF)) == static_cast<char>(0xFF));

constexpr std::array<int, 3> kConstexprInts{10, 20, 30};
static_assert(core::contains(kConstexprInts, 10));
static_assert(core::contains(kConstexprInts, 20));
static_assert(core::contains(kConstexprInts, 30));
static_assert(!core::contains(kConstexprInts, 99));

// Compile-time FrameRate contract checks (av/media_probe.h).
static_assert(!av::FrameRate{0, 0}.known());
static_assert(!av::FrameRate{30, 0}.known());
static_assert(!av::FrameRate{0, 1}.known());
static_assert(!av::FrameRate{-30, 1}.known());
static_assert(!av::FrameRate{30, -1}.known());
static_assert(!av::FrameRate{-30, -1}.known());
static_assert(av::FrameRate{30, 1}.known());
static_assert(av::FrameRate{30000, 1001}.known());
static_assert(av::FrameRate{30, 1}.value() == 30.0);
static_assert(av::FrameRate{0, 0}.value() == -1.0);
static_assert(av::FrameRate{30, 1} == av::FrameRate{30, 1});
static_assert(av::FrameRate{30, 1} != av::FrameRate{60, 1});
static_assert(av::FrameRate{30, 1} != av::FrameRate{30, 2});

// ToolApp base must be non-copyable.
static_assert(!std::is_copy_constructible_v<ToolApp>);
static_assert(!std::is_copy_assignable_v<ToolApp>);

struct FakeResource {
    int id = 0;
    int* freed_count = nullptr;
    int* last_freed_id = nullptr;
};

void free_single_ptr(FakeResource* ptr) noexcept {
    if (!ptr) {
        return;
    }
    ++(*ptr->freed_count);
    *ptr->last_freed_id = ptr->id;
}

void free_double_ptr(FakeResource** pptr) noexcept {
    if (!pptr || !*pptr) {
        return;
    }
    ++(*(*pptr)->freed_count);
    *(*pptr)->last_freed_id = (*pptr)->id;
    *pptr = nullptr;
}

using SingleHandle = core::UniqueHandle<FakeResource, free_single_ptr>;
using DoubleHandle = core::UniqueHandle<FakeResource, free_double_ptr>;

static_assert(core::ResettablePointer<SingleHandle>);
static_assert(core::ResettablePointer<DoubleHandle>);

int c_api_alloc_out(FakeResource** out, FakeResource* to_assign, bool succeed) noexcept {
    if (!out || *out != nullptr) {
        return -1;
    }
    if (!succeed) {
        return -2;
    }
    *out = to_assign;
    return 0;
}

int c_api_replace_inout(FakeResource** inout, FakeResource* replacement) noexcept {
    if (!inout) {
        return -1;
    }
    if (*inout) {
        free_single_ptr(*inout);
    }
    *inout = replacement;
    return 0;
}

[[nodiscard]] bool to_local_tm(std::time_t value, std::tm& out) noexcept {
#ifdef _WIN32
    return localtime_s(&out, &value) == 0;
#else
    return localtime_r(&value, &out) != nullptr;
#endif
}

[[nodiscard]] bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] int parse_digits(std::string_view sv, std::size_t pos, std::size_t len) noexcept {
    int value = 0;
    for (std::size_t i = 0; i < len; ++i) {
        value = (value * 10) + (sv[pos + i] - '0');
    }
    return value;
}

class ArgvBuilder {
public:
    explicit ArgvBuilder(std::initializer_list<std::string_view> args) {
        storage_.reserve(args.size());
        for (const auto arg : args) {
            storage_.emplace_back(arg);
        }
        ptrs_.reserve(storage_.size());
        for (auto& s : storage_) {
            ptrs_.push_back(s.data());
        }
    }

    [[nodiscard]] int argc() const noexcept { return static_cast<int>(ptrs_.size()); }
    [[nodiscard]] char** argv() noexcept { return ptrs_.data(); }

private:
    std::vector<std::string> storage_;
    std::vector<char*> ptrs_;
};

class ScopedLogAndCoutCapture {
public:
    using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;

    ScopedLogAndCoutCapture() : old_cout_buf_(std::cout.rdbuf(cout_stream_.rdbuf())) {
        auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
        backend->add_stream(boost::shared_ptr<std::ostream>(&log_stream_, boost::null_deleter{}));
        backend->auto_flush(true);
        sink_ = boost::make_shared<Sink>(backend);
        boost::log::core::get()->add_sink(sink_);
    }

    ~ScopedLogAndCoutCapture() noexcept {
        if (sink_) {
            sink_->flush();
            boost::log::core::get()->remove_sink(sink_);
        }
        std::cout.rdbuf(old_cout_buf_);
    }

    ScopedLogAndCoutCapture(const ScopedLogAndCoutCapture&) = delete;
    ScopedLogAndCoutCapture& operator=(const ScopedLogAndCoutCapture&) = delete;

    [[nodiscard]] std::string combined() const {
        if (sink_) {
            sink_->flush();
        }
        return log_stream_.str() + cout_stream_.str();
    }

private:
    std::ostringstream log_stream_;
    std::ostringstream cout_stream_;
    std::streambuf* old_cout_buf_;
    boost::shared_ptr<Sink> sink_;
};

class StubToolApp final : public ToolApp {
public:
    int return_code = 0;
    mutable int name_calls = 0;
    int define_options_calls = 0;
    mutable int extra_help_calls = 0;
    int execute_calls = 0;
    std::string captured_tag;

protected:
    std::string name() const override {
        ++name_calls;
        return "stub_tool";
    }

    void define_options(po::options_description& desc) override {
        ++define_options_calls;
        desc.add_options()("tag,t", po::value<std::string>()->default_value("default"), "Tag value");
    }

    std::string extra_help() const override {
        ++extra_help_calls;
        return "Stub extra guidance.";
    }

    int execute(const po::variables_map& vm) override {
        ++execute_calls;
        captured_tag = vm["tag"].as<std::string>();
        return return_code;
    }
};

}  // namespace

// Core concepts and ASCII/range helpers

TEST(CoreConceptsTest, AsciiToLowerAndToLowerAsciiHandleBoundariesAndSignBit) {
    struct CharCase {
        char input;
        char expected;
    };
    constexpr std::array<CharCase, 12> kCases{{
        {'\0', '\0'},
        {'@', '@'},
        {'A', 'a'},
        {'M', 'm'},
        {'Z', 'z'},
        {'[', '['},
        {'a', 'a'},
        {'z', 'z'},
        {'0', '0'},
        {static_cast<char>(0x7F), static_cast<char>(0x7F)},
        {static_cast<char>(0x80), static_cast<char>(0x80)},
        {static_cast<char>(0xFF), static_cast<char>(0xFF)},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(static_cast<unsigned int>(static_cast<unsigned char>(tc.input)));
        EXPECT_EQ(core::ascii_to_lower(tc.input), tc.expected);
    }

    EXPECT_EQ(core::to_lower_ascii(""), "");
    EXPECT_EQ(core::to_lower_ascii("Hello_WORLD-123!@["), "hello_world-123!@[");

    constexpr std::array<char, 7> kRawBytes{
        'A', '\0', 'Z', '[', static_cast<char>(0x7F), static_cast<char>(0x80),
        static_cast<char>(0xFF)};
    constexpr std::array<char, 7> kExpectedBytes{
        'a', '\0', 'z', '[', static_cast<char>(0x7F), static_cast<char>(0x80),
        static_cast<char>(0xFF)};
    const std::string_view raw_sv(kRawBytes.data(), kRawBytes.size());
    const std::string lowered = core::to_lower_ascii(raw_sv);

    ASSERT_EQ(lowered.size(), kExpectedBytes.size());
    for (std::size_t i = 0; i < kExpectedBytes.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(lowered[i], kExpectedBytes[i]);
    }
}

TEST(CoreConceptsTest, ContainsFindsFirstMiddleLastAndRejectsEmptyOrAbsent) {
    constexpr std::array<int, 4> kValues{3, 7, 11, 19};
    EXPECT_TRUE(core::contains(kValues, 3));
    EXPECT_TRUE(core::contains(kValues, 11));
    EXPECT_TRUE(core::contains(kValues, 19));
    EXPECT_FALSE(core::contains(kValues, 0));
    EXPECT_FALSE(core::contains(kValues, 20));

    const std::span<const int> empty_span{};
    EXPECT_FALSE(core::contains(empty_span, 3));
}

// Core RAII scoped handles, out_ptr/inout_ptr adaptors, and ScopeGuard

TEST(ScopedHandleTest, UniqueHandleDispatchesSinglePointerDeleter) {
    int freed_count = 0;
    int last_freed_id = -1;
    FakeResource first{1, &freed_count, &last_freed_id};
    FakeResource second{2, &freed_count, &last_freed_id};

    {
        SingleHandle empty_handle;
        EXPECT_EQ(empty_handle.get(), nullptr);
        empty_handle.reset();
    }
    EXPECT_EQ(freed_count, 0);

    {
        SingleHandle h1(&first);
        SingleHandle h2(std::move(h1));
        EXPECT_EQ(h1.get(), nullptr);
        EXPECT_EQ(h2.get(), &first);
        EXPECT_EQ(freed_count, 0);

        h2.reset(&second);
        EXPECT_EQ(freed_count, 1);
        EXPECT_EQ(last_freed_id, 1);
    }
    EXPECT_EQ(freed_count, 2);
    EXPECT_EQ(last_freed_id, 2);
}

TEST(ScopedHandleTest, UniqueHandleDispatchesDoublePointerDeleter) {
    int freed_count = 0;
    int last_freed_id = -1;
    FakeResource first{10, &freed_count, &last_freed_id};
    FakeResource second{20, &freed_count, &last_freed_id};

    {
        DoubleHandle empty_handle;
        empty_handle.reset(nullptr);
    }
    EXPECT_EQ(freed_count, 0);

    {
        DoubleHandle h1(&first);
        DoubleHandle h2;
        h2 = std::move(h1);
        EXPECT_EQ(h1.get(), nullptr);
        EXPECT_EQ(h2.get(), &first);
        EXPECT_EQ(freed_count, 0);

        h2.reset(&second);
        EXPECT_EQ(freed_count, 1);
        EXPECT_EQ(last_freed_id, 10);
    }
    EXPECT_EQ(freed_count, 2);
    EXPECT_EQ(last_freed_id, 20);
}

TEST(ScopedHandleTest, OutPtrResetsExistingAcquiresNewAndHandlesFailure) {
    int freed_count = 0;
    int last_freed_id = -1;
    FakeResource first{101, &freed_count, &last_freed_id};
    FakeResource second{102, &freed_count, &last_freed_id};
    FakeResource third{103, &freed_count, &last_freed_id};

    {
        DoubleHandle handle(&first);
        ASSERT_EQ(c_api_alloc_out(core::out_ptr(handle), &second, true), 0);
        EXPECT_EQ(freed_count, 1);
        EXPECT_EQ(last_freed_id, 101);
        EXPECT_EQ(handle.get(), &second);

        // Failing C-API call frees prior handle and leaves target null.
        ASSERT_EQ(c_api_alloc_out(core::out_ptr(handle), &third, false), -2);
        EXPECT_EQ(freed_count, 2);
        EXPECT_EQ(last_freed_id, 102);
        EXPECT_EQ(handle.get(), nullptr);

        // Subsequent valid call re-acquires cleanly after failure.
        ASSERT_EQ(c_api_alloc_out(core::out_ptr(handle), &third, true), 0);
        EXPECT_EQ(handle.get(), &third);
    }
    EXPECT_EQ(freed_count, 3);
    EXPECT_EQ(last_freed_id, 103);
}

TEST(ScopedHandleTest, InOutPtrTransfersAndReacquiresWithoutDoubleFree) {
    int freed_count = 0;
    int last_freed_id = -1;
    FakeResource first{201, &freed_count, &last_freed_id};
    FakeResource second{202, &freed_count, &last_freed_id};

    {
        SingleHandle handle;
        ASSERT_EQ(c_api_replace_inout(core::inout_ptr(handle), &first), 0);
        EXPECT_EQ(handle.get(), &first);
        EXPECT_EQ(freed_count, 0);

        // C-API frees first and replaces pointer with second.
        ASSERT_EQ(c_api_replace_inout(core::inout_ptr(handle), &second), 0);
        EXPECT_EQ(handle.get(), &second);
        EXPECT_EQ(freed_count, 1);
        EXPECT_EQ(last_freed_id, 201);

        // C-API consumes and nulls second; handle destructor must not double-free.
        ASSERT_EQ(c_api_replace_inout(core::inout_ptr(handle), nullptr), 0);
        EXPECT_EQ(handle.get(), nullptr);
        EXPECT_EQ(freed_count, 2);
        EXPECT_EQ(last_freed_id, 202);
    }
    EXPECT_EQ(freed_count, 2);
}

TEST(ScopedHandleTest, ScopeGuardFiresOnExitRespectsDismissAndMove) {
    int fire_count = 0;

    {
        const auto guard = core::make_scope_guard([&] { ++fire_count; });
        EXPECT_EQ(fire_count, 0);
    }
    EXPECT_EQ(fire_count, 1);

    {
        auto dismissed = core::make_scope_guard([&] { ++fire_count; });
        dismissed.dismiss();
    }
    EXPECT_EQ(fire_count, 1);

    {
        auto original = core::make_scope_guard([&] { ++fire_count; });
        auto moved_to = std::move(original);
        EXPECT_EQ(fire_count, 1);
        (void)moved_to;
    }
    EXPECT_EQ(fire_count, 2);

    {
        auto original = core::make_scope_guard([&] { ++fire_count; });
        auto moved_to = std::move(original);
        moved_to.dismiss();
    }
    EXPECT_EQ(fire_count, 2);
}

// Core formatting and range joining utilities

TEST(FormatUtilTest, FormatFixedAndFmtDoubleAreLocaleIndependent) {
    struct FixedCase {
        double value;
        int precision;
        std::string_view expected;
    };
    constexpr std::array<FixedCase, 8> kFixedCases{{
        {0.0, 6, "0.000000"},
        {-12.346, 2, "-12.35"},
        {-12.344, 2, "-12.34"},
        {1.234, 2, "1.23"},
        {1.236, 2, "1.24"},
        {1.4, 0, "1"},
        {1.6, 0, "2"},
        {1.25, 6, "1.250000"},
    }};

    for (const auto& tc : kFixedCases) {
        SCOPED_TRACE(tc.expected);
        const std::string actual = core::format_fixed(tc.value, tc.precision);
        EXPECT_EQ(actual, tc.expected);
        EXPECT_EQ(actual.find(','), std::string::npos);
    }

    struct DoubleCase {
        double value;
        std::string_view expected;
    };
    constexpr std::array<DoubleCase, 4> kDoubleCases{{
        {0.0, "0.000000"},
        {1.5, "1.500000"},
        {-42.125, "-42.125000"},
        {0.000001, "0.000001"},
    }};

    for (const auto& tc : kDoubleCases) {
        SCOPED_TRACE(tc.expected);
        const std::string actual = FFmpegUtils::fmt_double(tc.value);
        EXPECT_EQ(actual, tc.expected);
        EXPECT_EQ(actual.find(','), std::string::npos);
    }
}

TEST(FormatUtilTest, FormatHmsHandlesNegativeZeroBoundariesAndLargeHours) {
    struct HmsCase {
        double seconds;
        std::string_view expected;
    };
    constexpr std::array<HmsCase, 13> kCases{{
        {std::numeric_limits<double>::quiet_NaN(), "00:00:00"},
        {std::numeric_limits<double>::infinity(), "00:00:00"},
        {-std::numeric_limits<double>::infinity(), "00:00:00"},
        {-5.0, "00:00:00"},
        {-0.1, "00:00:00"},
        {0.0, "00:00:00"},
        {0.9, "00:00:00"},
        {59.0, "00:00:59"},
        {60.0, "00:01:00"},
        {3599.0, "00:59:59"},
        {3600.0, "01:00:00"},
        {3661.0, "01:01:01"},
        {360000.0, "100:00:00"},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.expected);
        EXPECT_EQ(core::format_hms(tc.seconds), tc.expected);
    }
}

TEST(FormatUtilTest, FormatZeroPaddedHandlesWidthBoundaries) {
    struct PadCase {
        std::string_view prefix;
        int index;
        int width;
        std::string_view suffix;
        std::string_view expected;
    };
    constexpr std::array<PadCase, 6> kCases{{
        {"frame_", 0, 4, ".jpg", "frame_0000.jpg"},
        {"frame_", 7, 4, ".jpg", "frame_0007.jpg"},
        {"frame_", 9999, 4, ".jpg", "frame_9999.jpg"},
        {"frame_", 10000, 4, ".jpg", "frame_10000.jpg"},
        {"p", 5, 1, "", "p5"},
        {"", 3, 2, "", "03"},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.expected);
        EXPECT_EQ(core::format_zero_padded(tc.prefix, tc.index, tc.width, tc.suffix), tc.expected);
    }
}

TEST(FormatUtilTest, FormatSpeedFormatsSignificantDigits) {
    struct SpeedCase {
        double ratio;
        std::string_view expected;
    };
    constexpr std::array<SpeedCase, 6> kCases{{
        {0.0, "0x"},
        {0.5, "0.5x"},
        {1.0, "1x"},
        {2.34, "2.34x"},
        {12.34, "12.3x"},
        {125.0, "125x"},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.expected);
        EXPECT_EQ(core::format_speed(tc.ratio), tc.expected);
    }
}

TEST(FormatUtilTest, JoinHandlesEmptySingleMultiAndSeparators) {
    const std::span<const std::string_view> empty{};
    EXPECT_EQ(core::join(empty, ","), "");

    constexpr std::array<std::string_view, 1> kSingle{"alpha"};
    EXPECT_EQ(core::join(kSingle, ", "), "alpha");

    constexpr std::array<std::string_view, 3> kMulti{"alpha", "beta", "gamma"};
    EXPECT_EQ(core::join(kMulti, ", "), "alpha, beta, gamma");
    EXPECT_EQ(core::join(kMulti, ""), "alphabetagamma");
    EXPECT_EQ(core::join(kMulti, " :: "), "alpha :: beta :: gamma");
}

// Media filesystem helpers

TEST(MediaFsTest, CurrentTimestampMatchesFormatAndLocalClock) {
    constexpr std::size_t kExpectedLen = 19;
    constexpr double kMaxClockDriftSec = 10.0;

    const std::time_t before = std::time(nullptr);
    const std::string ts = media::current_timestamp();
    const std::time_t after = std::time(nullptr);

    ASSERT_EQ(ts.size(), kExpectedLen);
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[7], '-');
    EXPECT_EQ(ts[10], '_');
    EXPECT_EQ(ts[13], '-');
    EXPECT_EQ(ts[16], '-');

    constexpr std::array<std::size_t, 14> kDigitPositions{
        0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (const std::size_t pos : kDigitPositions) {
        SCOPED_TRACE(pos);
        EXPECT_TRUE(is_ascii_digit(ts[pos]));
    }

    const int year = parse_digits(ts, 0, 4);
    const int month = parse_digits(ts, 5, 2);
    const int day = parse_digits(ts, 8, 2);
    const int hour = parse_digits(ts, 11, 2);
    const int minute = parse_digits(ts, 14, 2);
    const int second = parse_digits(ts, 17, 2);

    EXPECT_GE(year, 2020);
    EXPECT_GE(month, 1);
    EXPECT_LE(month, 12);
    EXPECT_GE(day, 1);
    EXPECT_LE(day, 31);
    EXPECT_GE(hour, 0);
    EXPECT_LE(hour, 23);
    EXPECT_GE(minute, 0);
    EXPECT_LE(minute, 59);
    EXPECT_GE(second, 0);
    EXPECT_LE(second, 59);

    std::tm parsed{};
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    parsed.tm_isdst = -1;
    const std::time_t parsed_time = std::mktime(&parsed);
    ASSERT_NE(parsed_time, static_cast<std::time_t>(-1));

    const double midpoint =
        (static_cast<double>(before) + static_cast<double>(after)) * 0.5;
    EXPECT_LE(std::abs(static_cast<double>(parsed_time) - midpoint), kMaxClockDriftSec);
}

TEST(MediaFsTest, FileDateParsesTimestampAndFallsBackToLastWriteTime) {
    struct DateCase {
        std::string_view filename;
        int year;
        int month;
        int day;
        int hour;
        int minute;
        int second;
    };
    constexpr std::array<DateCase, 3> kCases{{
        {"2026-03-22 00-16-33.mp4", 2026, 3, 22, 0, 16, 33},
        {"2024-02-29 23-59-59.mp4", 2024, 2, 29, 23, 59, 59},
        {"clip_2025-11-05 14-08-09_final.mkv", 2025, 11, 5, 14, 8, 9},
    }};

    test_support::ScopedTempDir sandbox("ut_file_date");

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.filename);
        const fs::path dummy = sandbox.write_bytes(tc.filename, {});
        const std::time_t parsed = media::file_date(dummy);
        EXPECT_EQ(FFmpegUtils::get_file_date(dummy), parsed);

        std::tm local{};
        ASSERT_TRUE(to_local_tm(parsed, local));
        EXPECT_EQ(local.tm_year + 1900, tc.year);
        EXPECT_EQ(local.tm_mon + 1, tc.month);
        EXPECT_EQ(local.tm_mday, tc.day);
        EXPECT_EQ(local.tm_hour, tc.hour);
        EXPECT_EQ(local.tm_min, tc.minute);
        EXPECT_EQ(local.tm_sec, tc.second);
    }

    // Fallback: filename without timestamp uses programmed fs::last_write_time.
    constexpr auto kAge = std::chrono::hours{2};
    constexpr double kToleranceSec = 2.0;
    const fs::path fallback_file = sandbox.write_bytes("unstructured_clip.mp4", {});
    const auto now_sys = std::chrono::system_clock::now();
    const auto target_sys = now_sys - kAge;
    fs::last_write_time(fallback_file, fs::file_time_type::clock::now() - kAge);

    const std::time_t actual_fallback = media::file_date(fallback_file);
    const std::time_t expected_fallback = std::chrono::system_clock::to_time_t(target_sys);
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now_sys);

    EXPECT_LE(std::abs(std::difftime(actual_fallback, expected_fallback)), kToleranceSec);
    EXPECT_GT(std::abs(std::difftime(now_time, actual_fallback)), 3600.0);
}

TEST(MediaFsTest, CollectVideoAndMp4FilesFilterCaseInsensitivelyAndSortChronologically) {
    test_support::ScopedTempDir sandbox("ut_collect_media");
    const fs::path media_dir = sandbox.create_subdir("clips");

    // Deliberately create files with timestamps inverted relative to alphabetical order,
    // plus an equal-timestamp pair to verify deterministic tie-breaking.
    constexpr std::array<std::string_view, 13> kExpectedVideoOrder{
        "z_clip_2025-01-01 08-00-00.MP4",
        "m_clip_2025-01-02 08-00-00.mp4",
        "a_tie_2025-01-03 08-00-00.mp4",
        "b_tie_2025-01-03 08-00-00.mp4",
        "v0_2025-01-03 12-00-00.mkv",
        "v1_2025-01-04 08-00-00.avi",
        "v2_2025-01-05 08-00-00.MKV",
        "v3_2025-01-06 08-00-00.mov",
        "v4_2025-01-07 08-00-00.wmv",
        "v5_2025-01-08 08-00-00.flv",
        "v6_2025-01-09 08-00-00.webm",
        "v7_2025-01-10 08-00-00.ts",
        "v8_2025-01-11 08-00-00.m4v",
    };
    constexpr std::array<std::string_view, 4> kExpectedMp4Order{
        "z_clip_2025-01-01 08-00-00.MP4",
        "m_clip_2025-01-02 08-00-00.mp4",
        "a_tie_2025-01-03 08-00-00.mp4",
        "b_tie_2025-01-03 08-00-00.mp4",
    };

    // Write in reverse order to avoid filesystem insertion order masking sort bugs.
    for (std::size_t i = kExpectedVideoOrder.size(); i > 0; --i) {
        sandbox.write_bytes(std::string("clips/") + std::string(kExpectedVideoOrder[i - 1]), {});
    }
    sandbox.write_text("clips/notes_2025-01-01 00-00-00.txt", "ignore");
    sandbox.write_text("clips/thumb_2025-01-01 00-00-00.jpg", "ignore");
    sandbox.write_text("clips/no_extension", "ignore");
    (void)sandbox.create_subdir("clips/fake_2025-01-01 00-00-00.mp4");

    const auto mp4s = media::collect_mp4_files(media_dir);
    ASSERT_EQ(mp4s.size(), kExpectedMp4Order.size());
    for (std::size_t i = 0; i < kExpectedMp4Order.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(mp4s[i].filename().string(), kExpectedMp4Order[i]);
    }

    const auto videos = media::collect_video_files(media_dir);
    ASSERT_EQ(videos.size(), kExpectedVideoOrder.size());
    for (std::size_t i = 0; i < kExpectedVideoOrder.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(videos[i].filename().string(), kExpectedVideoOrder[i]);
    }

    // Missing path, regular file path, and empty dir all return empty vectors.
    const fs::path missing_dir = sandbox.child("does_not_exist");
    const fs::path regular_file = sandbox.write_bytes("single.mp4", {});
    const fs::path empty_dir = sandbox.create_subdir("empty");

    EXPECT_TRUE(media::collect_mp4_files(missing_dir).empty());
    EXPECT_TRUE(media::collect_video_files(missing_dir).empty());
    EXPECT_TRUE(media::collect_mp4_files(regular_file).empty());
    EXPECT_TRUE(media::collect_video_files(regular_file).empty());
    EXPECT_TRUE(media::collect_mp4_files(empty_dir).empty());
    EXPECT_TRUE(media::collect_video_files(empty_dir).empty());
}

// AV value types, error formatting, capabilities, and probe

TEST(AvFrameRateTest, KnownValueAndEqualityBoundaries) {
    struct RateCase {
        av::FrameRate rate;
        bool expected_known;
        double expected_value;
    };
    constexpr std::array<RateCase, 9> kCases{{
        {{0, 0}, false, -1.0},
        {{30, 0}, false, -1.0},
        {{0, 1}, false, -1.0},
        {{-30, 1}, false, -1.0},
        {{30, -1}, false, -1.0},
        {{-30, -1}, false, -1.0},
        {{1, 1}, true, 1.0},
        {{30, 1}, true, 30.0},
        {{30000, 1001}, true, 29.97002997002997},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.rate.num);
        EXPECT_EQ(tc.rate.known(), tc.expected_known);
        EXPECT_NEAR(tc.rate.value(), tc.expected_value, 1e-9);
    }

    EXPECT_EQ((av::FrameRate{30, 1}), (av::FrameRate{30, 1}));
    EXPECT_NE((av::FrameRate{30, 1}), (av::FrameRate{60, 1}));
    EXPECT_NE((av::FrameRate{30, 1}), (av::FrameRate{30, 2}));
    EXPECT_EQ((av::FrameRate{0, 0}), av::FrameRate{});
}

TEST(AvErrorTest, FormatsDistinctCodesWithNumericSuffix) {
    constexpr std::array<int, 4> kCodes{-22, -2, -12, 0};
    std::array<std::string, kCodes.size()> messages;

    for (std::size_t i = 0; i < kCodes.size(); ++i) {
        const std::string suffix = " (" + std::to_string(kCodes[i]) + ")";
        messages[i] = av::err_str(kCodes[i]);
        SCOPED_TRACE(messages[i]);
        const std::size_t pos = messages[i].rfind(suffix);
        ASSERT_NE(pos, std::string::npos);
        EXPECT_GT(pos, 0u);
        EXPECT_EQ(pos + suffix.size(), messages[i].size());
    }

    for (std::size_t i = 0; i < messages.size(); ++i) {
        for (std::size_t j = i + 1; j < messages.size(); ++j) {
            EXPECT_NE(messages[i], messages[j]);
        }
    }
}

TEST(AvCapabilitiesTest, SoftwareCodecsExistAndUnknownNamesRejected) {
    EXPECT_FALSE(av::encoder_exists("definitely_not_an_encoder"));
    EXPECT_FALSE(av::can_use_encoder("definitely_not_an_encoder"));
    EXPECT_FALSE(av::encoder_exists(""));
    EXPECT_FALSE(av::can_use_encoder(""));

    EXPECT_FALSE(av::can_use_hwaccel("definitely_not_a_hwaccel"));
    EXPECT_FALSE(av::can_use_hwaccel(""));

    EXPECT_TRUE(av::encoder_exists("libx264"));
    EXPECT_TRUE(av::can_use_encoder("libx264"));
    EXPECT_TRUE(av::encoder_exists("aac"));
}

TEST(AvProbeTest, RejectsMissingEmptyAndCorruptFiles) {
    test_support::ScopedTempDir sandbox("ut_av_probe");
    const std::array<fs::path, 3> bad_files{
        sandbox.child("nonexistent.mp4"),
        sandbox.write_bytes("empty.mp4", {}),
        sandbox.write_text("corrupt.mp4", "not a valid media container payload"),
    };

    for (const auto& bad : bad_files) {
        SCOPED_TRACE(bad.filename().string());
        const av::MediaInfo info = av::probe(bad);
        EXPECT_FALSE(info.ok);
        EXPECT_LT(info.duration_sec, 0.0);
        EXPECT_FALSE(info.frame_rate.known());
        EXPECT_FALSE(info.has_video);
        EXPECT_FALSE(info.has_audio);
        EXPECT_EQ(info.width, 0);
        EXPECT_EQ(info.height, 0);
        EXPECT_LT(FFmpegUtils::get_video_duration(bad), 0.0);
    }
}

TEST(AvProbeTest, ReadsRealCaptureMetadata) {
    const fs::path video = test_support::fixture_path(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(video)) << "Missing fixture: " << video.string();

    const av::MediaInfo info = av::probe(video);
    EXPECT_TRUE(info.ok);
    EXPECT_TRUE(info.frame_rate.known());
    EXPECT_NEAR(info.frame_rate.value(), 30.0, 0.01);
    EXPECT_NEAR(info.duration_sec, 16.1, 0.5);
    EXPECT_EQ(info.width, 1920);
    EXPECT_EQ(info.height, 1080);
    EXPECT_TRUE(info.has_video);
    EXPECT_TRUE(info.has_audio);
    EXPECT_EQ(info.video_codec, "hevc");
    EXPECT_EQ(info.audio_codec, "aac");
}

// ToolApp Template Method contract

TEST(ToolAppTest, EnforcesCliSkeletonAndPropagatesExecuteStatus) {
    {
        StubToolApp app;
        EXPECT_EQ(app.run(0, nullptr), 1);
        EXPECT_EQ(app.run(1, nullptr), 1);
        EXPECT_EQ(app.execute_calls, 0);
    }

    {
        StubToolApp app;
        ArgvBuilder args{"stub_tool"};
        const ScopedLogAndCoutCapture capture;
        EXPECT_EQ(app.run(args.argc(), args.argv()), 0);
        EXPECT_EQ(app.define_options_calls, 1);
        EXPECT_GE(app.name_calls, 1);
        EXPECT_EQ(app.extra_help_calls, 1);
        EXPECT_EQ(app.execute_calls, 0);
    }

    {
        StubToolApp app;
        ArgvBuilder args{"stub_tool", "--help"};
        const ScopedLogAndCoutCapture capture;
        EXPECT_EQ(app.run(args.argc(), args.argv()), 0);
        EXPECT_EQ(app.define_options_calls, 1);
        EXPECT_GE(app.name_calls, 1);
        EXPECT_EQ(app.extra_help_calls, 1);
        EXPECT_EQ(app.execute_calls, 0);
        const std::string help_text = capture.combined();
        EXPECT_NE(help_text.find("stub_tool"), std::string::npos);
        EXPECT_NE(help_text.find("--tag"), std::string::npos);
        EXPECT_NE(help_text.find("Stub extra guidance."), std::string::npos);
    }

    {
        StubToolApp app;
        ArgvBuilder args{"stub_tool", "-h"};
        const ScopedLogAndCoutCapture capture;
        EXPECT_EQ(app.run(args.argc(), args.argv()), 0);
        EXPECT_EQ(app.define_options_calls, 1);
        EXPECT_EQ(app.extra_help_calls, 1);
        EXPECT_EQ(app.execute_calls, 0);
    }

    {
        StubToolApp app;
        ArgvBuilder args{"stub_tool", "--unknown-option"};
        EXPECT_EQ(app.run(args.argc(), args.argv()), 1);
        EXPECT_EQ(app.execute_calls, 0);
    }

    {
        StubToolApp app;
        app.return_code = 0;
        ArgvBuilder args{"stub_tool", "--tag", "alpha"};
        EXPECT_EQ(app.run(args.argc(), args.argv()), 0);
        EXPECT_EQ(app.execute_calls, 1);
        EXPECT_EQ(app.captured_tag, "alpha");
    }

    {
        StubToolApp app;
        app.return_code = 42;
        ArgvBuilder args{"stub_tool", "-t", "beta"};
        EXPECT_EQ(app.run(args.argc(), args.argv()), 42);
        EXPECT_EQ(app.execute_calls, 1);
        EXPECT_EQ(app.captured_tag, "beta");
    }
}
