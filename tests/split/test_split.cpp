#include "split/split.h"

#include "av/media_probe.h"
#include "concat/concat.h"
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
using test_support::kFixtureMainVideo;
using test_support::kFixtureUniformClip1;
using test_support::list_all_entries;
using test_support::list_regular_files;
using test_support::probe_valid_media;
using test_support::require_fixture;
using test_support::run_tool_app;
using test_support::verify_decodable_frame_at;

constexpr double kFpsTolerance = 1.0;
constexpr double kPartDurationToleranceSec = 2.0;
constexpr double kSumDurationToleranceSec = 2.5;
constexpr double kRoundTripDurationToleranceSec = 2.5;
constexpr double kAbsPoint1Sec = 4.0;
constexpr double kAbsPoint2Sec = 10.0;
constexpr double kAbsPart1MinSec = 2.0;
constexpr double kAbsPart2MinSec = 4.0;
constexpr double kAbsPart3MinSec = 4.0;
constexpr double kRelPoint1Sec = 3.0;
constexpr double kRelPoint2DeltaSec = 5.0;
constexpr double kRelPart1MinSec = 1.0;
constexpr double kRelPart2MinSec = 3.5;
constexpr double kRelPart3MinSec = 6.0;
constexpr double kSingleSplitPointSec = 8.0;
constexpr double kProbeOffsetSec = 0.5;
constexpr double kSeam1ProbeSec = 4.2;
constexpr double kSeam2ProbeSec = 10.2;
constexpr double kEndMarginSec = 0.5;
constexpr std::size_t kMaxFaultArgs = 6;

struct SplitFaultCase {
    std::string_view label;
    bool use_existing_input;
    std::array<std::string_view, kMaxFaultArgs> extra_args;
    std::size_t arg_count;
};

constexpr std::array<SplitFaultCase, 7> kSplitFaultCases{{
    {"NonexistentInput", false, {{"-p", "1"}}, 2},
    {"NegativePoint", true, {{"--points=-2"}}, 1},
    {"ZeroPoint", true, {{"-p", "0"}}, 2},
    {"NonAscendingAbsolutePoints", true, {{"-p", "1.5", "0.5"}}, 3},
    {"DuplicateAbsolutePoints", true, {{"-p", "1", "1"}}, 3},
    {"OutOfRangePoint", true, {{"-p", "999"}}, 2},
    {"RelativeCumulativeOverflow", true, {{"-p", "10", "10", "-r"}}, 4},
}};

[[nodiscard]] fs::path find_single_part(
    const fs::path& dir, std::string_view stem, int part_index) {
    const std::string prefix = std::string(stem) + "_part" + std::to_string(part_index) + "_";
    const auto matches = list_regular_files(dir, ".mp4", prefix);
    EXPECT_EQ(1u, matches.size()) << "Expected exactly 1 file matching " << prefix << "*.mp4 in " << dir;
    return matches.empty() ? fs::path{} : matches.front();
}

TEST(SplitComponentTest, MultiPointAbsoluteSplitPreservesStreamsAndSliceDurations) {
    const fs::path fixture = require_fixture(kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture));

    ScopedTempDir sandbox("split_abs");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("split_abs_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "in/lecture.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    const int rc = run_tool_app<SplitApp>({
        "split",
        "-i", input_copy.string(),
        "-o", out_dir.string(),
        "-p", "4", "10",
    });
    ASSERT_EQ(0, rc);
    EXPECT_TRUE(input_snap.is_unchanged());
    EXPECT_EQ(1u, list_all_entries(in_dir).size());
    ASSERT_EQ(3u, list_all_entries(out_dir).size())
        << "Expected exact 3 split parts with zero stray files";

    const fs::path part1 = find_single_part(out_dir, "lecture", 1);
    const fs::path part2 = find_single_part(out_dir, "lecture", 2);
    const fs::path part3 = find_single_part(out_dir, "lecture", 3);
    ASSERT_FALSE(part1.empty());
    ASSERT_FALSE(part2.empty());
    ASSERT_FALSE(part3.empty());

    const MediaExpectation stream_parity{
        .video_codec = "hevc",
        .audio_codec = "aac",
        .width = src_info.width,
        .height = src_info.height,
        .has_video = true,
        .has_audio = true,
        .expected_fps = src_info.frame_rate.value(),
        .fps_tolerance = kFpsTolerance,
    };

    const av::MediaInfo p1 = probe_valid_media(part1, stream_parity);
    const av::MediaInfo p2 = probe_valid_media(part2, stream_parity);
    const av::MediaInfo p3 = probe_valid_media(part3, stream_parity);
    ASSERT_TRUE(p1.ok && p2.ok && p3.ok);

    EXPECT_GT(p1.duration_sec, kAbsPart1MinSec);
    EXPECT_NEAR(p1.duration_sec, kAbsPoint1Sec, kPartDurationToleranceSec);
    EXPECT_GT(p2.duration_sec, kAbsPart2MinSec);
    EXPECT_NEAR(p2.duration_sec, kAbsPoint2Sec - kAbsPoint1Sec, kPartDurationToleranceSec);
    EXPECT_GT(p3.duration_sec, kAbsPart3MinSec);
    EXPECT_NEAR(p3.duration_sec, src_info.duration_sec - kAbsPoint2Sec, kPartDurationToleranceSec);
    EXPECT_NEAR(p1.duration_sec + p2.duration_sec + p3.duration_sec,
                src_info.duration_sec,
                kSumDurationToleranceSec);

    verify_decodable_frame_at(part1, kProbeOffsetSec, scratch, "abs_p1.jpg");
    verify_decodable_frame_at(part2, kProbeOffsetSec, scratch, "abs_p2.jpg");
    verify_decodable_frame_at(part3, kProbeOffsetSec, scratch, "abs_p3.jpg");
}

TEST(SplitComponentTest, MultiPointRelativeSplitAccumulatesOffsets) {
    const fs::path fixture = require_fixture(kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture));

    ScopedTempDir sandbox("split_rel");
    const fs::path out_dir = sandbox.create_subdir("out");
    ScopedTempDir scratch("split_rel_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "in/relative.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    // Relative -p 3 5 splits at 3s and 8s (3 + 5), yielding a ~5s middle part rather than 2s.
    const int rc = run_tool_app<SplitApp>({
        "split",
        "-i", input_copy.string(),
        "-o", out_dir.string(),
        "-p", "3", "5",
        "-r",
    });
    ASSERT_EQ(0, rc);
    EXPECT_TRUE(input_snap.is_unchanged());
    ASSERT_EQ(3u, list_all_entries(out_dir).size());

    const fs::path part1 = find_single_part(out_dir, "relative", 1);
    const fs::path part2 = find_single_part(out_dir, "relative", 2);
    const fs::path part3 = find_single_part(out_dir, "relative", 3);
    ASSERT_FALSE(part1.empty());
    ASSERT_FALSE(part2.empty());
    ASSERT_FALSE(part3.empty());

    const MediaExpectation stream_parity{
        .video_codec = "hevc",
        .audio_codec = "aac",
        .width = src_info.width,
        .height = src_info.height,
        .has_video = true,
        .has_audio = true,
        .expected_fps = src_info.frame_rate.value(),
        .fps_tolerance = kFpsTolerance,
    };

    const av::MediaInfo p1 = probe_valid_media(part1, stream_parity);
    const av::MediaInfo p2 = probe_valid_media(part2, stream_parity);
    const av::MediaInfo p3 = probe_valid_media(part3, stream_parity);
    ASSERT_TRUE(p1.ok && p2.ok && p3.ok);

    EXPECT_GT(p1.duration_sec, kRelPart1MinSec);
    EXPECT_NEAR(p1.duration_sec, kRelPoint1Sec, kPartDurationToleranceSec);
    EXPECT_GT(p2.duration_sec, kRelPart2MinSec)
        << "Relative mode -p 3 5 -r must accumulate to 8s (5s part2), not cut at absolute 5s";
    EXPECT_NEAR(p2.duration_sec, kRelPoint2DeltaSec, kPartDurationToleranceSec);
    EXPECT_GT(p3.duration_sec, kRelPart3MinSec);
    EXPECT_NEAR(p3.duration_sec,
                src_info.duration_sec - (kRelPoint1Sec + kRelPoint2DeltaSec),
                kPartDurationToleranceSec);
    EXPECT_NEAR(p1.duration_sec + p2.duration_sec + p3.duration_sec,
                src_info.duration_sec,
                kSumDurationToleranceSec);

    verify_decodable_frame_at(part1, kProbeOffsetSec, scratch, "rel_p1.jpg");
    verify_decodable_frame_at(part2, kProbeOffsetSec, scratch, "rel_p2.jpg");
    verify_decodable_frame_at(part3, kProbeOffsetSec, scratch, "rel_p3.jpg");
}

TEST(SplitComponentTest, SplitToConcatProducerConsumerRoundTrip) {
    const fs::path fixture = require_fixture(kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture));

    ScopedTempDir sandbox("split_concat_rt");
    const fs::path parts_dir = sandbox.create_subdir("parts");
    const fs::path joined_dir = sandbox.create_subdir("joined");
    ScopedTempDir scratch("split_concat_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "in/source.mp4");
    const FileSnapshot source_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    const int rc_split = run_tool_app<SplitApp>({
        "split",
        "-i", input_copy.string(),
        "-o", parts_dir.string(),
        "-p", "4", "10",
    });
    ASSERT_EQ(0, rc_split);
    ASSERT_EQ(3u, list_all_entries(parts_dir).size());

    const auto part_files = list_regular_files(parts_dir, ".mp4");
    ASSERT_EQ(3u, part_files.size());
    std::vector<FileSnapshot> part_snaps;
    part_snaps.reserve(part_files.size());
    for (const auto& part : part_files) {
        part_snaps.emplace_back(part);
    }

    const int rc_concat = run_tool_app<ConcatApp>({
        "concat_videos",
        "-i", parts_dir.string(),
        "-o", joined_dir.string(),
    });
    ASSERT_EQ(0, rc_concat);

    ASSERT_EQ(1u, list_all_entries(joined_dir).size());
    const auto joined_outputs = list_regular_files(joined_dir, ".mp4", "concatenated_");
    ASSERT_EQ(1u, joined_outputs.size());

    const av::MediaInfo joined_info = probe_valid_media(
        joined_outputs.front(),
        MediaExpectation{
            .video_codec = "hevc",
            .audio_codec = "aac",
            .width = src_info.width,
            .height = src_info.height,
            .has_video = true,
            .has_audio = true,
            .expected_fps = src_info.frame_rate.value(),
            .fps_tolerance = kFpsTolerance,
            .expected_duration_sec = src_info.duration_sec,
            .duration_tolerance_sec = kRoundTripDurationToleranceSec,
        });
    ASSERT_TRUE(joined_info.ok);

    verify_decodable_frame_at(joined_outputs.front(), kProbeOffsetSec, scratch, "rt_start.jpg");
    verify_decodable_frame_at(joined_outputs.front(), kSeam1ProbeSec, scratch, "rt_seam1.jpg");
    verify_decodable_frame_at(joined_outputs.front(), kSeam2ProbeSec, scratch, "rt_seam2.jpg");
    verify_decodable_frame_at(
        joined_outputs.front(), joined_info.duration_sec - kEndMarginSec, scratch, "rt_end.jpg");

    EXPECT_TRUE(source_snap.is_unchanged());
    EXPECT_TRUE(std::ranges::all_of(part_snaps, &FileSnapshot::is_unchanged));
    EXPECT_EQ(3u, list_all_entries(parts_dir).size());
}

TEST(SplitComponentTest, SameInputOutputDirPreservesOriginalAlongsideParts) {
    const fs::path fixture = require_fixture(kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture));

    ScopedTempDir sandbox("split_same_dir");
    const fs::path same_dir = sandbox.create_subdir("same");
    ScopedTempDir scratch("split_same_frames");

    const fs::path input_copy = sandbox.copy_in(fixture, "same/meeting.mp4");
    const FileSnapshot input_snap(input_copy);
    const av::MediaInfo src_info = probe_valid_media(input_copy);
    ASSERT_TRUE(src_info.ok);

    // Omitting -o writes split parts directly alongside the input video.
    const int rc = run_tool_app<SplitApp>({
        "split",
        "-i", input_copy.string(),
        "-p", "8",
    });
    ASSERT_EQ(0, rc);
    EXPECT_TRUE(input_snap.is_unchanged()) << "Original file was altered during in-place split";

    ASSERT_EQ(3u, list_all_entries(same_dir).size())
        << "Expected original plus 2 split parts with zero stray files";

    const fs::path part1 = find_single_part(same_dir, "meeting", 1);
    const fs::path part2 = find_single_part(same_dir, "meeting", 2);
    ASSERT_FALSE(part1.empty());
    ASSERT_FALSE(part2.empty());

    const MediaExpectation stream_parity{
        .video_codec = "hevc",
        .audio_codec = "aac",
        .width = src_info.width,
        .height = src_info.height,
        .has_video = true,
        .has_audio = true,
        .expected_fps = src_info.frame_rate.value(),
        .fps_tolerance = kFpsTolerance,
    };

    const av::MediaInfo p1 = probe_valid_media(part1, stream_parity);
    const av::MediaInfo p2 = probe_valid_media(part2, stream_parity);
    ASSERT_TRUE(p1.ok && p2.ok);
    EXPECT_NEAR(p1.duration_sec, kSingleSplitPointSec, kPartDurationToleranceSec);
    EXPECT_NEAR(p2.duration_sec, src_info.duration_sec - kSingleSplitPointSec, kPartDurationToleranceSec);
    EXPECT_NEAR(p1.duration_sec + p2.duration_sec, src_info.duration_sec, kSumDurationToleranceSec);

    verify_decodable_frame_at(part1, kProbeOffsetSec, scratch, "same_p1.jpg");
    verify_decodable_frame_at(part2, kProbeOffsetSec, scratch, "same_p2.jpg");
}

TEST(SplitComponentTest, InvalidInputsAndPointsFailWithoutPartialOutputs) {
    const fs::path fixture = require_fixture(kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(fixture));

    ScopedTempDir sandbox("split_faults");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path input_copy = sandbox.copy_in(fixture, "in/short_clip.mp4");
    const FileSnapshot input_snap(input_copy);

    for (const auto& tc : kSplitFaultCases) {
        SCOPED_TRACE(tc.label);
        const fs::path out_dir = sandbox.create_subdir(std::string("out_") + std::string(tc.label));
        const fs::path input_arg = tc.use_existing_input
            ? input_copy
            : sandbox.child("missing_split_video.mp4");

        std::vector<std::string> args = {
            "split",
            "-i", input_arg.string(),
            "-o", out_dir.string(),
        };
        for (const auto extra : std::span{tc.extra_args}.first(tc.arg_count)) {
            args.emplace_back(extra);
        }

        const int rc = run_tool_app<SplitApp>(std::span<const std::string>{args});
        EXPECT_EQ(1, rc);
        EXPECT_TRUE(list_all_entries(out_dir).empty())
            << "Failed split invocation left stray/partial files in " << out_dir;
    }

    EXPECT_TRUE(input_snap.is_unchanged());
    EXPECT_EQ(1u, list_all_entries(in_dir).size());
}

}  // namespace
