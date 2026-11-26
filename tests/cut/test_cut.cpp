#include "cut/cut.h"

#include "av/frame_grabber.h"
#include "av/media_probe.h"
#include "support/cli_runner.hpp"
#include "support/media_assertions.hpp"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

using test_support::FileSnapshot;
using test_support::MediaExpectation;
using test_support::ScopedTempDir;
using test_support::digest_file;
using test_support::kFixtureMainVideo;
using test_support::kFixtureUniformClip1;
using test_support::list_all_entries;
using test_support::list_regular_files;
using test_support::probe_valid_media;
using test_support::require_fixture;
using test_support::run_tool_app;
using test_support::verify_decodable_frame_at;

constexpr double kFpsTolerance = 1.0;
constexpr double kMultiCutRemovedSec = 4.0;
constexpr double kMultiCutDurationToleranceSec = 2.5;
constexpr double kMultiCutMinDurationSec = 9.5;
constexpr double kMultiCutMaxDurationSec = 14.5;
constexpr double kSingleCutMinDurationSec = 9.0;
constexpr double kSingleCutMinReductionSec = 0.5;
constexpr double kRepeatDurationToleranceSec = 0.2;
constexpr double kBlurDurationToleranceSec = 0.4;
constexpr double kBlurInsideProbeSec = 1.0;
constexpr double kBlurOutsideProbeSec = 0.1;
constexpr double kMinBlurPixelDiff = 2.0;
constexpr double kStartProbeSec = 0.5;
constexpr double kEndMarginSec = 0.5;
constexpr std::size_t kMaxFaultArgs = 8;

struct CutFaultCase {
    std::string_view label;
    bool use_existing_input;
    std::array<std::string_view, kMaxFaultArgs> extra_args;
    std::size_t arg_count;
};

constexpr std::array<CutFaultCase, 6> kCutFaultCases{{
    {"NonexistentInput", false, {{"-s", "1", "-e", "3"}}, 4},
    {"MismatchedStartEndCounts", true, {{"-s", "1", "5", "-e", "3"}}, 5},
    {"InvertedInterval", true, {{"-s", "5", "-e", "2"}}, 4},
    {"ZeroLengthInterval", true, {{"-s", "3", "-e", "3"}}, 4},
    {"NegativeStart", true, {{"--start=-1", "-e", "3"}}, 3},
    {"OverlappingIntervals", true, {{"-s", "2", "4", "-e", "6", "8"}}, 6},
}};

[[nodiscard]] cv::Mat decode_frame_bgr(
    const fs::path& video_file, double timestamp_sec, const ScopedTempDir& scratch,
    std::string_view filename) {
    const fs::path frame_path = scratch.child(filename);
    EXPECT_TRUE(av::grab_frame(video_file, frame_path, timestamp_sec))
        << "Failed to grab frame at t=" << timestamp_sec << "s from " << video_file;
    if (!fs::is_regular_file(frame_path)) {
        return {};
    }
    return cv::imread(frame_path.string(), cv::IMREAD_COLOR);
}

[[nodiscard]] double mean_abs_pixel_diff(const cv::Mat& a, const cv::Mat& b) {
    if (a.empty() || b.empty() || a.size() != b.size() || a.type() != b.type()) {
        return -1.0;
    }
    const double total_samples =
        static_cast<double>(a.total()) * static_cast<double>(a.channels());
    return cv::norm(a, b, cv::NORM_L1) / total_samples;
}

TEST(CutComponentTest, MultiIntervalLosslessStreamCopyCutPreservesStreamsAndShortensDuration) {
    const fs::path fixture = require_fixture(kFixtureMainVideo);
    ASSERT_TRUE(fs::exists(fixture));

    ScopedTempDir sandbox("cut_multi");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("cut_multi_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "in/source.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    const fs::path out_file = out_dir / "multi_cut.mp4";
    const int rc = run_tool_app<CutApp>({
        "cut",
        "-i", input_copy.string(),
        "-o", out_file.string(),
        "-s", "2", "9",
        "-e", "4", "11",
    });
    ASSERT_EQ(0, rc);

    const auto out_entries = list_all_entries(out_dir);
    ASSERT_EQ(1u, out_entries.size()) << "Expected exact single output with zero stray temp files";
    EXPECT_EQ(out_file, out_entries.front());
    EXPECT_EQ(1u, list_all_entries(in_dir).size()) << "Input directory must remain free of temp files";
    EXPECT_TRUE(input_snap.is_unchanged()) << "Input media was modified in place";

    const av::MediaInfo out_info = probe_valid_media(
        out_file,
        MediaExpectation{
            .video_codec = "hevc",
            .audio_codec = "aac",
            .width = src_info.width,
            .height = src_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = src_info.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = src_info.duration_sec - kMultiCutRemovedSec,
            .duration_tolerance_sec = kMultiCutDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);
    EXPECT_GT(out_info.duration_sec, kMultiCutMinDurationSec);
    EXPECT_LT(out_info.duration_sec, kMultiCutMaxDurationSec);

    verify_decodable_frame_at(out_file, kStartProbeSec, scratch, "start.jpg");
    verify_decodable_frame_at(out_file, out_info.duration_sec * 0.5, scratch, "mid.jpg");
    verify_decodable_frame_at(out_file, out_info.duration_sec - kEndMarginSec, scratch, "end.jpg");
}

TEST(CutComponentTest, DefaultOmittedOutputAndRepeatOverwriteIdempotence) {
    const fs::path fixture = require_fixture(kFixtureMainVideo);
    ASSERT_TRUE(fs::exists(fixture));

    ScopedTempDir sandbox("cut_default_repeat");
    const fs::path same_dir = sandbox.create_subdir("same");
    const fs::path repeat_dir = sandbox.create_subdir("repeat");
    ScopedTempDir scratch("cut_default_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "same/recording.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    const int rc_default = run_tool_app<CutApp>({
        "cut",
        "-i", input_copy.string(),
        "-s", "2",
        "-e", "5",
    });
    ASSERT_EQ(0, rc_default);
    EXPECT_TRUE(input_snap.is_unchanged());

    const auto same_entries = list_all_entries(same_dir);
    ASSERT_EQ(2u, same_entries.size()) << "Expected original plus default cut output with zero temp files";
    const auto generated = list_regular_files(same_dir, ".mp4", "recording_cut_");
    ASSERT_EQ(1u, generated.size()) << "Expected <stem>_cut_<ts>.mp4 alongside input";

    const av::MediaInfo default_info = probe_valid_media(
        generated.front(),
        MediaExpectation{
            .video_codec = src_info.video_codec,
            .audio_codec = src_info.audio_codec,
            .width = src_info.width,
            .height = src_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = src_info.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
        });
    ASSERT_TRUE(default_info.ok);
    EXPECT_GT(default_info.duration_sec, kSingleCutMinDurationSec);
    EXPECT_LT(default_info.duration_sec, src_info.duration_sec - kSingleCutMinReductionSec);
    verify_decodable_frame_at(generated.front(), kStartProbeSec, scratch, "default_start.jpg");

    const fs::path explicit_out = repeat_dir / "overwrite_target.mp4";
    const int rc_first = run_tool_app<CutApp>({
        "cut",
        "-i", input_copy.string(),
        "-o", explicit_out.string(),
        "-s", "2",
        "-e", "5",
    });
    ASSERT_EQ(0, rc_first);
    ASSERT_EQ(1u, list_all_entries(repeat_dir).size());
    const av::MediaInfo first_info = probe_valid_media(explicit_out);
    ASSERT_TRUE(first_info.ok);

    const int rc_second = run_tool_app<CutApp>({
        "cut",
        "-i", input_copy.string(),
        "-o", explicit_out.string(),
        "-s", "2",
        "-e", "5",
    });
    ASSERT_EQ(0, rc_second);
    ASSERT_EQ(1u, list_all_entries(repeat_dir).size()) << "Overwrite run leaked stray temp files";
    EXPECT_TRUE(input_snap.is_unchanged());

    const av::MediaInfo second_info = probe_valid_media(
        explicit_out,
        MediaExpectation{
            .video_codec = src_info.video_codec,
            .audio_codec = src_info.audio_codec,
            .width = src_info.width,
            .height = src_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = src_info.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = first_info.duration_sec,
            .duration_tolerance_sec = kRepeatDurationToleranceSec,
        });
    ASSERT_TRUE(second_info.ok);
    verify_decodable_frame_at(explicit_out, kStartProbeSec, scratch, "repeat_start.jpg");
}

TEST(CutComponentTest, BlurAndSoundOffPreserveDurationAndAlterPixelsInsideWindow) {
    const fs::path fixture = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::exists(fixture));

    ScopedTempDir sandbox("cut_blur");
    const fs::path blur_dir = sandbox.create_subdir("blur_in");
    const fs::path muted_dir = sandbox.create_subdir("muted_out");
    ScopedTempDir scratch("cut_blur_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "blur_in/clip.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    const int rc_blur = run_tool_app<CutApp>({
        "cut",
        "-i", input_copy.string(),
        "-s", "0.5",
        "-e", "1.5",
        "--blur",
    });
    ASSERT_EQ(0, rc_blur);
    EXPECT_TRUE(input_snap.is_unchanged());

    ASSERT_EQ(2u, list_all_entries(blur_dir).size()) << "Expected original plus default blur output";
    const auto blur_outputs = list_regular_files(blur_dir, ".mp4", "clip_blur_");
    ASSERT_EQ(1u, blur_outputs.size()) << "Expected <stem>_blur_<ts>.mp4 alongside input";
    const fs::path blur_file = blur_outputs.front();

    const av::MediaInfo blur_info = probe_valid_media(
        blur_file,
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = src_info.audio_codec,
            .width = src_info.width,
            .height = src_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_duration_sec = src_info.duration_sec,
            .duration_tolerance_sec = kBlurDurationToleranceSec,
        });
    ASSERT_TRUE(blur_info.ok);

    const cv::Mat src_inside = decode_frame_bgr(input_copy, kBlurInsideProbeSec, scratch, "src_in.jpg");
    const cv::Mat blur_inside = decode_frame_bgr(blur_file, kBlurInsideProbeSec, scratch, "blur_in.jpg");
    const cv::Mat src_outside = decode_frame_bgr(input_copy, kBlurOutsideProbeSec, scratch, "src_out.jpg");
    const cv::Mat blur_outside = decode_frame_bgr(blur_file, kBlurOutsideProbeSec, scratch, "blur_out.jpg");

    ASSERT_FALSE(src_inside.empty());
    ASSERT_FALSE(blur_inside.empty());
    ASSERT_FALSE(src_outside.empty());
    ASSERT_FALSE(blur_outside.empty());

    EXPECT_EQ(src_info.width, blur_inside.cols);
    EXPECT_EQ(src_info.height, blur_inside.rows);
    EXPECT_EQ(src_info.width, blur_outside.cols);
    EXPECT_EQ(src_info.height, blur_outside.rows);

    const double inside_diff = mean_abs_pixel_diff(src_inside, blur_inside);
    const double outside_diff = mean_abs_pixel_diff(src_outside, blur_outside);
    EXPECT_GT(inside_diff, kMinBlurPixelDiff)
        << "Blurred frame inside [0.5, 1.5] did not diverge measurably from source";
    EXPECT_GT(inside_diff, outside_diff)
        << "Blur effect inside [0.5, 1.5] should exceed unblurred frame delta outside window";

    const fs::path muted_file = muted_dir / "clip_blur_muted.mp4";
    const int rc_muted = run_tool_app<CutApp>({
        "cut",
        "-i", input_copy.string(),
        "-o", muted_file.string(),
        "-s", "0.5",
        "-e", "1.5",
        "--blur",
        "--sound-off",
    });
    ASSERT_EQ(0, rc_muted);
    EXPECT_TRUE(input_snap.is_unchanged());
    ASSERT_EQ(1u, list_all_entries(muted_dir).size());

    const av::MediaInfo muted_info = probe_valid_media(
        muted_file,
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = "aac",
            .width = src_info.width,
            .height = src_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_duration_sec = src_info.duration_sec,
            .duration_tolerance_sec = kBlurDurationToleranceSec,
        });
    ASSERT_TRUE(muted_info.ok);

    const cv::Mat muted_inside =
        decode_frame_bgr(muted_file, kBlurInsideProbeSec, scratch, "muted_in.jpg");
    ASSERT_FALSE(muted_inside.empty());
    EXPECT_EQ(src_info.width, muted_inside.cols);
    EXPECT_EQ(src_info.height, muted_inside.rows);
    EXPECT_GT(mean_abs_pixel_diff(src_inside, muted_inside), kMinBlurPixelDiff);
    EXPECT_NE(digest_file(muted_file), digest_file(blur_file))
        << "--blur --sound-off output should differ from non-muted --blur output";
}

TEST(CutComponentTest, InvalidInputsAndIntervalsFailWithoutPartialOutputs) {
    const fs::path fixture = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::exists(fixture));

    ScopedTempDir sandbox("cut_faults");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path input_copy = sandbox.copy_in(fixture, "in/valid.mp4");
    const FileSnapshot input_snap(input_copy);

    for (const auto& tc : kCutFaultCases) {
        SCOPED_TRACE(tc.label);
        const fs::path out_dir = sandbox.create_subdir(std::string("out_") + std::string(tc.label));
        const fs::path out_file = out_dir / "should_not_exist.mp4";
        const fs::path input_arg = tc.use_existing_input
            ? input_copy
            : (sandbox.child("missing_video_file.mp4"));

        std::vector<std::string> args = {
            "cut",
            "-i", input_arg.string(),
            "-o", out_file.string(),
        };
        for (const auto extra : std::span{tc.extra_args}.first(tc.arg_count)) {
            args.emplace_back(extra);
        }

        const int rc = run_tool_app<CutApp>(std::span<const std::string>{args});
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Failed cut invocation left partial/stray files in " << out_dir;
    }

    EXPECT_TRUE(input_snap.is_unchanged());
    EXPECT_EQ(1u, list_all_entries(in_dir).size());
}

}  // namespace
