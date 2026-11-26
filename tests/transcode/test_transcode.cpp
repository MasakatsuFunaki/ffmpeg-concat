#include "transcode/transcode.h"

#include "av/media_probe.h"
#include "av/transcoder.h"
#include "ffmpeg/progress_reporter.h"
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
using test_support::kFixtureUniformClip2;
using test_support::kFixtureUniformFpsDir;
using test_support::list_all_entries;
using test_support::list_regular_files;
using test_support::path_to_utf8;
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
constexpr std::size_t kExpectedSingleOutput = 1;
constexpr std::size_t kExpectedTwoClips = 2;
constexpr std::u8string_view kUnicodeInDir = u8"in_café_動画";
constexpr std::u8string_view kUnicodeOutDir = u8"out_café_動画";
constexpr std::u8string_view kUnicodeClip1Rel = u8"in_café_動画/café_動画_2026-01-01 00-00-00.mp4";
constexpr std::u8string_view kUnicodeClip2Rel = u8"in_café_動画/café_動画_2026-01-02 00-00-00.mp4";

[[nodiscard]] bool create_silent_h264_clip(
    const fs::path& src, const fs::path& dst, double src_duration_sec) {
    av::TranscodeRequest req;
    req.inputs = {src};
    req.output = dst;
    req.filter_description = "[in0v]null[outv]";
    req.video_encoder = "libx264";
    req.video_options = {{"crf", "28"}, {"preset", "ultrafast"}};
    req.audio_copy = false;
    NullProgressReporter reporter;
    return av::transcode(req, reporter, src_duration_sec);
}

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
            << "Failed transcode invocation left stray files in " << path_to_utf8(out_dir);
    }

    EXPECT_TRUE(input_snap.is_unchanged());
}

TEST(TranscodeComponentTest, AudioLessSingleMultiAndMixedAudioDirectoryContracts) {
    const fs::path mixed_fixture = require_fixture(kFixtureMixedFpsDir);
    ASSERT_TRUE(fs::is_directory(mixed_fixture));

    const auto mixed_clips = list_regular_files(mixed_fixture, ".mp4");
    ASSERT_EQ(kExpectedTwoClips, mixed_clips.size());

    ScopedTempDir sandbox("transcode_silent");
    const fs::path silent_dir = sandbox.create_subdir("in_silent");
    ScopedTempDir scratch("transcode_silent_frames");

    std::vector<av::MediaInfo> silent_infos;
    std::vector<FileSnapshot> silent_snaps;
    silent_infos.reserve(mixed_clips.size());
    silent_snaps.reserve(mixed_clips.size());

    for (const auto& src : mixed_clips) {
        const av::MediaInfo src_info = probe_valid_media(src);
        ASSERT_TRUE(src_info.ok);
        const fs::path dst = silent_dir / src.filename();
        ASSERT_TRUE(create_silent_h264_clip(src, dst, src_info.duration_sec));

        const av::MediaInfo s_info = probe_valid_media(
            dst,
            MediaExpectation{
                .video_codec = "h264",
                .audio_codec = "",
                .width = src_info.width,
                .height = src_info.height,
                .has_video = true,
                .has_audio = false,
            });
        ASSERT_TRUE(s_info.ok);
        ASSERT_TRUE(s_info.has_video);
        ASSERT_FALSE(s_info.has_audio);
        ASSERT_TRUE(s_info.audio_codec.empty());
        silent_infos.push_back(s_info);
        silent_snaps.emplace_back(dst);
    }
    ASSERT_EQ(kExpectedTwoClips, silent_infos.size());
    EXPECT_NE(silent_infos[0].frame_rate, silent_infos[1].frame_rate);

    {
        SCOPED_TRACE("SingleSilentFile");
        const fs::path out_dir = sandbox.create_subdir("out_silent_single");
        const std::string in_arg = path_to_utf8(silent_snaps[0].path());
        const std::string out_arg = path_to_utf8(out_dir);
        const int rc = run_tool_app<TranscodeApp>({
            "transcode",
            "-i", in_arg,
            "-o", out_arg,
            "-e", "libx264",
            "-p", "ultrafast",
            "-q", "28",
        });
        ASSERT_EQ(0, rc);

        ASSERT_EQ(kExpectedSingleOutput, list_all_entries(out_dir).size());
        const auto outputs = list_regular_files(out_dir, ".mp4", "transcoded_");
        ASSERT_EQ(kExpectedSingleOutput, outputs.size());

        const av::MediaInfo out_info = probe_valid_media(
            outputs.front(),
            MediaExpectation{
                .video_codec = "h264",
                .audio_codec = "",
                .width = silent_infos[0].width,
                .height = silent_infos[0].height,
                .has_video = true,
                .has_audio = false,
                .expected_duration_sec = silent_infos[0].duration_sec,
                .duration_tolerance_sec = kDurationToleranceSec,
            });
        ASSERT_TRUE(out_info.ok);
        EXPECT_TRUE(out_info.audio_codec.empty());

        verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "silent_single_early.jpg");
        verify_decodable_frame_at(
            outputs.front(), kLateSingleProbeSec, scratch, "silent_single_late.jpg");
        EXPECT_TRUE(silent_snaps[0].is_unchanged());
    }

    {
        SCOPED_TRACE("MultiSilentMixedFpsDirectory");
        const fs::path out_dir = sandbox.create_subdir("out_silent_multi");
        const std::string in_arg = path_to_utf8(silent_dir);
        const std::string out_arg = path_to_utf8(out_dir);
        const int rc = run_tool_app<TranscodeApp>({
            "transcode",
            "-i", in_arg,
            "-o", out_arg,
            "-e", "libx264",
            "-p", "ultrafast",
            "-q", "28",
        });
        ASSERT_EQ(0, rc);

        ASSERT_EQ(kExpectedSingleOutput, list_all_entries(out_dir).size());
        const auto outputs = list_regular_files(out_dir, ".mp4", "transcoded_");
        ASSERT_EQ(kExpectedSingleOutput, outputs.size());

        const av::MediaInfo out_info = probe_valid_media(
            outputs.front(),
            MediaExpectation{
                .video_codec = "h264",
                .audio_codec = "",
                .width = silent_infos[0].width,
                .height = silent_infos[0].height,
                .has_video = true,
                .has_audio = false,
                .expected_fps = kNormalizedFps,
                .fps_tolerance = kFpsTolerance,
                .expected_duration_sec = kJoinedDurationSec,
                .duration_tolerance_sec = kDurationToleranceSec,
            });
        ASSERT_TRUE(out_info.ok);
        EXPECT_TRUE(out_info.audio_codec.empty());

        verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "silent_multi_early.jpg");
        verify_decodable_frame_at(
            outputs.front(), kLateJoinedProbeSec, scratch, "silent_multi_late.jpg");
        EXPECT_TRUE(std::ranges::all_of(silent_snaps, &FileSnapshot::is_unchanged));
        EXPECT_EQ(kExpectedTwoClips, list_all_entries(silent_dir).size());
    }

    {
        SCOPED_TRACE("MixedAudioAndSilentDirectoryRejected");
        const fs::path mixed_av_dir = sandbox.create_subdir("in_mixed_av");
        const fs::path out_dir = sandbox.create_subdir("out_mixed_av");
        const fs::path av_copy =
            sandbox.copy_in(mixed_clips[0], "in_mixed_av/2026-01-01 00-00-00.mp4");
        const fs::path silent_copy =
            sandbox.copy_in(silent_snaps[1].path(), "in_mixed_av/2026-01-02 00-00-00.mp4");

        const av::MediaInfo av_info = probe_valid_media(av_copy);
        const av::MediaInfo silent_copy_info = probe_valid_media(silent_copy);
        ASSERT_TRUE(av_info.ok && av_info.has_audio);
        ASSERT_TRUE(silent_copy_info.ok && !silent_copy_info.has_audio);

        const FileSnapshot av_snap(av_copy);
        const FileSnapshot silent_copy_snap(silent_copy);

        const std::string in_arg = path_to_utf8(mixed_av_dir);
        const std::string out_arg = path_to_utf8(out_dir);
        const int rc = run_tool_app<TranscodeApp>({
            "transcode",
            "-i", in_arg,
            "-o", out_arg,
            "-e", "libx264",
            "-p", "ultrafast",
            "-q", "28",
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Mixed audio/silent rejection leaked stray files in " << path_to_utf8(out_dir);
        EXPECT_TRUE(av_snap.is_unchanged());
        EXPECT_TRUE(silent_copy_snap.is_unchanged());
        EXPECT_EQ(kExpectedTwoClips, list_all_entries(mixed_av_dir).size());
    }
}

TEST(TranscodeComponentTest, UnicodeNonAsciiPathsTranscodeAndJoin) {
    const fs::path clip1 = require_fixture(kFixtureUniformClip1);
    const fs::path clip2 = require_fixture(kFixtureUniformClip2);
    ASSERT_TRUE(fs::is_regular_file(clip1));
    ASSERT_TRUE(fs::is_regular_file(clip2));

    ScopedTempDir sandbox("transcode_unicode");
    const fs::path in_dir = sandbox.create_subdir(kUnicodeInDir);
    const fs::path out_dir = sandbox.create_subdir(kUnicodeOutDir);
    ScopedTempDir scratch("transcode_unicode_frames");

    const fs::path u_clip1 = sandbox.copy_in(clip1, kUnicodeClip1Rel);
    const fs::path u_clip2 = sandbox.copy_in(clip2, kUnicodeClip2Rel);
    const FileSnapshot snap1(u_clip1);
    const FileSnapshot snap2(u_clip2);

    const av::MediaInfo info1 = probe_valid_media(u_clip1);
    const av::MediaInfo info2 = probe_valid_media(u_clip2);
    ASSERT_TRUE(info1.ok);
    ASSERT_TRUE(info2.ok);

    const std::string in_arg = path_to_utf8(in_dir);
    const std::string out_arg = path_to_utf8(out_dir);
    const int rc = run_tool_app<TranscodeApp>({
        "transcode",
        "-i", in_arg,
        "-o", out_arg,
        "-e", "libx264",
        "-p", "ultrafast",
        "-q", "28",
    });
    ASSERT_EQ(0, rc);

    ASSERT_EQ(kExpectedSingleOutput, list_all_entries(out_dir).size());
    const auto outputs = list_regular_files(out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(kExpectedSingleOutput, outputs.size());

    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = "h264",
            .audio_codec = "aac",
            .width = info1.width,
            .height = info1.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = kNormalizedFps,
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = info1.duration_sec + info2.duration_sec,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);

    verify_decodable_frame_at(outputs.front(), kEarlyProbeSec, scratch, "unicode_early.jpg");
    verify_decodable_frame_at(outputs.front(), kLateJoinedProbeSec, scratch, "unicode_late.jpg");
    EXPECT_TRUE(snap1.is_unchanged());
    EXPECT_TRUE(snap2.is_unchanged());
    EXPECT_EQ(kExpectedTwoClips, list_all_entries(in_dir).size());
}

}  // namespace
