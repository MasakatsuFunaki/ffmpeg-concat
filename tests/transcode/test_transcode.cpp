#include "transcode/transcode.h"

#include "av/media_probe.h"
#include "support/cli_runner.hpp"
#include "support/media_assertions.hpp"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
using test_support::kFixtureMixedFpsDir;
using test_support::kFixtureUniformClip1;
using test_support::kFixtureUniformFpsDir;
using test_support::list_all_entries;
using test_support::list_regular_files;
using test_support::probe_valid_media;
using test_support::require_fixture;
using test_support::run_tool_app;
using test_support::verify_decodable_frame_at;

constexpr double kNormalizedFps = 30.0;
constexpr double kFpsTolerance = 1.0;
constexpr double kJoinedDurationSec = 4.0;
constexpr double kSingleDurationSec = 2.0;
constexpr double kDurationToleranceSec = 0.4;
constexpr double kEarlyProbeSec = 0.5;
constexpr double kLateJoinedProbeSec = 3.0;
constexpr double kLateSingleProbeSec = 1.5;
constexpr std::size_t kMaxFaultArgs = 8;

enum class FaultInputKind { Nonexistent, EmptyDir, ValidClip };

struct TranscodeFaultCase {
    std::string_view label;
    FaultInputKind input_kind;
    std::array<std::string_view, kMaxFaultArgs> extra_args;
    std::size_t arg_count;
};

constexpr std::array<TranscodeFaultCase, 5> kTranscodeFaultCases{{
    {"NonexistentInput", FaultInputKind::Nonexistent, {{"-e", "libx264", "-p", "ultrafast"}}, 4},
    {"EmptyDirectory", FaultInputKind::EmptyDir, {{"-e", "libx264", "-p", "ultrafast"}}, 4},
    {"NegativeCrf", FaultInputKind::ValidClip, {{"-e", "libx264", "-p", "ultrafast", "--crf=-1"}}, 5},
    {"ExcessiveCrf", FaultInputKind::ValidClip, {{"-e", "libx264", "-p", "ultrafast", "-q", "52"}}, 6},
    {"UnknownEncoder", FaultInputKind::ValidClip, {{"-e", "nonexistent_encoder_xyz"}}, 2},
}};

[[nodiscard]] std::vector<FileSnapshot> copy_mp4_directory(
    const fs::path& src_dir, const ScopedTempDir& sandbox, std::string_view dst_rel) {
    const auto clips = list_regular_files(src_dir, ".mp4");
    std::vector<FileSnapshot> snaps;
    snaps.reserve(clips.size());
    for (const auto& clip : clips) {
        const fs::path copied =
            sandbox.copy_in(clip, std::string(dst_rel) + "/" + clip.filename().string());
        snaps.emplace_back(copied);
    }
    return snaps;
}

TEST(TranscodeComponentTest, MixedFpsDirectoryJoinNormalizesToLowestFps) {
    const fs::path mixed_fixture = require_fixture(kFixtureMixedFpsDir);
    ASSERT_TRUE(fs::is_directory(mixed_fixture));

    ScopedTempDir sandbox("transcode_mixed");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("transcode_mixed_frames");

    const auto input_snaps = copy_mp4_directory(mixed_fixture, sandbox, "in");
    ASSERT_GE(input_snaps.size(), 2u);
    const av::MediaInfo ref_info = probe_valid_media(input_snaps.front().path());
    ASSERT_TRUE(ref_info.ok);

    const int rc = run_tool_app<TranscodeApp>({
        "transcode",
        "-i", in_dir.string(),
        "-o", out_dir.string(),
        "-e", "libx264",
        "-p", "ultrafast",
        "-q", "28",
    });
    ASSERT_EQ(0, rc);

    const auto out_entries = list_all_entries(out_dir);
    ASSERT_EQ(1u, out_entries.size()) << "Expected single output file with zero stray artifacts";
    const auto outputs = list_regular_files(out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(1u, outputs.size());

    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = "aac",
            .width = ref_info.width,
            .height = ref_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = kNormalizedFps,
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = kJoinedDurationSec,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);

    verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "early.jpg");
    verify_decodable_frame_at(outputs.front(), kLateJoinedProbeSec, scratch, "late.jpg");

    EXPECT_TRUE(std::ranges::all_of(input_snaps, &FileSnapshot::is_unchanged));
    EXPECT_EQ(input_snaps.size(), list_all_entries(in_dir).size());
}

TEST(TranscodeComponentTest, UniformFpsDirectoryJoinPreservesResolutionAndRate) {
    const fs::path uniform_fixture = require_fixture(kFixtureUniformFpsDir);
    ASSERT_TRUE(fs::is_directory(uniform_fixture));

    ScopedTempDir sandbox("transcode_uniform");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("transcode_uniform_frames");

    const auto input_snaps = copy_mp4_directory(uniform_fixture, sandbox, "in");
    ASSERT_GE(input_snaps.size(), 2u);
    const av::MediaInfo ref_info = probe_valid_media(input_snaps.front().path());
    ASSERT_TRUE(ref_info.ok);

    const int rc = run_tool_app<TranscodeApp>({
        "transcode",
        "-i", in_dir.string(),
        "-o", out_dir.string(),
        "-e", "libx264",
        "-p", "ultrafast",
    });
    ASSERT_EQ(0, rc);

    const auto out_entries = list_all_entries(out_dir);
    ASSERT_EQ(1u, out_entries.size());
    const auto outputs = list_regular_files(out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(1u, outputs.size());

    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = "aac",
            .width = ref_info.width,
            .height = ref_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = kNormalizedFps,
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = kJoinedDurationSec,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);

    verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "early.jpg");
    verify_decodable_frame_at(outputs.front(), kLateJoinedProbeSec, scratch, "late.jpg");
    EXPECT_TRUE(std::ranges::all_of(input_snaps, &FileSnapshot::is_unchanged));
}

TEST(TranscodeComponentTest, SingleFileInputProducesValidH264AacOutput) {
    const fs::path clip_fixture = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(clip_fixture));

    ScopedTempDir sandbox("transcode_single");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("transcode_single_frames");

    const fs::path input_copy = sandbox.copy_in(clip_fixture, "in/clip.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo ref_info = probe_valid_media(input_copy);
    ASSERT_TRUE(ref_info.ok);

    const int rc = run_tool_app<TranscodeApp>({
        "transcode",
        "-i", input_copy.string(),
        "-o", out_dir.string(),
        "-e", "libx264",
        "-p", "ultrafast",
        "-q", "26",
    });
    ASSERT_EQ(0, rc);

    ASSERT_EQ(1u, list_all_entries(out_dir).size());
    const auto outputs = list_regular_files(out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(1u, outputs.size());

    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = "aac",
            .width = ref_info.width,
            .height = ref_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = kNormalizedFps,
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = kSingleDurationSec,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);

    verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "early.jpg");
    verify_decodable_frame_at(outputs.front(), kLateSingleProbeSec, scratch, "late.jpg");
    EXPECT_TRUE(input_snap.is_unchanged());
    EXPECT_EQ(1u, list_all_entries(in_dir).size());
}

TEST(TranscodeComponentTest, SameInputOutputDirectoryPreservesOriginalClip) {
    const fs::path clip_fixture = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(clip_fixture));

    ScopedTempDir sandbox("transcode_same_dir");
    const fs::path same_dir = sandbox.create_subdir("same");
    ScopedTempDir scratch("transcode_same_frames");

    const fs::path input_copy = sandbox.copy_in(clip_fixture, "same/2026-01-01 00-00-00.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo ref_info = probe_valid_media(input_copy);
    ASSERT_TRUE(ref_info.ok);

    const int rc = run_tool_app<TranscodeApp>({
        "transcode",
        "-i", same_dir.string(),
        "-o", same_dir.string(),
        "-e", "libx264",
        "-p", "ultrafast",
    });
    ASSERT_EQ(0, rc);

    EXPECT_TRUE(input_snap.is_unchanged()) << "Original clip was altered in same-dir transcode";
    const auto entries = list_all_entries(same_dir);
    ASSERT_EQ(2u, entries.size()) << "Expected exact original plus one transcoded_*.mp4";

    const auto outputs = list_regular_files(same_dir, ".mp4", "transcoded_");
    ASSERT_EQ(1u, outputs.size());

    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = "aac",
            .width = ref_info.width,
            .height = ref_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = kNormalizedFps,
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = kSingleDurationSec,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);
    verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "same_dir.jpg");
}

TEST(TranscodeComponentTest, InvalidInputsCrfAndEncoderFailWithoutStrayOutputs) {
    const fs::path clip_fixture = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(clip_fixture));

    ScopedTempDir sandbox("transcode_faults");
    const fs::path empty_dir = sandbox.create_subdir("empty");
    const fs::path input_copy = sandbox.copy_in(clip_fixture, "in/valid.mp4");
    const FileSnapshot input_snap(input_copy);

    for (const auto& tc : kTranscodeFaultCases) {
        SCOPED_TRACE(tc.label);
        const fs::path out_dir = sandbox.create_subdir(std::string("out_") + std::string(tc.label));

        fs::path input_arg;
        switch (tc.input_kind) {
            case FaultInputKind::Nonexistent:
                input_arg = sandbox.child("nonexistent_input_dir");
                break;
            case FaultInputKind::EmptyDir:
                input_arg = empty_dir;
                break;
            case FaultInputKind::ValidClip:
                input_arg = input_copy;
                break;
        }

        std::vector<std::string> args = {
            "transcode",
            "-i", input_arg.string(),
            "-o", out_dir.string(),
        };
        for (const auto extra : std::span{tc.extra_args}.first(tc.arg_count)) {
            args.emplace_back(extra);
        }

        const int rc = run_tool_app<TranscodeApp>(std::span<const std::string>{args});
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Failed transcode invocation left stray files in " << out_dir;
    }

    EXPECT_TRUE(input_snap.is_unchanged());
}

}  // namespace
