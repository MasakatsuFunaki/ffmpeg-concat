#include "encode/encode_plan.h"
#include "encode/encoder_catalog.h"
#include "encode/encoder_profile.h"
#include "av/av_capabilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using OptionList = std::vector<std::pair<std::string, std::string>>;

constexpr av::FrameRate kFps60{60, 1};
constexpr av::FrameRate kFps50{50, 1};
constexpr av::FrameRate kFps30{30, 1};
constexpr av::FrameRate kFps24{24, 1};
constexpr av::FrameRate kFps2997{30000, 1001};
constexpr av::FrameRate kFps2997Approx{2997, 100};
constexpr av::FrameRate kFps60Over2{60, 2};
constexpr av::FrameRate kUnprobed{0, 0};

[[nodiscard]] std::string_view find_option(const OptionList& opts,
                                           std::string_view key) noexcept {
    const auto it = std::ranges::find_if(
        opts, [key](const auto& kv) { return kv.first == key; });
    if (it == opts.end()) {
        return {};
    }
    return it->second;
}

[[nodiscard]] bool has_option(const OptionList& opts, std::string_view key) noexcept {
    return std::ranges::any_of(opts, [key](const auto& kv) { return kv.first == key; });
}

[[nodiscard]] EncodePlan make_base_plan(std::size_t input_count) {
    EncodePlan plan;
    for (std::size_t i = 0; i < input_count; ++i) {
        plan.inputs.emplace_back("in" + std::to_string(i) + ".mp4");
    }
    plan.output = "out.mp4";
    plan.encoder = "libx264";
    plan.crf = 30;
    plan.preset = "ultrafast";
    return plan;
}

}  // namespace

TEST(LowestFpsIfMixedTest, TableCoversEmptyUniformNoiseInvalidAndOrderIndependence) {
    struct LowestFpsCase {
        std::string_view label;
        std::array<av::FrameRate, 5> rates;
        std::size_t count;
        bool expected_known;
        av::FrameRate expected_rate;
    };

    constexpr std::array<LowestFpsCase, 11> kCases{{
        {"empty", {{}}, 0, false, kUnprobed},
        {"single_rate", {{kFps60}}, 1, false, kUnprobed},
        {"unprobed_plus_single", {{kUnprobed, kFps60}}, 2, false, kUnprobed},
        {"all_invalid_or_unprobed",
         {{kUnprobed, av::FrameRate{-30, 1}, av::FrameRate{30, 0}}},
         3,
         false,
         kUnprobed},
        {"uniform_rates", {{kFps30, kFps30, kFps30}}, 3, false, kUnprobed},
        {"unreduced_equivalent_60_over_2_and_30_over_1",
         {{kFps60Over2, kFps30}},
         2,
         false,
         kUnprobed},
        {"rounding_noise_ntsc_vs_2997_over_100",
         {{kFps2997, kFps2997Approx}},
         2,
         false,
         kUnprobed},
        {"genuine_difference_30_vs_ntsc", {{kFps30, kFps2997}}, 2, true, kFps2997},
        {"order_independent_four_rates",
         {{kFps60, kFps30, kFps24, kFps50}},
         4,
         true,
         kFps24},
        {"invalid_negative_and_zero_entries_ignored",
         {{av::FrameRate{-30, 1}, av::FrameRate{30, -1}, av::FrameRate{0, 1}, kFps60, kFps30}},
         5,
         true,
         kFps30},
        {"preserves_exact_ntsc_rational_when_higher_first",
         {{kFps60, kFps2997}},
         2,
         true,
         kFps2997},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const std::span<const av::FrameRate> rate_span{tc.rates.data(), tc.count};
        const av::FrameRate actual = lowest_fps_if_mixed(rate_span);
        EXPECT_EQ(actual.known(), tc.expected_known);
        if (tc.expected_known) {
            EXPECT_EQ(actual, tc.expected_rate);
        }
    }

    // Exercise the initializer_list overload explicitly as well.
    EXPECT_EQ(lowest_fps_if_mixed({kFps60, kFps30}), kFps30);
    EXPECT_FALSE(lowest_fps_if_mixed(std::initializer_list<av::FrameRate>{}).known());
}

TEST(BuildFilterDescriptionTest, TableCoversEmptySingleMultiAndExactRationals) {
    struct FilterCase {
        std::string_view label;
        std::size_t input_count;
        av::FrameRate target_fps;
        std::string_view expected;
    };

    constexpr std::array<FilterCase, 7> kCases{{
        {"empty_inputs_no_fps", 0, kUnprobed, ""},
        {"empty_inputs_with_fps", 0, kFps30, ""},
        {"single_input_passthrough",
         1,
         kUnprobed,
         "[in0v]null[outv];[in0a]anull[outa]"},
        {"single_input_with_fps",
         1,
         kFps30,
         "[in0v]fps=30/1[outv];[in0a]anull[outa]"},
        {"two_inputs_plain_concat",
         2,
         kUnprobed,
         "[in0v][in0a][in1v][in1a]concat=n=2:v=1:a=1[outv][outa]"},
        {"two_inputs_normalized_fps",
         2,
         kFps30,
         "[in0v]fps=30/1[v0];[in1v]fps=30/1[v1];"
         "[v0][in0a][v1][in1a]concat=n=2:v=1:a=1[outv][outa]"},
        {"three_inputs_exact_ntsc_rational",
         3,
         kFps2997,
         "[in0v]fps=30000/1001[v0];[in1v]fps=30000/1001[v1];[in2v]fps=30000/1001[v2];"
         "[v0][in0a][v1][in1a][v2][in2a]concat=n=3:v=1:a=1[outv][outa]"},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EncodePlan plan = make_base_plan(tc.input_count);
        plan.target_fps = tc.target_fps;

        const std::string filter = build_filter_description(plan);
        EXPECT_EQ(filter, tc.expected);
        EXPECT_EQ(filter.find('.'), std::string::npos);
        EXPECT_EQ(filter.find(','), std::string::npos);
    }
}

TEST(BuildRequestTest, MapsPlanFieldsAndHandWrittenFilterGraph) {
    constexpr std::string_view kExpectedFilter =
        "[in0v]fps=30/1[v0];[in1v]fps=30/1[v1];"
        "[v0][in0a][v1][in1a]concat=n=2:v=1:a=1[outv][outa]";
    constexpr int64_t kExpectedAudioBitRate = 192000;

    EncodePlan default_plan = make_base_plan(2);
    const av::TranscodeRequest default_req = build_request(default_plan);
    EXPECT_FALSE(default_req.hwaccel_cuda);
    EXPECT_FALSE(default_req.audio_copy);
    EXPECT_EQ(default_req.filter_description,
              "[in0v][in0a][in1v][in1a]concat=n=2:v=1:a=1[outv][outa]");

    EncodePlan gpu_plan = make_base_plan(2);
    gpu_plan.hwaccel_cuda = true;
    gpu_plan.target_fps = kFps30;

    const av::TranscodeRequest req = build_request(gpu_plan);
    EXPECT_EQ(req.inputs, gpu_plan.inputs);
    EXPECT_EQ(req.output, gpu_plan.output);
    EXPECT_EQ(req.video_encoder, "libx264");
    EXPECT_TRUE(req.hwaccel_cuda);
    EXPECT_EQ(req.filter_description, kExpectedFilter);
    EXPECT_EQ(req.video_bit_rate, 0);
    EXPECT_EQ(req.audio_encoder, "aac");
    EXPECT_EQ(req.audio_bit_rate, kExpectedAudioBitRate);
    EXPECT_FALSE(req.audio_copy);
}

TEST(BuildRequestTest, EncoderDialectMatrixCoversAllProfilesDefaultsAndCrfBoundaries) {
    struct DialectCase {
        std::string_view encoder;
        int crf;
        std::string_view preset;
        std::string_view quality_key;
        std::string_view expected_quality;
        std::string_view speed_key;
        std::string_view expected_speed;
        std::string_view forbidden_key;
    };

    constexpr std::array<DialectCase, 12> kCases{{
        {"av1_nvenc", 24, "p6", "cq", "24", "preset", "p6", "crf"},
        {"av1_nvenc", 0, "", "cq", "0", "preset", "p4", "crf"},
        {"h264_nvenc", 51, "p1", "cq", "51", "preset", "p1", "crf"},
        {"h264_nvenc", 30, "", "cq", "30", "preset", "p4", "crf"},
        {"libsvtav1", 35, "8", "crf", "35", "preset", "8", "cq"},
        {"libsvtav1", 0, "", "crf", "0", "preset", "6", "cq"},
        {"libx264", 23, "ultrafast", "crf", "23", "preset", "ultrafast", "cq"},
        {"libx264", 51, "", "crf", "51", "preset", "medium", "cq"},
        {"libaom-av1", 32, "2", "crf", "32", "cpu-used", "2", "preset"},
        {"libaom-av1", 30, "", "crf", "30", "cpu-used", "4", "preset"},
        {"custom_future_encoder", 28, "3", "crf", "28", "cpu-used", "3", "preset"},
        {"custom_future_encoder", 30, "", "crf", "30", "cpu-used", "4", "preset"},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(std::string(tc.encoder) + ":" + std::string(tc.preset));
        EncodePlan plan = make_base_plan(2);
        plan.encoder = std::string(tc.encoder);
        plan.crf = tc.crf;
        plan.preset = std::string(tc.preset);

        const av::TranscodeRequest req = build_request(plan);
        EXPECT_EQ(req.video_encoder, tc.encoder);
        EXPECT_EQ(req.video_bit_rate, 0);
        ASSERT_EQ(req.video_options.size(), 2u);
        EXPECT_EQ(find_option(req.video_options, tc.quality_key), tc.expected_quality);
        EXPECT_EQ(find_option(req.video_options, tc.speed_key), tc.expected_speed);
        EXPECT_FALSE(has_option(req.video_options, tc.forbidden_key));
    }
}

TEST(EncoderCatalogTest, ProfileForDispatchesAllKnownAndFallbackEncoders) {
    struct ProfileCase {
        std::string_view encoder;
        int crf;
        std::string_view preset;
        std::string_view expected_name;
        std::string_view quality_key;
        std::string_view expected_quality;
        std::string_view speed_key;
        std::string_view expected_speed;
    };

    constexpr std::array<ProfileCase, 7> kCases{{
        {"av1_nvenc", 28, "", "av1_nvenc", "cq", "28", "preset", "p4"},
        {"h264_nvenc", 22, "p5", "h264_nvenc", "cq", "22", "preset", "p5"},
        {"libsvtav1", 30, "", "libsvtav1", "crf", "30", "preset", "6"},
        {"libx264", 18, "", "libx264", "crf", "18", "preset", "medium"},
        {"libaom-av1", 34, "", "libaom-av1", "crf", "34", "cpu-used", "4"},
        {"unknown_codec", 30, "5", "unknown_codec", "crf", "30", "cpu-used", "5"},
        {"", 30, "", "", "crf", "30", "cpu-used", "4"},
    }};

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.encoder);
        const auto profile = EncoderCatalog::profile_for(tc.encoder);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->name(), tc.expected_name);

        const EncoderOptions opts = profile->options(tc.crf, tc.preset);
        EXPECT_EQ(opts.bit_rate, 0);
        ASSERT_EQ(opts.opts.size(), 2u);
        EXPECT_EQ(find_option(opts.opts, tc.quality_key), tc.expected_quality);
        EXPECT_EQ(find_option(opts.opts, tc.speed_key), tc.expected_speed);
    }
}

TEST(EncoderCatalogTest, DetectReturnsUsableEncoderFromCandidateSet) {
    constexpr std::array<std::string_view, 2> kH264Candidates{"h264_nvenc", "libx264"};
    constexpr std::array<std::string_view, 3> kAv1Candidates{
        "av1_nvenc", "libsvtav1", "libaom-av1"};

    const std::string h264 = EncoderCatalog::detect("h264");
    EXPECT_TRUE(std::ranges::find(kH264Candidates, h264) != kH264Candidates.end())
        << "Unexpected h264 encoder: " << h264;
    EXPECT_TRUE(av::can_use_encoder(h264));

    const std::string av1 = EncoderCatalog::detect("av1");
    EXPECT_TRUE(std::ranges::find(kAv1Candidates, av1) != kAv1Candidates.end())
        << "Unexpected av1 encoder: " << av1;
    EXPECT_TRUE(av::can_use_encoder(av1));

    const std::string fallback = EncoderCatalog::detect("default_codec");
    EXPECT_TRUE(std::ranges::find(kAv1Candidates, fallback) != kAv1Candidates.end())
        << "Unexpected default encoder: " << fallback;
    EXPECT_TRUE(av::can_use_encoder(fallback));
}
