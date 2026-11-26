#include "concat/concat.h"

#include "av/media_probe.h"
#include "av/transcoder.h"
#include "ffmpeg/progress_reporter.h"
#include "support/cli_runner.hpp"
#include "support/media_assertions.hpp"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

using test_support::FileSnapshot;
using test_support::MediaExpectation;
using test_support::ScopedTempDir;
using test_support::kFixtureMainVideo;
using test_support::kFixtureUniformClip1;
using test_support::kFixtureUniformClip2;
using test_support::list_all_entries;
using test_support::list_regular_files;
using test_support::path_to_utf8;
using test_support::probe_valid_media;
using test_support::require_fixture;
using test_support::run_tool_app;
using test_support::to_bytes;
using test_support::verify_decodable_frame_at;

constexpr double kFpsTolerance = 0.5;
constexpr double kDurationToleranceSec = 0.6;
constexpr double kRepeatDurationToleranceSec = 1.0;
constexpr double kStartProbeSec = 0.2;
constexpr double kEndMarginSec = 0.5;
constexpr std::size_t kExpectedSingleOutput = 1;
constexpr std::size_t kExpectedTwoInputs = 2;
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

TEST(ConcatComponentTest, MultiClipLosslessConcatIgnoresNonMp4Siblings) {
    const fs::path clip1 = require_fixture(kFixtureUniformClip1);
    const fs::path clip2 = require_fixture(kFixtureUniformClip2);
    ASSERT_TRUE(fs::is_regular_file(clip1));
    ASSERT_TRUE(fs::is_regular_file(clip2));

    const av::MediaInfo info1 = probe_valid_media(clip1);
    const av::MediaInfo info2 = probe_valid_media(clip2);
    ASSERT_TRUE(info1.ok);
    ASSERT_TRUE(info2.ok);

    ScopedTempDir sandbox("concat_multi");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("concat_multi_frames");

    std::vector<FileSnapshot> input_snaps;
    input_snaps.emplace_back(sandbox.copy_in(clip1, "in/2026-01-01 00-00-00.mp4"));
    input_snaps.emplace_back(sandbox.copy_in(clip2, "in/2026-01-02 00-00-00.mp4"));
    input_snaps.emplace_back(sandbox.copy_in(clip1, "in/2026-01-03 00-00-00.mp4"));
    input_snaps.emplace_back(sandbox.write_text("in/notes.txt", "non-mp4 sibling ignored"));
    input_snaps.emplace_back(
        sandbox.write_bytes("in/thumb.jpg", to_bytes({0xFF, 0xD8, 0xFF, 0xD9})));
    input_snaps.emplace_back(
        sandbox.write_bytes("in/other.mkv", to_bytes({0x1A, 0x45, 0xDF, 0xA3})));

    const int rc = run_tool_app<ConcatApp>({
        "concat_videos",
        "-i", in_dir.string(),
        "-o", out_dir.string(),
    });
    ASSERT_EQ(0, rc);

    const auto out_entries = list_all_entries(out_dir);
    ASSERT_EQ(1u, out_entries.size()) << "Expected exact single output with zero stray temp files";
    const auto outputs = list_regular_files(out_dir, ".mp4", "concatenated_");
    ASSERT_EQ(1u, outputs.size());

    const double expected_duration = info1.duration_sec + info2.duration_sec + info1.duration_sec;
    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = info1.video_codec,
            .audio_codec = info1.audio_codec,
            .width = info1.width,
            .height = info1.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = info1.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = expected_duration,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);

    verify_decodable_frame_at(outputs.front(), kStartProbeSec, scratch, "start.jpg");
    verify_decodable_frame_at(outputs.front(), out_info.duration_sec * 0.5, scratch, "mid.jpg");
    verify_decodable_frame_at(outputs.front(), out_info.duration_sec - kEndMarginSec, scratch, "end.jpg");

    EXPECT_TRUE(std::ranges::all_of(input_snaps, &FileSnapshot::is_unchanged));
    EXPECT_EQ(input_snaps.size(), list_all_entries(in_dir).size());
}

TEST(ConcatComponentTest, ChronologicalFilenameDateSortingAndSameDirRepeatSafety) {
    const fs::path clip1 = require_fixture(kFixtureUniformClip1);
    const fs::path clip2 = require_fixture(kFixtureUniformClip2);
    ASSERT_TRUE(fs::is_regular_file(clip1));
    ASSERT_TRUE(fs::is_regular_file(clip2));

    const av::MediaInfo info1 = probe_valid_media(clip1);
    const av::MediaInfo info2 = probe_valid_media(clip2);
    ASSERT_TRUE(info1.ok);
    ASSERT_TRUE(info2.ok);

    ScopedTempDir sandbox("concat_same_dir");
    const fs::path same_dir = sandbox.create_subdir("same");
    ScopedTempDir scratch("concat_same_frames");

    // Alphabetical prefixes intentionally disagree with embedded YYYY-MM-DD HH-MM-SS dates,
    // and the clip pattern [clip1, clip1, clip2] is asymmetric so chronological vs alphabetical
    // order produces a structurally distinct sequence.
    std::vector<FileSnapshot> original_snaps;
    original_snaps.emplace_back(sandbox.copy_in(clip1, "same/zebra_2026-01-01 08-00-00.mp4"));
    original_snaps.emplace_back(sandbox.copy_in(clip1, "same/middle_2026-01-02 09-00-00.mp4"));
    original_snaps.emplace_back(sandbox.copy_in(clip2, "same/alpha_2026-01-03 10-00-00.mp4"));

    const int rc_pass1 = run_tool_app<ConcatApp>({
        "concat_videos",
        "-i", same_dir.string(),
        "-o", same_dir.string(),
    });
    ASSERT_EQ(0, rc_pass1);
    EXPECT_TRUE(std::ranges::all_of(original_snaps, &FileSnapshot::is_unchanged));

    ASSERT_EQ(4u, list_all_entries(same_dir).size())
        << "Expected 3 originals plus 1 concatenated_*.mp4 with zero stray temp files";
    const auto pass1_outputs = list_regular_files(same_dir, ".mp4", "concatenated_");
    ASSERT_EQ(1u, pass1_outputs.size());

    const double pass1_expected_dur = info1.duration_sec + info2.duration_sec + info1.duration_sec;
    const av::MediaInfo pass1_info = probe_valid_media(
        pass1_outputs.front(),
        MediaExpectation{
            .video_codec = info1.video_codec,
            .audio_codec = info1.audio_codec,
            .width = info1.width,
            .height = info1.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = info1.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = pass1_expected_dur,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(pass1_info.ok);
    verify_decodable_frame_at(pass1_outputs.front(), kStartProbeSec, scratch, "pass1_start.jpg");
    verify_decodable_frame_at(
        pass1_outputs.front(), pass1_info.duration_sec - kEndMarginSec, scratch, "pass1_end.jpg");

    const fs::path first_output = pass1_outputs.front();
    const FileSnapshot first_output_snap(first_output);

    const int rc_pass2 = run_tool_app<ConcatApp>({
        "concat_videos",
        "-i", same_dir.string(),
        "-o", same_dir.string(),
    });
    ASSERT_EQ(0, rc_pass2);
    EXPECT_TRUE(std::ranges::all_of(original_snaps, &FileSnapshot::is_unchanged));
    EXPECT_TRUE(first_output_snap.is_unchanged())
        << "Repeat invocation modified prior concatenated output";

    ASSERT_EQ(5u, list_all_entries(same_dir).size())
        << "Expected 3 originals plus 2 concatenated_*.mp4 files with zero stray files";
    const auto pass2_outputs = list_regular_files(same_dir, ".mp4", "concatenated_");
    ASSERT_EQ(2u, pass2_outputs.size());

    const auto new_it = std::ranges::find_if(
        pass2_outputs, [&](const fs::path& p) { return p != first_output; });
    ASSERT_NE(new_it, pass2_outputs.end());

    const av::MediaInfo pass2_info = probe_valid_media(
        *new_it,
        MediaExpectation{
            .video_codec = info1.video_codec,
            .audio_codec = info1.audio_codec,
            .width = info1.width,
            .height = info1.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = info1.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = pass1_expected_dur + pass1_info.duration_sec,
            .duration_tolerance_sec = kRepeatDurationToleranceSec,
        });
    ASSERT_TRUE(pass2_info.ok);
    verify_decodable_frame_at(*new_it, kStartProbeSec, scratch, "pass2_start.jpg");
    verify_decodable_frame_at(*new_it, pass2_info.duration_sec * 0.5, scratch, "pass2_mid.jpg");
    verify_decodable_frame_at(*new_it, pass2_info.duration_sec - kEndMarginSec, scratch, "pass2_end.jpg");
}

TEST(ConcatComponentTest, FaultCasesAndMidBatchCorruptInputLeaveZeroPartialOutputs) {
    const fs::path clip1 = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(clip1));

    ScopedTempDir sandbox("concat_faults");

    {
        SCOPED_TRACE("NonexistentInputDir");
        const fs::path out_dir = sandbox.create_subdir("out_nonexistent");
        const int rc = run_tool_app<ConcatApp>({
            "concat_videos",
            "-i", sandbox.child("does_not_exist").string(),
            "-o", out_dir.string(),
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty());
    }

    {
        SCOPED_TRACE("EmptyInputDir");
        const fs::path empty_in = sandbox.create_subdir("in_empty");
        const fs::path out_dir = sandbox.create_subdir("out_empty");
        const int rc = run_tool_app<ConcatApp>({
            "concat_videos",
            "-i", empty_in.string(),
            "-o", out_dir.string(),
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty());
    }

    {
        SCOPED_TRACE("SingleMp4InputDir");
        const fs::path single_in = sandbox.create_subdir("in_single");
        const fs::path out_dir = sandbox.create_subdir("out_single");
        const FileSnapshot only_clip(sandbox.copy_in(clip1, "in_single/2026-01-01 00-00-00.mp4"));
        (void)sandbox.write_text("in_single/readme.txt", "not an mp4");

        const int rc = run_tool_app<ConcatApp>({
            "concat_videos",
            "-i", single_in.string(),
            "-o", out_dir.string(),
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty());
        EXPECT_TRUE(only_clip.is_unchanged());
    }

    {
        SCOPED_TRACE("MidBatchCorruptSecondMp4");
        const fs::path corrupt_in = sandbox.create_subdir("in_corrupt");
        const fs::path out_dir = sandbox.create_subdir("out_corrupt");
        const FileSnapshot valid_clip(sandbox.copy_in(clip1, "in_corrupt/2026-01-01 00-00-00.mp4"));
        (void)sandbox.write_bytes(
            "in_corrupt/2026-01-02 00-00-00.mp4",
            to_bytes({0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70, 0xFF, 0xFE, 0x00, 0x01}));

        const int rc = run_tool_app<ConcatApp>({
            "concat_videos",
            "-i", corrupt_in.string(),
            "-o", out_dir.string(),
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Mid-batch corrupt input leaked a partial output or temp file";
        EXPECT_TRUE(valid_clip.is_unchanged());
    }
}

TEST(ConcatComponentTest, RejectsMismatchedCodecsAndAudioPresenceWithoutPartialOutputs) {
    const fs::path main_video = require_fixture(kFixtureMainVideo);
    const fs::path uniform_clip = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(main_video));
    ASSERT_TRUE(fs::is_regular_file(uniform_clip));

    const av::MediaInfo main_info = probe_valid_media(main_video);
    const av::MediaInfo clip_info = probe_valid_media(uniform_clip);
    ASSERT_TRUE(main_info.ok);
    ASSERT_TRUE(clip_info.ok);
    ASSERT_NE(main_info.video_codec, clip_info.video_codec);

    ScopedTempDir sandbox("concat_mismatch");

    {
        SCOPED_TRACE("MismatchedCodecsAndResolution");
        const fs::path in_dir = sandbox.create_subdir("in_codec");
        const fs::path out_dir = sandbox.create_subdir("out_codec");
        const FileSnapshot snap_main(
            sandbox.copy_in(main_video, "in_codec/2026-01-01 00-00-00.mp4"));
        const FileSnapshot snap_clip(
            sandbox.copy_in(uniform_clip, "in_codec/2026-01-02 00-00-00.mp4"));

        const std::string in_arg = path_to_utf8(in_dir);
        const std::string out_arg = path_to_utf8(out_dir);
        const int rc = run_tool_app<ConcatApp>({
            "concat_videos",
            "-i", in_arg,
            "-o", out_arg,
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Mismatched codec concat leaked a partial output or temp file";
        EXPECT_TRUE(snap_main.is_unchanged());
        EXPECT_TRUE(snap_clip.is_unchanged());
        EXPECT_EQ(kExpectedTwoInputs, list_all_entries(in_dir).size());
    }

    {
        SCOPED_TRACE("MismatchedAudioPresenceSameCodecAndResolution");
        const fs::path in_dir = sandbox.create_subdir("in_audio");
        const fs::path out_dir = sandbox.create_subdir("out_audio");
        const fs::path av_clip = sandbox.copy_in(uniform_clip, "in_audio/2026-01-01 00-00-00.mp4");
        const fs::path silent_clip = in_dir / "2026-01-02 00-00-00.mp4";
        ASSERT_TRUE(create_silent_h264_clip(uniform_clip, silent_clip, clip_info.duration_sec));

        const av::MediaInfo silent_info = probe_valid_media(
            silent_clip,
            MediaExpectation{
                .video_codec = clip_info.video_codec,
                .audio_codec = "",
                .width = clip_info.width,
                .height = clip_info.height,
                .has_video = true,
                .has_audio = false,
            });
        ASSERT_TRUE(silent_info.ok);
        ASSERT_FALSE(silent_info.has_audio);
        ASSERT_TRUE(silent_info.audio_codec.empty());

        const FileSnapshot snap_av(av_clip);
        const FileSnapshot snap_silent(silent_clip);

        const std::string in_arg = path_to_utf8(in_dir);
        const std::string out_arg = path_to_utf8(out_dir);
        const int rc = run_tool_app<ConcatApp>({
            "concat_videos",
            "-i", in_arg,
            "-o", out_arg,
        });
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Audio/video + silent concat leaked a partial output or temp file";
        EXPECT_TRUE(snap_av.is_unchanged());
        EXPECT_TRUE(snap_silent.is_unchanged());
        EXPECT_EQ(kExpectedTwoInputs, list_all_entries(in_dir).size());
    }
}

TEST(ConcatComponentTest, UnicodeNonAsciiPathsConcatenateLosslessly) {
    const fs::path clip1 = require_fixture(kFixtureUniformClip1);
    const fs::path clip2 = require_fixture(kFixtureUniformClip2);
    ASSERT_TRUE(fs::is_regular_file(clip1));
    ASSERT_TRUE(fs::is_regular_file(clip2));

    ScopedTempDir sandbox("concat_unicode");
    const fs::path in_dir = sandbox.create_subdir(kUnicodeInDir);
    const fs::path out_dir = sandbox.create_subdir(kUnicodeOutDir);
    ScopedTempDir scratch("concat_unicode_frames");

    const fs::path u_clip1 = sandbox.copy_in(clip1, kUnicodeClip1Rel);
    const fs::path u_clip2 = sandbox.copy_in(clip2, kUnicodeClip2Rel);
    const FileSnapshot snap1(u_clip1);
    const FileSnapshot snap2(u_clip2);

    const av::MediaInfo info1 = probe_valid_media(u_clip1);
    const av::MediaInfo info2 = probe_valid_media(u_clip2);
    ASSERT_TRUE(info1.ok);
    ASSERT_TRUE(info2.ok);
    ASSERT_TRUE(info1.has_video && info1.has_audio);
    ASSERT_TRUE(info2.has_video && info2.has_audio);

    const std::string in_arg = path_to_utf8(in_dir);
    const std::string out_arg = path_to_utf8(out_dir);
    const int rc = run_tool_app<ConcatApp>({
        "concat_videos",
        "-i", in_arg,
        "-o", out_arg,
    });
    ASSERT_EQ(0, rc);

    const auto out_entries = list_all_entries(out_dir);
    ASSERT_EQ(kExpectedSingleOutput, out_entries.size())
        << "Expected exact single output with zero stray temp files";
    const auto outputs = list_regular_files(out_dir, ".mp4", "concatenated_");
    ASSERT_EQ(kExpectedSingleOutput, outputs.size());

    const av::MediaInfo out_info = probe_valid_media(
        outputs.front(),
        MediaExpectation{
            .video_codec = info1.video_codec,
            .audio_codec = info1.audio_codec,
            .width = info1.width,
            .height = info1.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = info1.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = info1.duration_sec + info2.duration_sec,
            .duration_tolerance_sec = kDurationToleranceSec,
        });
    ASSERT_TRUE(out_info.ok);

    verify_decodable_frame_at(outputs.front(), kStartProbeSec, scratch, "unicode_start.jpg");
    verify_decodable_frame_at(
        outputs.front(), out_info.duration_sec - kEndMarginSec, scratch, "unicode_end.jpg");

    EXPECT_TRUE(snap1.is_unchanged());
    EXPECT_TRUE(snap2.is_unchanged());
    EXPECT_EQ(kExpectedTwoInputs, list_all_entries(in_dir).size());
}

}  // namespace
