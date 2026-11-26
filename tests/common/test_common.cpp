#include "common.h"
#include "av/av_capabilities.h"
#include "av/av_error.h"
#include "av/bgr_converter.h"
#include "av/frame_sampler.h"
#include "av/media_probe.h"
#include "core/arch.h"
#include "core/concepts.h"
#include "core/format_util.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"
#include "media/media_fs.h"
#include "platform/locale_guard.h"
#include "platform/time_util.h"
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
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
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
    const std::optional<std::tm> converted = platform::local_time(value);
    if (!converted.has_value()) {
        return false;
    }
    out = *converted;
    return true;
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

// FrameSampler must be move-only with noexcept move operations.
static_assert(!std::is_copy_constructible_v<av::FrameSampler>);
static_assert(!std::is_copy_assignable_v<av::FrameSampler>);
static_assert(std::is_nothrow_move_constructible_v<av::FrameSampler>);
static_assert(std::is_nothrow_move_assignable_v<av::FrameSampler>);

constexpr int kBgr24BytesPerPixel = 3;

[[nodiscard]] std::string bytes_to_string(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::uint8_t byte : bytes) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

void expect_utf8_bytes(const std::string& actual,
                       std::span<const std::uint8_t> expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(static_cast<std::uint8_t>(static_cast<unsigned char>(actual[i])),
                  expected[i]);
    }
}

void verify_frame_invariants(const av::FrameBuffer& fb,
                             int expected_width,
                             int expected_height) {
    ASSERT_GT(expected_width, 0);
    ASSERT_GT(expected_height, 0);

    const int expected_stride = expected_width * kBgr24BytesPerPixel;
    const std::size_t expected_size =
        static_cast<std::size_t>(expected_height) * static_cast<std::size_t>(expected_stride);

    EXPECT_FALSE(fb.empty());
    EXPECT_EQ(fb.width, expected_width);
    EXPECT_EQ(fb.height, expected_height);
    EXPECT_EQ(fb.stride, expected_stride);
    ASSERT_EQ(fb.bgr24.size(), expected_size);
    EXPECT_TRUE(std::isfinite(fb.timestamp_sec));

    const auto [min_it, max_it] = std::ranges::minmax_element(fb.bgr24);
    EXPECT_LT(*min_it, *max_it);
}

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

// Core UTF-8 <-> std::filesystem::path conversions

TEST(CorePathUtilTest, ToUtf8AndFromUtf8MatchRfc3629ByteMatrix) {
    struct PathUtf8Case {
        std::string_view label;
        std::u8string_view u8_input;
        std::span<const std::uint8_t> expected_bytes;
    };

    static constexpr std::array<std::uint8_t, 11> kAsciiBytes{
        0x63u, 0x6Cu, 0x69u, 0x70u, 0x5Fu, 0x30u, 0x31u, 0x2Eu, 0x6Du, 0x70u, 0x34u};
    static constexpr std::array<std::uint8_t, 5> kAccentedLatinBytes{
        0x63u, 0x61u, 0x66u, 0xC3u, 0xA9u};
    static constexpr std::array<std::uint8_t, 6> kCjkBytes{
        0xE5u, 0x8Bu, 0x95u, 0xE7u, 0x94u, 0xBBu};
    static constexpr std::array<std::uint8_t, 4> kEmojiBytes{
        0xF0u, 0x9Fu, 0x8Eu, 0xACu};
    static constexpr std::array<std::uint8_t, 21> kMixedBytes{
        0x63u, 0x61u, 0x66u, 0xC3u, 0xA9u, 0x20u,
        0xE5u, 0x8Bu, 0x95u, 0xE7u, 0x94u, 0xBBu, 0x20u,
        0xF0u, 0x9Fu, 0x8Eu, 0xACu, 0x2Eu, 0x6Du, 0x70u, 0x34u};

    constexpr std::array<PathUtf8Case, 6> kCases{{
        {"empty", u8"", std::span<const std::uint8_t>{}},
        {"ascii", u8"clip_01.mp4", std::span<const std::uint8_t>{kAsciiBytes}},
        {"two_byte_latin", u8"caf\u00E9", std::span<const std::uint8_t>{kAccentedLatinBytes}},
        {"three_byte_cjk", u8"\u52D5\u753B", std::span<const std::uint8_t>{kCjkBytes}},
        {"four_byte_emoji", u8"\U0001F3AC", std::span<const std::uint8_t>{kEmojiBytes}},
        {"mixed_unicode",
         u8"caf\u00E9 \u52D5\u753B \U0001F3AC.mp4",
         std::span<const std::uint8_t>{kMixedBytes}},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const fs::path native_path{tc.u8_input};
        const std::string encoded = core::to_utf8(native_path);
        expect_utf8_bytes(encoded, tc.expected_bytes);

        const std::string raw_utf8 = bytes_to_string(tc.expected_bytes);
        const fs::path decoded = core::from_utf8(raw_utf8);
        EXPECT_EQ(decoded, native_path);
        expect_utf8_bytes(core::to_utf8(decoded), tc.expected_bytes);
    }
}

TEST(CorePathUtilTest, NestedUnicodePathPreservesComponentsAndFilesystemRoundTrip) {
    constexpr std::u8string_view kDirU8 = u8"caf\u00E9_dir";
    constexpr std::u8string_view kSubU8 = u8"\u52D5\u753B_sub";
    constexpr std::u8string_view kFileU8 = u8"clip_\U0001F3AC.bin";

    constexpr std::array<std::uint8_t, 9> kDirBytes{
        0x63u, 0x61u, 0x66u, 0xC3u, 0xA9u, 0x5Fu, 0x64u, 0x69u, 0x72u};
    constexpr std::array<std::uint8_t, 10> kSubBytes{
        0xE5u, 0x8Bu, 0x95u, 0xE7u, 0x94u, 0xBBu, 0x5Fu, 0x73u, 0x75u, 0x62u};
    constexpr std::array<std::uint8_t, 13> kFileBytes{
        0x63u, 0x6Cu, 0x69u, 0x70u, 0x5Fu, 0xF0u, 0x9Fu, 0x8Eu, 0xACu,
        0x2Eu, 0x62u, 0x69u, 0x6Eu};
    constexpr std::array<std::uint8_t, 8> kPayloadBytes{
        0x00u, 0x46u, 0x46u, 0x7Fu, 0x80u, 0xC3u, 0xE5u, 0xFFu};

    const fs::path nested_rel = fs::path{kDirU8} / fs::path{kSubU8} / fs::path{kFileU8};
    const fs::path round_tripped_rel = core::from_utf8(core::to_utf8(nested_rel));
    expect_utf8_bytes(core::to_utf8(round_tripped_rel.filename()), kFileBytes);
    expect_utf8_bytes(core::to_utf8(round_tripped_rel.parent_path().filename()), kSubBytes);
    expect_utf8_bytes(
        core::to_utf8(round_tripped_rel.parent_path().parent_path().filename()), kDirBytes);

    const std::string generic_utf8 =
        bytes_to_string(kDirBytes) + "/" + bytes_to_string(kSubBytes) + "/" +
        bytes_to_string(kFileBytes);
    const fs::path parsed_nested = core::from_utf8(generic_utf8);
    expect_utf8_bytes(core::to_utf8(parsed_nested.filename()), kFileBytes);
    expect_utf8_bytes(core::to_utf8(parsed_nested.parent_path().filename()), kSubBytes);
    expect_utf8_bytes(
        core::to_utf8(parsed_nested.parent_path().parent_path().filename()), kDirBytes);

    test_support::ScopedTempDir sandbox("ut_path_util");
    const fs::path full_path = sandbox.path() / nested_rel;
    const fs::path reopened_path = core::from_utf8(core::to_utf8(full_path));

    std::error_code ec;
    ASSERT_TRUE(fs::create_directories(reopened_path.parent_path(), ec));
    ASSERT_FALSE(ec) << ec.message();

    {
        std::ofstream out(reopened_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        for (const std::uint8_t byte : kPayloadBytes) {
            out.put(static_cast<char>(byte));
        }
        out.flush();
        ASSERT_TRUE(out.good());
    }

    EXPECT_TRUE(fs::is_regular_file(full_path));
    EXPECT_TRUE(fs::is_regular_file(reopened_path));
    expect_utf8_bytes(core::to_utf8(reopened_path.filename()), kFileBytes);
    expect_utf8_bytes(core::to_utf8(reopened_path.parent_path().filename()), kSubBytes);
    expect_utf8_bytes(
        core::to_utf8(reopened_path.parent_path().parent_path().filename()), kDirBytes);

    const std::vector<std::byte> actual_bytes =
        test_support::read_file_bytes(core::from_utf8(core::to_utf8(full_path)));
    ASSERT_EQ(actual_bytes.size(), kPayloadBytes.size());
    for (std::size_t i = 0; i < kPayloadBytes.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(static_cast<std::uint8_t>(actual_bytes[i]), kPayloadBytes[i]);
    }
}

// Platform abstractions (thread-safe locale guard and local calendar time)

TEST(PlatformLocaleGuardTest, PinsDecimalFormattingSupportsMoveNestingAndConcurrentThreads) {
    static_assert(!std::is_copy_constructible_v<platform::ScopedCNumericLocale>);
    static_assert(!std::is_copy_assignable_v<platform::ScopedCNumericLocale>);
    static_assert(std::is_nothrow_move_constructible_v<platform::ScopedCNumericLocale>);
    static_assert(std::is_nothrow_move_assignable_v<platform::ScopedCNumericLocale>);
    static_assert(std::is_nothrow_destructible_v<platform::ScopedCNumericLocale>);

    constexpr std::size_t kFormatBufLen = 32;
    constexpr double kSampleValue = 1.25;
    constexpr double kParsedExpected = 3.5;
    constexpr std::string_view kExpectedFormatted = "1.25";
    constexpr std::string_view kCommaFormatted = "1,25";
    constexpr std::size_t kWorkerCount = 4;
    constexpr std::size_t kIterationsPerWorker = 32;

    const auto format_sample = [&]() -> std::string {
        std::array<char, kFormatBufLen> buf{};
        const int written = std::snprintf(buf.data(), buf.size(), "%.2f", kSampleValue);
        if (written <= 0 || static_cast<std::size_t>(written) >= buf.size()) {
            return {};
        }
        return std::string(buf.data(), static_cast<std::size_t>(written));
    };

    const auto parse_c_double = [](const char* text, double& out) -> bool {
        char* end = nullptr;
        out = std::strtod(text, &end);
        return end != nullptr && *end == '\0';
    };

    {
        platform::ScopedCNumericLocale guard;
        EXPECT_EQ(format_sample(), kExpectedFormatted);
        double parsed = 0.0;
        ASSERT_TRUE(parse_c_double("3.5", parsed));
        EXPECT_DOUBLE_EQ(parsed, kParsedExpected);

        platform::ScopedCNumericLocale moved_ctor(std::move(guard));
        EXPECT_EQ(format_sample(), kExpectedFormatted);

        platform::ScopedCNumericLocale moved_assign;
        moved_assign = std::move(moved_ctor);
        EXPECT_EQ(format_sample(), kExpectedFormatted);

        {
            const platform::ScopedCNumericLocale inner;
            EXPECT_EQ(format_sample(), kExpectedFormatted);
        }
        EXPECT_EQ(format_sample(), kExpectedFormatted);
    }

    // Concurrent threads entering and exiting ScopedCNumericLocale must remain race-free.
    std::atomic<int> worker_failures{0};
    {
        std::vector<std::jthread> workers;
        workers.reserve(kWorkerCount);
        for (std::size_t t = 0; t < kWorkerCount; ++t) {
            workers.emplace_back([&]() {
                for (std::size_t i = 0; i < kIterationsPerWorker; ++i) {
                    const platform::ScopedCNumericLocale scoped;
                    if (format_sample() != kExpectedFormatted) {
                        worker_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    double parsed = 0.0;
                    if (!parse_c_double("3.5", parsed) || parsed != kParsedExpected) {
                        worker_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }
    EXPECT_EQ(worker_failures.load(std::memory_order_relaxed), 0);

    // If a comma-decimal locale exists on the host, verify per-thread pinning and restoration
    // on an isolated thread while restoring the process-global numeric locale on scope exit.
    const std::string saved_global_locale = []() -> std::string {
        const char* cur = std::setlocale(LC_NUMERIC, nullptr);
        return cur != nullptr ? std::string(cur) : std::string("C");
    }();
    const auto restore_global_locale = core::make_scope_guard([&]() {
        (void)std::setlocale(LC_NUMERIC, saved_global_locale.c_str());
    });

    std::atomic<bool> comma_restore_mismatch{false};
    {
        std::jthread locale_thread([&]() {
            constexpr std::array<const char*, 3> kCommaLocales{
                "de_DE.UTF-8", "de-DE", "fr_FR.UTF-8"};
            const bool installed_comma = [&]() {
                for (const char* name : kCommaLocales) {
                    if (std::setlocale(LC_NUMERIC, name) != nullptr &&
                        format_sample() == kCommaFormatted) {
                        return true;
                    }
                }
                return false;
            }();
            if (!installed_comma) {
                return;
            }
            {
                const platform::ScopedCNumericLocale guard;
                if (format_sample() != kExpectedFormatted) {
                    comma_restore_mismatch.store(true, std::memory_order_relaxed);
                }
            }
            if (format_sample() != kCommaFormatted) {
                comma_restore_mismatch.store(true, std::memory_order_relaxed);
            }
        });
    }
    EXPECT_FALSE(comma_restore_mismatch.load(std::memory_order_relaxed));
}

TEST(PlatformTimeUtilTest, ConvertsCalendarDatesRejectsExtremeBoundsAndIsThreadSafe) {
    struct CalendarCase {
        std::string_view label;
        int year;
        int month;
        int day;
        int hour;
        int minute;
        int second;
    };
    constexpr std::array<CalendarCase, 3> kCases{{
        {"y2k_midnight", 2000, 1, 1, 0, 0, 0},
        {"leap_day_end", 2024, 2, 29, 23, 59, 59},
        {"capture_date", 2026, 3, 22, 14, 35, 9},
    }};
    constexpr int kTmYearBase = 1900;
    constexpr int kEpochMinTmYear = 69;
    constexpr int kEpochMaxTmYear = 70;
    constexpr int kMaxMonthIndex = 11;
    constexpr int kMaxDayOfMonth = 31;
    constexpr int kMaxHour = 23;
    constexpr int kMaxMinute = 59;
    constexpr int kMaxSecond = 59;

    std::array<std::time_t, kCases.size()> timestamps{};
    for (std::size_t i = 0; i < kCases.size(); ++i) {
        const auto& tc = kCases[i];
        SCOPED_TRACE(tc.label);
        std::tm input{};
        input.tm_year = tc.year - kTmYearBase;
        input.tm_mon = tc.month - 1;
        input.tm_mday = tc.day;
        input.tm_hour = tc.hour;
        input.tm_min = tc.minute;
        input.tm_sec = tc.second;
        input.tm_isdst = -1;

        const std::time_t epoch_sec = std::mktime(&input);
        ASSERT_NE(epoch_sec, static_cast<std::time_t>(-1));
        timestamps[i] = epoch_sec;

        const std::optional<std::tm> result = platform::local_time(epoch_sec);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->tm_year + kTmYearBase, tc.year);
        EXPECT_EQ(result->tm_mon + 1, tc.month);
        EXPECT_EQ(result->tm_mday, tc.day);
        EXPECT_EQ(result->tm_hour, tc.hour);
        EXPECT_EQ(result->tm_min, tc.minute);
        EXPECT_EQ(result->tm_sec, tc.second);
    }

    const std::optional<std::tm> epoch_zero = platform::local_time(std::time_t{0});
    ASSERT_TRUE(epoch_zero.has_value());
    EXPECT_GE(epoch_zero->tm_year, kEpochMinTmYear);
    EXPECT_LE(epoch_zero->tm_year, kEpochMaxTmYear);
    EXPECT_GE(epoch_zero->tm_mon, 0);
    EXPECT_LE(epoch_zero->tm_mon, kMaxMonthIndex);
    EXPECT_GE(epoch_zero->tm_mday, 1);
    EXPECT_LE(epoch_zero->tm_mday, kMaxDayOfMonth);
    EXPECT_GE(epoch_zero->tm_hour, 0);
    EXPECT_LE(epoch_zero->tm_hour, kMaxHour);
    EXPECT_GE(epoch_zero->tm_min, 0);
    EXPECT_LE(epoch_zero->tm_min, kMaxMinute);
    EXPECT_GE(epoch_zero->tm_sec, 0);
    EXPECT_LE(epoch_zero->tm_sec, kMaxSecond);

    constexpr std::size_t kMin64BitTimeTBytes = 8U;
    if constexpr (sizeof(std::time_t) >= kMin64BitTimeTBytes) {
        EXPECT_FALSE(platform::local_time(std::numeric_limits<std::time_t>::min()).has_value());
        EXPECT_FALSE(platform::local_time(std::numeric_limits<std::time_t>::max()).has_value());
    }

    // Concurrent calls with distinct timestamps must not clobber each other.
    constexpr std::size_t kIterationsPerThread = 64;
    std::atomic<int> concurrent_mismatches{0};
    {
        std::vector<std::jthread> workers;
        workers.reserve(kCases.size());
        for (std::size_t idx = 0; idx < kCases.size(); ++idx) {
            workers.emplace_back([&, idx]() {
                const auto& expected = kCases[idx];
                const std::time_t ts = timestamps[idx];
                for (std::size_t iter = 0; iter < kIterationsPerThread; ++iter) {
                    const std::optional<std::tm> actual = platform::local_time(ts);
                    if (!actual.has_value() ||
                        (actual->tm_year + kTmYearBase) != expected.year ||
                        (actual->tm_mon + 1) != expected.month ||
                        actual->tm_mday != expected.day ||
                        actual->tm_hour != expected.hour ||
                        actual->tm_min != expected.minute ||
                        actual->tm_sec != expected.second) {
                        concurrent_mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }
    EXPECT_EQ(concurrent_mismatches.load(std::memory_order_relaxed), 0);
}

// Architecture-isolated BGRA->BGR24 pixel converter

TEST(AvBgrConverterTest, StripBgraToBgr24HandlesEmptySignBitPixelsAndLengthMismatchSeams) {
    static_assert(av::BgrConvertPolicy<av::DirectBgr24Policy>);
    static_assert(av::BgrConvertPolicy<av::BgraBridgePolicy>);
    static_assert(av::BgrConvertPolicy<av::DefaultBgrConvertPolicy>);
    static_assert(!av::BgrConvertPolicy<int>);
    static_assert(std::is_same_v<
                  av::DefaultBgrConvertPolicy,
                  std::conditional_t<core::arch::kIsArm64,
                                     av::BgraBridgePolicy,
                                     av::DirectBgr24Policy>>);

    // Empty 0 -> 0 succeeds cleanly.
    EXPECT_TRUE(av::strip_bgra_to_bgr24(std::span<const std::uint8_t>{},
                                        std::span<std::uint8_t>{}));

    // Pinned 3-pixel vector exercising sign-bit bytes (0x00, 0x7F, 0x80, 0xFF) and distinct alphas.
    constexpr std::array<std::uint8_t, 12> kBgraInput{{
        0x00u, 0x7Fu, 0x80u, 0x11u,
        0xFFu, 0x01u, 0xFEu, 0xAAu,
        0x12u, 0x34u, 0x56u, 0xFFu,
    }};
    constexpr std::array<std::uint8_t, 9> kExpectedBgr24{{
        0x00u, 0x7Fu, 0x80u,
        0xFFu, 0x01u, 0xFEu,
        0x12u, 0x34u, 0x56u,
    }};
    std::array<std::uint8_t, kExpectedBgr24.size()> bgr_out{};
    ASSERT_TRUE(av::strip_bgra_to_bgr24(kBgraInput, bgr_out));
    EXPECT_EQ(bgr_out, kExpectedBgr24);

    // Length-mismatch boundary seams must return false without mutating destination bytes.
    constexpr std::uint8_t kSrcFillByte = 0xCCu;
    constexpr std::uint8_t kSentinelByte = 0x5Au;
    struct SizeMismatchCase {
        std::string_view label;
        std::size_t bgra_len;
        std::size_t bgr_len;
    };
    constexpr std::array<SizeMismatchCase, 7> kMismatchCases{{
        {"bgra_len_1_not_div4", 1, 0},
        {"bgra_len_3_not_div4", 3, 0},
        {"bgra_len_5_not_div4", 5, 3},
        {"bgr_too_short", 8, 5},
        {"bgr_too_long", 8, 7},
        {"empty_src_nonempty_dst", 0, 3},
        {"nonempty_src_empty_dst", 4, 0},
    }};

    for (const auto& tc : kMismatchCases) {
        SCOPED_TRACE(tc.label);
        const std::vector<std::uint8_t> src(tc.bgra_len, kSrcFillByte);
        std::vector<std::uint8_t> dst(tc.bgr_len, kSentinelByte);
        EXPECT_FALSE(av::strip_bgra_to_bgr24(src, dst));
        EXPECT_TRUE(std::ranges::all_of(dst, [](std::uint8_t b) { return b == kSentinelByte; }));
    }
}

TEST(AvBgrConverterTest, DirectAndBgraBridgePoliciesDispatchAndHandleErrorsOnAllArchitectures) {
    constexpr int kWidth = 2;
    constexpr int kHeight = 2;
    constexpr int kDirectStride = kWidth * static_cast<int>(av::kBgr24BytesPerPixel);
    constexpr int kBgraStride = kWidth * static_cast<int>(av::kBgraBytesPerPixel);
    constexpr std::size_t kOutBytes =
        static_cast<std::size_t>(kWidth * kHeight) * av::kBgr24BytesPerPixel;
    constexpr std::uint8_t kSentinel = 0xA5u;
    constexpr int kOverflowWidth =
        (std::numeric_limits<int>::max() / static_cast<int>(av::kBgraBytesPerPixel)) + 1;

    constexpr std::array<std::uint8_t, 16> kFakeBgraPixels{{
        0x10u, 0x20u, 0x30u, 0xFFu,
        0x40u, 0x50u, 0x60u, 0x80u,
        0x7Fu, 0x80u, 0x00u, 0x01u,
        0xF0u, 0xE1u, 0xD2u, 0x00u,
    }};
    constexpr std::array<std::uint8_t, 12> kExpectedStrippedBgr{{
        0x10u, 0x20u, 0x30u,
        0x40u, 0x50u, 0x60u,
        0x7Fu, 0x80u, 0x00u,
        0xF0u, 0xE1u, 0xD2u,
    }};

    // Both DirectBgr24Policy and BgraBridgePolicy reject invalid dimensions/overflow/size.
    auto check_invalid_args = [](auto& converter) {
        int calls = 0;
        const auto fail_if_called = [&](bool, std::uint8_t*, int) {
            ++calls;
            return true;
        };
        std::array<std::uint8_t, kOutBytes> out{};
        EXPECT_FALSE(converter.convert(0, kHeight, true, out, fail_if_called));
        EXPECT_FALSE(converter.convert(kWidth, 0, true, out, fail_if_called));
        EXPECT_FALSE(converter.convert(-1, kHeight, false, out, fail_if_called));
        EXPECT_FALSE(converter.convert(kOverflowWidth, 1, true, out, fail_if_called));
        std::array<std::uint8_t, kOutBytes - 1U> short_out{};
        EXPECT_FALSE(converter.convert(kWidth, kHeight, true, short_out, fail_if_called));
        EXPECT_EQ(calls, 0);
    };

    // DirectBgr24Policy always requests direct BGR24 regardless of is_yuv420p.
    {
        av::BgrConverter<av::DirectBgr24Policy> direct;
        check_invalid_args(direct);

        for (const bool is_yuv420p : {true, false}) {
            SCOPED_TRACE(is_yuv420p);
            std::array<std::uint8_t, kOutBytes> out{};
            bool saw_bgra = true;
            std::uint8_t* saw_dst = nullptr;
            int saw_stride = 0;

            ASSERT_TRUE(direct.convert(
                kWidth, kHeight, is_yuv420p, out,
                [&](bool to_bgra, std::uint8_t* dst, int stride) {
                    saw_bgra = to_bgra;
                    saw_dst = dst;
                    saw_stride = stride;
                    std::ranges::copy(kExpectedStrippedBgr, dst);
                    return true;
                }));
            EXPECT_FALSE(saw_bgra);
            EXPECT_EQ(saw_dst, out.data());
            EXPECT_EQ(saw_stride, kDirectStride);
            EXPECT_EQ(out, kExpectedStrippedBgr);
        }

        std::array<std::uint8_t, kOutBytes> out{};
        EXPECT_FALSE(direct.convert(kWidth, kHeight, true, out,
                                    [](bool, std::uint8_t*, int) { return false; }));
        direct.clear();
    }

    // BgraBridgePolicy routes YUV420P through BGRA scratch + alpha strip, and non-YUV420P direct.
    {
        av::BgrConverter<av::BgraBridgePolicy> bridge;
        check_invalid_args(bridge);

        std::array<std::uint8_t, kOutBytes> out{};
        bool saw_bgra = false;
        std::uint8_t* saw_dst = nullptr;
        int saw_stride = 0;
        ASSERT_TRUE(bridge.convert(
            kWidth, kHeight, true, out,
            [&](bool to_bgra, std::uint8_t* dst, int stride) {
                saw_bgra = to_bgra;
                saw_dst = dst;
                saw_stride = stride;
                std::ranges::copy(kFakeBgraPixels, dst);
                return true;
            }));
        EXPECT_TRUE(saw_bgra);
        EXPECT_NE(saw_dst, out.data());
        EXPECT_EQ(saw_stride, kBgraStride);
        EXPECT_EQ(out, kExpectedStrippedBgr);

        // Non-YUV420P bypasses the BGRA bridge and writes directly to out_bgr24.
        out.fill(0U);
        ASSERT_TRUE(bridge.convert(
            kWidth, kHeight, false, out,
            [&](bool to_bgra, std::uint8_t* dst, int stride) {
                saw_bgra = to_bgra;
                saw_dst = dst;
                saw_stride = stride;
                std::ranges::copy(kExpectedStrippedBgr, dst);
                return true;
            }));
        EXPECT_FALSE(saw_bgra);
        EXPECT_EQ(saw_dst, out.data());
        EXPECT_EQ(saw_stride, kDirectStride);
        EXPECT_EQ(out, kExpectedStrippedBgr);

        // Valid -> Failing -> clear() -> Valid (larger 3x2) state seam.
        out.fill(kSentinel);
        EXPECT_FALSE(bridge.convert(kWidth, kHeight, true, out,
                                    [](bool, std::uint8_t*, int) { return false; }));
        EXPECT_TRUE(std::ranges::all_of(out, [](std::uint8_t b) { return b == kSentinel; }));

        bridge.clear();
        constexpr int kLargerWidth = 3;
        constexpr int kLargerHeight = 2;
        constexpr std::size_t kLargerPixels =
            static_cast<std::size_t>(kLargerWidth * kLargerHeight);
        constexpr std::size_t kBlueOffset = 1U;
        constexpr std::size_t kGreenOffset = 10U;
        constexpr std::size_t kRedOffset = 20U;
        constexpr std::uint8_t kOpaqueAlpha = 0xFFu;
        constexpr std::size_t kBlueChannel = 0U;
        constexpr std::size_t kGreenChannel = 1U;
        constexpr std::size_t kRedChannel = 2U;
        constexpr std::size_t kAlphaChannel = 3U;
        std::vector<std::uint8_t> larger_out(kLargerPixels * av::kBgr24BytesPerPixel, 0U);
        ASSERT_TRUE(bridge.convert(
            kLargerWidth, kLargerHeight, true, larger_out,
            [](bool to_bgra, std::uint8_t* dst, int stride) {
                if (!to_bgra || stride != kLargerWidth * static_cast<int>(av::kBgraBytesPerPixel)) {
                    return false;
                }
                for (std::size_t p = 0; p < kLargerPixels; ++p) {
                    const std::size_t base = p * av::kBgraBytesPerPixel;
                    dst[base + kBlueChannel] = static_cast<std::uint8_t>(p + kBlueOffset);
                    dst[base + kGreenChannel] = static_cast<std::uint8_t>(p + kGreenOffset);
                    dst[base + kRedChannel] = static_cast<std::uint8_t>(p + kRedOffset);
                    dst[base + kAlphaChannel] = kOpaqueAlpha;
                }
                return true;
            }));
        ASSERT_EQ(larger_out.size(), kLargerPixels * av::kBgr24BytesPerPixel);
        for (std::size_t p = 0; p < kLargerPixels; ++p) {
            SCOPED_TRACE(p);
            const std::size_t base = p * av::kBgr24BytesPerPixel;
            EXPECT_EQ(larger_out[base + kBlueChannel],
                      static_cast<std::uint8_t>(p + kBlueOffset));
            EXPECT_EQ(larger_out[base + kGreenChannel],
                      static_cast<std::uint8_t>(p + kGreenOffset));
            EXPECT_EQ(larger_out[base + kRedChannel],
                      static_cast<std::uint8_t>(p + kRedOffset));
        }
    }
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

// AV reusable video frame sampler

TEST(AvFrameSamplerTest, DefaultStateAndInvalidFilesStayClosed) {
    constexpr int kSampleWidth = 2;
    constexpr int kSampleHeight = 1;
    constexpr int kSampleStride = kSampleWidth * kBgr24BytesPerPixel;
    constexpr std::size_t kSamplePixelBytes =
        static_cast<std::size_t>(kSampleHeight) * static_cast<std::size_t>(kSampleStride);
    constexpr std::uint8_t kSamplePixelValue = 0x7Fu;
    constexpr std::array<double, 2> kProbeTimestamps{0.5, 2.0};

    // FrameBuffer::empty() boundary predicate matrix across all 4 fields.
    const std::vector<std::uint8_t> populated_pixels(kSamplePixelBytes, kSamplePixelValue);
    struct BufferEmptyCase {
        std::string_view label;
        int width;
        int height;
        int stride;
        bool has_pixels;
        bool expected_empty;
    };
    constexpr std::array<BufferEmptyCase, 6> kBufferCases{{
        {"default_zeroed", 0, 0, 0, false, true},
        {"zero_width", 0, kSampleHeight, kSampleStride, true, true},
        {"zero_height", kSampleWidth, 0, kSampleStride, true, true},
        {"zero_stride", kSampleWidth, kSampleHeight, 0, true, true},
        {"empty_pixels", kSampleWidth, kSampleHeight, kSampleStride, false, true},
        {"valid_bgr24", kSampleWidth, kSampleHeight, kSampleStride, true, false},
    }};
    for (const auto& tc : kBufferCases) {
        SCOPED_TRACE(tc.label);
        av::FrameBuffer fb;
        fb.width = tc.width;
        fb.height = tc.height;
        fb.stride = tc.stride;
        if (tc.has_pixels) {
            fb.bgr24 = populated_pixels;
        }
        EXPECT_EQ(fb.empty(), tc.expected_empty);
    }

    av::FrameSampler sampler;
    EXPECT_FALSE(sampler.is_open());
    EXPECT_EQ(sampler.width(), 0);
    EXPECT_EQ(sampler.height(), 0);
    EXPECT_LE(sampler.duration_sec(), 0.0);
    EXPECT_FALSE(sampler.sample_at(0.0).has_value());

    std::size_t callback_calls = 0;
    EXPECT_EQ(sampler.sample_timestamps(
                  kProbeTimestamps,
                  [&](std::size_t, av::FrameBuffer&&) { ++callback_calls; }),
              0u);
    EXPECT_EQ(callback_calls, 0u);

    test_support::ScopedTempDir sandbox("ut_av_frame_sampler");
    const std::array<fs::path, 3> bad_files{
        sandbox.child("nonexistent_sampler.mp4"),
        sandbox.write_bytes("empty_sampler.mp4", {}),
        sandbox.write_text("corrupt_sampler.mp4", "not a valid media container payload"),
    };

    for (const auto& bad : bad_files) {
        SCOPED_TRACE(core::to_utf8(bad.filename()));
        EXPECT_FALSE(sampler.open(bad));
        EXPECT_FALSE(sampler.is_open());
        EXPECT_EQ(sampler.width(), 0);
        EXPECT_EQ(sampler.height(), 0);
        EXPECT_LE(sampler.duration_sec(), 0.0);
        EXPECT_FALSE(sampler.sample_at(0.0).has_value());
    }
}

TEST(AvFrameSamplerTest, SamplesFixtureAndUnicodePathWithValidErrorValidSeam) {
    constexpr double kDurationToleranceSec = 0.5;
    constexpr double kEarlyTimestampSec = 0.5;
    constexpr double kMidTimestampSec = 2.0;
    constexpr double kLateTimestampSec = 8.0;
    constexpr double kNegativeTimestampSec = -1.0;
    constexpr double kPastEofOffsetSec = 10.0;
    constexpr std::u8string_view kUnicodeRelPath =
        u8"sampler_caf\u00E9_\u52D5\u753B/clip_\u52D5\u753B.mp4";

    const fs::path fixture = test_support::fixture_path(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture)) << "Missing fixture: " << core::to_utf8(fixture);

    const av::MediaInfo probe = av::probe(fixture);
    ASSERT_TRUE(probe.ok);
    ASSERT_GT(probe.width, 0);
    ASSERT_GT(probe.height, 0);
    ASSERT_GT(probe.duration_sec, 0.0);

    test_support::ScopedTempDir sandbox("ut_frame_sampler_unicode");
    const fs::path unicode_copy = sandbox.copy_in(fixture, kUnicodeRelPath);
    ASSERT_TRUE(fs::is_regular_file(unicode_copy));

    struct PathCase {
        std::string_view label;
        fs::path video_path;
    };
    const std::array<PathCase, 2> paths{{
        {"committed_fixture", fixture},
        {"unicode_copy", unicode_copy},
    }};

    for (const auto& path_case : paths) {
        SCOPED_TRACE(path_case.label);
        av::FrameSampler sampler;
        ASSERT_TRUE(sampler.open(path_case.video_path));
        EXPECT_TRUE(sampler.is_open());
        EXPECT_EQ(sampler.width(), probe.width);
        EXPECT_EQ(sampler.height(), probe.height);
        EXPECT_NEAR(sampler.duration_sec(), probe.duration_sec, kDurationToleranceSec);

        // Valid decode before injecting out-of-range / non-finite timestamps.
        const auto early = sampler.sample_at(kEarlyTimestampSec);
        ASSERT_TRUE(early.has_value());
        verify_frame_invariants(*early, probe.width, probe.height);

        struct InvalidTimestampCase {
            std::string_view label;
            double timestamp_sec;
        };
        const std::array<InvalidTimestampCase, 5> invalid_timestamps{{
            {"negative", kNegativeTimestampSec},
            {"quiet_nan", std::numeric_limits<double>::quiet_NaN()},
            {"pos_inf", std::numeric_limits<double>::infinity()},
            {"neg_inf", -std::numeric_limits<double>::infinity()},
            {"past_eof", sampler.duration_sec() + kPastEofOffsetSec},
        }};
        for (const auto& bad : invalid_timestamps) {
            SCOPED_TRACE(bad.label);
            EXPECT_FALSE(sampler.sample_at(bad.timestamp_sec).has_value());
            EXPECT_TRUE(sampler.is_open());
        }

        // Subsequent valid decodes must still succeed cleanly after rejected timestamps.
        const auto mid = sampler.sample_at(kMidTimestampSec);
        ASSERT_TRUE(mid.has_value());
        verify_frame_invariants(*mid, probe.width, probe.height);

        const auto late = sampler.sample_at(kLateTimestampSec);
        ASSERT_TRUE(late.has_value());
        verify_frame_invariants(*late, probe.width, probe.height);
        EXPECT_GT(late->timestamp_sec, early->timestamp_sec);
        EXPECT_NE(early->bgr24, late->bgr24);
    }
}

TEST(AvFrameSamplerTest, SampleTimestampsDecodesSequentialFramesAndSkipsInvalidEntries) {
    constexpr std::array<double, 4> kSequentialTimestamps{0.5, 2.0, 5.0, 8.0};
    constexpr std::array<double, 3> kMixedTimestamps{0.5, -1.0, 8.0};
    constexpr std::size_t kExpectedMixedDecoded = 2;
    constexpr std::size_t kFirstValidIndex = 0;
    constexpr std::size_t kLastValidIndex = 2;

    const fs::path fixture = test_support::fixture_path(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture)) << "Missing fixture: " << core::to_utf8(fixture);

    const av::MediaInfo probe = av::probe(fixture);
    ASSERT_TRUE(probe.ok);

    av::FrameSampler sampler;
    ASSERT_TRUE(sampler.open(fixture));

    std::size_t empty_calls = 0;
    EXPECT_EQ(sampler.sample_timestamps(
                  std::span<const double>{},
                  [&](std::size_t, av::FrameBuffer&&) { ++empty_calls; }),
              0u);
    EXPECT_EQ(empty_calls, 0u);

    std::vector<std::size_t> indices;
    std::vector<av::FrameBuffer> frames;
    indices.reserve(kSequentialTimestamps.size());
    frames.reserve(kSequentialTimestamps.size());

    const std::size_t decoded_count = sampler.sample_timestamps(
        kSequentialTimestamps,
        [&](std::size_t idx, av::FrameBuffer&& frame) {
            indices.push_back(idx);
            frames.push_back(std::move(frame));
        });

    ASSERT_EQ(decoded_count, kSequentialTimestamps.size());
    ASSERT_EQ(indices.size(), kSequentialTimestamps.size());
    ASSERT_EQ(frames.size(), kSequentialTimestamps.size());

    for (std::size_t i = 0; i < kSequentialTimestamps.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(indices[i], i);
        verify_frame_invariants(frames[i], probe.width, probe.height);
        if (i > 0u) {
            EXPECT_GE(frames[i].timestamp_sec, frames[i - 1u].timestamp_sec);
        }
    }
    EXPECT_GT(frames.back().timestamp_sec, frames.front().timestamp_sec);
    EXPECT_NE(frames.front().bgr24, frames.back().bgr24);

    // Invalid entry mid-span is skipped while preserving original input indices.
    std::vector<std::size_t> mixed_indices;
    std::vector<av::FrameBuffer> mixed_frames;
    const std::size_t mixed_count = sampler.sample_timestamps(
        kMixedTimestamps,
        [&](std::size_t idx, av::FrameBuffer&& frame) {
            mixed_indices.push_back(idx);
            mixed_frames.push_back(std::move(frame));
        });

    ASSERT_EQ(mixed_count, kExpectedMixedDecoded);
    ASSERT_EQ(mixed_indices.size(), kExpectedMixedDecoded);
    ASSERT_EQ(mixed_frames.size(), kExpectedMixedDecoded);
    EXPECT_EQ(mixed_indices[0], kFirstValidIndex);
    EXPECT_EQ(mixed_indices[1], kLastValidIndex);
    verify_frame_invariants(mixed_frames[0], probe.width, probe.height);
    verify_frame_invariants(mixed_frames[1], probe.width, probe.height);
    EXPECT_NE(mixed_frames[0].bgr24, mixed_frames[1].bgr24);
}

TEST(AvFrameSamplerTest, SupportsMoveConstructionAssignmentCloseAndReopen) {
    constexpr double kEarlyTimestampSec = 0.5;
    constexpr double kMidTimestampSec = 2.0;

    const fs::path fixture = test_support::fixture_path(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture)) << "Missing fixture: " << core::to_utf8(fixture);

    const av::MediaInfo probe = av::probe(fixture);
    ASSERT_TRUE(probe.ok);

    av::FrameSampler orig;
    ASSERT_TRUE(orig.open(fixture));
    ASSERT_TRUE(orig.is_open());

    av::FrameSampler moved_ctor(std::move(orig));
    EXPECT_FALSE(orig.is_open());
    EXPECT_EQ(orig.width(), 0);
    EXPECT_EQ(orig.height(), 0);
    EXPECT_LE(orig.duration_sec(), 0.0);
    EXPECT_FALSE(orig.sample_at(kEarlyTimestampSec).has_value());

    EXPECT_TRUE(moved_ctor.is_open());
    EXPECT_EQ(moved_ctor.width(), probe.width);
    EXPECT_EQ(moved_ctor.height(), probe.height);
    const auto ctor_frame = moved_ctor.sample_at(kEarlyTimestampSec);
    ASSERT_TRUE(ctor_frame.has_value());
    verify_frame_invariants(*ctor_frame, probe.width, probe.height);

    av::FrameSampler moved_assign;
    moved_assign = std::move(moved_ctor);
    EXPECT_FALSE(moved_ctor.is_open());
    EXPECT_FALSE(moved_ctor.sample_at(kEarlyTimestampSec).has_value());

    EXPECT_TRUE(moved_assign.is_open());
    EXPECT_EQ(moved_assign.width(), probe.width);
    EXPECT_EQ(moved_assign.height(), probe.height);
    const auto assign_frame = moved_assign.sample_at(kMidTimestampSec);
    ASSERT_TRUE(assign_frame.has_value());
    verify_frame_invariants(*assign_frame, probe.width, probe.height);

    moved_assign.close();
    EXPECT_FALSE(moved_assign.is_open());
    EXPECT_EQ(moved_assign.width(), 0);
    EXPECT_EQ(moved_assign.height(), 0);
    EXPECT_LE(moved_assign.duration_sec(), 0.0);
    EXPECT_FALSE(moved_assign.sample_at(kEarlyTimestampSec).has_value());

    // Closing an already-closed sampler is idempotent.
    moved_assign.close();
    EXPECT_FALSE(moved_assign.is_open());

    // Reopen after close, then verify failed open resets state and subsequent open recovers.
    ASSERT_TRUE(moved_assign.open(fixture));
    EXPECT_TRUE(moved_assign.is_open());
    const auto reopened_frame = moved_assign.sample_at(kEarlyTimestampSec);
    ASSERT_TRUE(reopened_frame.has_value());
    verify_frame_invariants(*reopened_frame, probe.width, probe.height);

    test_support::ScopedTempDir sandbox("ut_frame_sampler_reopen");
    EXPECT_FALSE(moved_assign.open(sandbox.child("missing_reopen.mp4")));
    EXPECT_FALSE(moved_assign.is_open());
    EXPECT_FALSE(moved_assign.sample_at(kEarlyTimestampSec).has_value());

    ASSERT_TRUE(moved_assign.open(fixture));
    EXPECT_TRUE(moved_assign.is_open());
    const auto recovered_frame = moved_assign.sample_at(kMidTimestampSec);
    ASSERT_TRUE(recovered_frame.has_value());
    verify_frame_invariants(*recovered_frame, probe.width, probe.height);
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
