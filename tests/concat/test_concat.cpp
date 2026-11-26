#include "concat/concat.h"

#include "av/media_probe.h"
#include "support/cli_runner.hpp"
#include "support/media_assertions.hpp"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using test_support::FileSnapshot;
using test_support::MediaExpectation;
using test_support::ScopedTempDir;
using test_support::kFixtureUniformClip1;
using test_support::kFixtureUniformClip2;
using test_support::list_all_entries;
using test_support::list_regular_files;
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

}  // namespace
