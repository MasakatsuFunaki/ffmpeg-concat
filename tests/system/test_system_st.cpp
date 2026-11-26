#include "tool_paths.hpp"

#include "support/cli_runner.hpp"
#include "support/media_assertions.hpp"
#include "support/test_sandbox.hpp"

#include "av/frame_grabber.h"
#include "av/media_probe.h"
#include "av/transcoder.h"
#include "ffmpeg/progress_reporter.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kMainVideoWidth = 1920;
constexpr int kMainVideoHeight = 1080;
constexpr double kExpectedFps = 30.0;
constexpr double kFpsTolerance = 1.0;

constexpr double kSplitFirstSec = 3.0;
constexpr double kSplitSecondRelSec = 5.0;
constexpr double kSplitCumulativeSec = kSplitFirstSec + kSplitSecondRelSec;
constexpr double kSplitSecondMinSec = 3.5;
constexpr double kSplitPartTolSec = 1.5;
constexpr double kSplitSumMatchTolSec = 1.0;
constexpr double kSplitRoundTripTolSec = 2.0;
constexpr std::size_t kExpectedSplitPartCount = 3u;

constexpr double kUnicodeSplitFirstSec = 4.0;
constexpr double kUnicodeSplitSecondSec = 10.0;
constexpr double kUnicodeSplitPartTolSec = 2.0;
constexpr double kUnicodeRoundTripTolSec = 2.5;
constexpr double kUnicodeCutRemovedSec = 3.0;
constexpr double kUnicodeCutMinShortenSec = 1.0;
constexpr std::size_t kUnicodeInputCount = 2u;

constexpr double kFrameProbeStartSec = 0.5;
constexpr double kFrameProbeEndMarginSec = 1.0;

constexpr double kCutMinDurationSec = 9.5;
constexpr double kCutMaxDurationSec = 14.5;
constexpr double kCutRemovedTotalSec = 4.0;
constexpr double kCutDurationTolSec = 2.0;
constexpr double kCutProbeStartSec = 1.0;

constexpr double kBlurDurationTolSec = 0.5;
constexpr double kBlurSampleSec = 1.0;

constexpr std::size_t kExtractImageCount = 3u;
constexpr std::size_t kExtractMosaicMinStriking = 2u;
constexpr std::size_t kMaxStrikingFrames = 100u;
constexpr std::size_t kMosaicDirExpectedEntries = 2u;
constexpr std::size_t kSha256HexLength = 64u;
constexpr double kMosaicMinStdDev = 1.0;

constexpr double kTranscodeExpectedDurationSec = 4.0;
constexpr double kTranscodeDurationTolSec = 0.6;
constexpr std::size_t kTranscodeMinInputClips = 2u;
constexpr std::size_t kTranscodeExtractCount = 4u;
constexpr std::size_t kSilentExtractImageCount = 3u;
constexpr std::size_t kMismatchInputCount = 2u;

constexpr int kExpectedSuccessExitCode = 0;
constexpr int kExpectedFailureExitCode = 1;

constexpr std::array<std::string_view, 2> kForbiddenTempDirNames{{
    "_temp_candidates",
    "_temp_text",
}};

constexpr std::array<std::string_view, 7> kStreamMismatchDiagnosticTerms{{
    "mismatch",
    "incompatible",
    "codec",
    "resolution",
    "dimension",
    "hevc",
    "h264",
}};

enum class ToolId : std::size_t {
    kConcat = 0,
    kTranscode = 1,
    kCut = 2,
    kSplit = 3,
    kExtract = 4,
};

struct ToolContract {
    ToolId id;
    std::string_view name;
    std::string_view target_path;
    std::array<std::string_view, 6> required_help_flags;
};

constexpr std::array<ToolContract, 5> kToolContracts{{
    {ToolId::kConcat,
     "concat_videos",
     test_support::tool_paths::kConcat,
     {{"--input", "--output", "--help", "", "", ""}}},
    {ToolId::kTranscode,
     "transcode",
     test_support::tool_paths::kTranscode,
     {{"--input", "--output", "--crf", "--encoder", "--preset", "--help"}}},
    {ToolId::kCut,
     "cut",
     test_support::tool_paths::kCut,
     {{"--input", "--output", "--start", "--end", "--blur", "--sound-off"}}},
    {ToolId::kSplit,
     "split",
     test_support::tool_paths::kSplit,
     {{"--input", "--output", "--points", "--relative", "--help", ""}}},
    {ToolId::kExtract,
     "extract",
     test_support::tool_paths::kExtract,
     {{"--input", "--output", "--image", "--image-detect", "--mosaic", "--extract-text"}}},
}};

[[nodiscard]] constexpr const ToolContract& contract_for(ToolId id) noexcept {
    return kToolContracts[static_cast<std::size_t>(id)];
}

[[nodiscard]] test_support::ProcessOptions isolated_process_options(const fs::path& cwd) {
    test_support::ProcessOptions opts;
    opts.working_dir = cwd;
    opts.clear_path = true;
    return opts;
}

[[nodiscard]] std::string format_process_diagnostics(
    std::string_view tool_name, const test_support::ProcessResult& res) {
    return std::format(
        "tool={} exit_code={} timed_out={}\n--- stdout ---\n{}\n--- stderr ---\n{}",
        tool_name, res.exit_code, res.timed_out, res.stdout_text, res.stderr_text);
}

[[nodiscard]] std::string format_missing_tool_diagnostic(
    std::string_view tool_name, const fs::path& resolved_path) {
    const auto env_dir = test_support::get_env_var("FFMPEG_TOOLS_BIN_DIR");
    return std::format(
        "Required CLI executable missing for '{}' at '{}' (FFMPEG_TOOLS_BIN_DIR={})",
        tool_name, test_support::path_to_utf8(resolved_path), env_dir ? *env_dir : "<unset>");
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1u);
}

[[nodiscard]] constexpr bool is_hex_digit(char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

[[nodiscard]] bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto equal_ci = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return !std::ranges::search(haystack, needle, equal_ci).empty();
}

[[nodiscard]] bool contains_any_case_insensitive(
    std::string_view haystack, std::span<const std::string_view> needles) {
    return std::ranges::any_of(needles, [haystack](std::string_view needle) {
        return contains_case_insensitive(haystack, needle);
    });
}

[[nodiscard]] bool is_valid_dir_fingerprint(
    std::string_view text, std::size_t expected_entries) noexcept {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const auto line = trim_ascii(
            nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos));
        if (!line.empty()) {
            const auto colon = line.rfind(':');
            if (colon == std::string_view::npos || colon == 0) {
                return false;
            }
            const auto hex = line.substr(colon + 1);
            if (hex.size() != kSha256HexLength || !std::ranges::all_of(hex, is_hex_digit)) {
                return false;
            }
            ++count;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return count == expected_entries;
}

[[nodiscard]] test_support::MediaExpectation make_lossless_main_expectation(
    std::optional<double> expected_duration = std::nullopt,
    double duration_tol = kSplitPartTolSec) {
    test_support::MediaExpectation exp;
    exp.video_codec = "hevc";
    exp.audio_codec = "aac";
    exp.width = kMainVideoWidth;
    exp.height = kMainVideoHeight;
    exp.has_video = true;
    exp.has_audio = true;
    exp.expected_fps = kExpectedFps;
    exp.fps_tolerance = kFpsTolerance;
    exp.expected_duration_sec = expected_duration;
    exp.duration_tolerance_sec = duration_tol;
    return exp;
}

[[nodiscard]] cv::Mat decode_and_verify_jpeg(
    const fs::path& jpg_path, int expected_width, int expected_height) {
    const std::string display = test_support::path_to_utf8(jpg_path);
    EXPECT_TRUE(fs::is_regular_file(jpg_path)) << "Missing JPEG: " << display;
    if (!fs::is_regular_file(jpg_path)) {
        return {};
    }
    const std::vector<std::byte> raw = test_support::read_file_bytes(jpg_path);
    EXPECT_FALSE(raw.empty()) << "Empty JPEG file: " << display;
    if (raw.empty()) {
        return {};
    }
    std::vector<std::uint8_t> encoded(raw.size());
    std::ranges::transform(raw, encoded.begin(), [](std::byte b) noexcept {
        return std::to_integer<std::uint8_t>(b);
    });
    const cv::Mat img = cv::imdecode(encoded, cv::IMREAD_COLOR);
    EXPECT_FALSE(img.empty()) << "Failed to decode JPEG: " << display;
    if (!img.empty()) {
        EXPECT_EQ(img.cols, expected_width) << "JPEG width mismatch: " << display;
        EXPECT_EQ(img.rows, expected_height) << "JPEG height mismatch: " << display;
    }
    return img;
}

void verify_all_jpegs_match_dimensions(
    std::span<const fs::path> jpg_files, int expected_width, int expected_height) {
    for (const auto& jpg : jpg_files) {
        SCOPED_TRACE(test_support::path_to_utf8(jpg));
        const cv::Mat decoded = decode_and_verify_jpeg(jpg, expected_width, expected_height);
        EXPECT_FALSE(decoded.empty());
    }
}

void verify_extract_image_detect_mosaic(
    const fs::path& stem_dir,
    int expected_width,
    int expected_height,
    std::size_t expected_image_count) {
    const fs::path image_dir = stem_dir / "image";
    const fs::path detect_dir = stem_dir / "detect";
    const fs::path mosaics_dir = stem_dir / "mosaics";

    EXPECT_EQ(test_support::list_all_entries(image_dir).size(), expected_image_count);
    const auto image_files = test_support::list_regular_files(image_dir, ".jpg", "frame_");
    ASSERT_EQ(image_files.size(), expected_image_count);
    verify_all_jpegs_match_dimensions(image_files, expected_width, expected_height);

    const auto striking_files = test_support::list_regular_files(detect_dir, ".jpg", "striking_");
    ASSERT_GE(striking_files.size(), kExtractMosaicMinStriking);
    ASSERT_LE(striking_files.size(), kMaxStrikingFrames);
    verify_all_jpegs_match_dimensions(striking_files, expected_width, expected_height);

    const fs::path detect_hash_file = detect_dir / ".detect_hash";
    ASSERT_TRUE(fs::is_regular_file(detect_hash_file));
    EXPECT_TRUE(is_valid_dir_fingerprint(
        test_support::read_file_text(detect_hash_file), striking_files.size()));
    EXPECT_EQ(test_support::list_all_entries(detect_dir).size(), striking_files.size() + 1u);

    const fs::path mosaic_jpg = mosaics_dir / "mosaic.jpg";
    const fs::path mosaic_hash_file = mosaics_dir / ".mosaic_hash";
    EXPECT_EQ(test_support::list_all_entries(mosaics_dir).size(), kMosaicDirExpectedEntries);
    ASSERT_TRUE(fs::is_regular_file(mosaic_hash_file));
    EXPECT_FALSE(trim_ascii(test_support::read_file_text(mosaic_hash_file)).empty());

    const cv::Mat mosaic = decode_and_verify_jpeg(mosaic_jpg, kMainVideoWidth, kMainVideoHeight);
    ASSERT_FALSE(mosaic.empty());
    cv::Scalar mean_val;
    cv::Scalar stddev_val;
    cv::meanStdDev(mosaic, mean_val, stddev_val);
    const double max_channel_stddev = std::max({stddev_val[0], stddev_val[1], stddev_val[2]});
    EXPECT_GT(max_channel_stddev, kMosaicMinStdDev)
        << "Mosaic image must contain non-uniform rendered frame content";
}

void assert_no_temp_dirs_recursive(const fs::path& root) {
    if (!fs::exists(root)) {
        return;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = test_support::path_to_utf8(entry.path().filename());
        const bool is_forbidden = std::ranges::any_of(
            kForbiddenTempDirNames,
            [&name](std::string_view forbidden) noexcept { return name == forbidden; });
        EXPECT_FALSE(is_forbidden)
            << "Leftover temporary directory found: " << test_support::path_to_utf8(entry.path());
    }
}

[[nodiscard]] av::MediaInfo generate_silent_clip(const fs::path& src, const fs::path& dst) {
    const av::TranscodeRequest req{
        .inputs = {src},
        .output = dst,
        .filter_description = "[in0v]null[outv]",
        .video_encoder = "libx264",
        .video_options = {{"crf", "28"}, {"preset", "ultrafast"}},
        .audio_copy = false,
    };
    NullProgressReporter reporter;
    const bool ok = av::transcode(req, reporter, 0.0);
    EXPECT_TRUE(ok) << "Failed to generate silent clip from "
                    << test_support::path_to_utf8(src) << " to "
                    << test_support::path_to_utf8(dst);

    test_support::MediaExpectation silent_exp;
    silent_exp.video_codec = "h264";
    silent_exp.has_video = true;
    silent_exp.has_audio = false;
    const av::MediaInfo info = test_support::probe_valid_media(dst, silent_exp);
    EXPECT_TRUE(info.audio_codec.empty())
        << "Expected empty audio_codec for silent clip: " << test_support::path_to_utf8(dst);
    return info;
}

[[nodiscard]] std::vector<test_support::FileSnapshot> copy_directory_mp4s(
    const fs::path& src_dir, const fs::path& dst_dir) {
    const auto sources = test_support::list_regular_files(src_dir, ".mp4");
    std::vector<test_support::FileSnapshot> snaps;
    snaps.reserve(sources.size());
    for (const auto& src : sources) {
        const fs::path dst = dst_dir / src.filename();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        snaps.emplace_back(dst);
    }
    return snaps;
}

void assert_all_unchanged(std::span<const test_support::FileSnapshot> snaps) {
    for (const auto& snap : snaps) {
        EXPECT_TRUE(snap.is_unchanged())
            << "Input file was unexpectedly modified: " << test_support::path_to_utf8(snap.path());
    }
}

struct PinnedArtifact {
    fs::path path;
    fs::file_time_type pinned_mtime{};
    test_support::ContentDigest digest{};
};

[[nodiscard]] PinnedArtifact pin_artifact(const fs::path& file) {
    return PinnedArtifact{
        .path = file,
        .pinned_mtime = test_support::pin_mtime_in_past(file),
        .digest = test_support::digest_file(file),
    };
}

class SystemTest : public ::testing::Test {
protected:
    [[nodiscard]] fs::path resolve_tool(ToolId id) const {
        const auto& contract = contract_for(id);
        return test_support::resolve_tool_executable(contract.name, contract.target_path);
    }

    [[nodiscard]] test_support::ProcessResult run_tool(
        ToolId id, std::initializer_list<std::string> args, const fs::path& cwd) const {
        const auto& contract = contract_for(id);
        const fs::path exe = resolve_tool(id);
        EXPECT_TRUE(fs::is_regular_file(exe))
            << format_missing_tool_diagnostic(contract.name, exe);
        const std::vector<std::string> owned(args.begin(), args.end());
        return test_support::run_process(
            exe, std::span<const std::string>{owned}, isolated_process_options(cwd));
    }
};

TEST_F(SystemTest, PackagedStandaloneExecutablesAndCliContract) {
    const test_support::ScopedTempDir sandbox("st_cli_contract");
    const auto is_non_empty = [](std::string_view sv) noexcept { return !sv.empty(); };

    for (const auto& contract : kToolContracts) {
        SCOPED_TRACE(std::string(contract.name));

        const fs::path exe = resolve_tool(contract.id);
        const std::string exe_display = test_support::path_to_utf8(exe);
        ASSERT_TRUE(fs::is_regular_file(exe))
            << format_missing_tool_diagnostic(contract.name, exe);
        EXPECT_GT(fs::file_size(exe), 0u) << "Executable is empty: " << exe_display;

        const fs::path bin_dir = exe.parent_path();
        EXPECT_FALSE(fs::exists(bin_dir / "ffmpeg"))
            << "External ffmpeg binary must not be shipped alongside " << exe_display;
        EXPECT_FALSE(fs::exists(bin_dir / "ffmpeg.exe"))
            << "External ffmpeg.exe binary must not be shipped alongside " << exe_display;

        const auto res = run_tool(contract.id, {"--help"}, sandbox.path());
        ASSERT_FALSE(res.timed_out) << format_process_diagnostics(contract.name, res);
        ASSERT_EQ(res.exit_code, kExpectedSuccessExitCode)
            << format_process_diagnostics(contract.name, res);
        EXPECT_TRUE(res.stderr_text.empty())
            << "Unexpected stderr on --help: " << format_process_diagnostics(contract.name, res);

        for (const auto flag : contract.required_help_flags | std::views::filter(is_non_empty)) {
            EXPECT_NE(res.stdout_text.find(flag), std::string::npos)
                << "Missing documented option '" << flag << "' in " << contract.name
                << " --help output:\n"
                << res.stdout_text;
        }
    }
}

TEST_F(SystemTest, SplitToConcatLosslessRoundTripPipeline) {
    const fs::path fixture = test_support::require_fixture(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(fixture));

    const test_support::ScopedTempDir sandbox("st_split_concat");
    const fs::path src_dir = sandbox.create_subdir("source");
    const fs::path parts_dir = sandbox.create_subdir("parts");
    const fs::path joined_dir = sandbox.create_subdir("joined");
    const test_support::ScopedTempDir frame_scratch("st_split_concat_frames");

    const fs::path main_copy = src_dir / fixture.filename();
    fs::copy_file(fixture, main_copy, fs::copy_options::overwrite_existing);
    const test_support::FileSnapshot main_snap(main_copy);

    const av::MediaInfo src_info =
        test_support::probe_valid_media(main_copy, make_lossless_main_expectation());
    ASSERT_TRUE(src_info.ok);
    ASSERT_GT(src_info.duration_sec, kSplitCumulativeSec + kSplitPartTolSec);

    const auto split_res = run_tool(
        ToolId::kSplit,
        {"-i", test_support::path_to_utf8(main_copy),
         "-o", test_support::path_to_utf8(parts_dir),
         "-p", "3", "5", "-r"},
        sandbox.path());
    ASSERT_FALSE(split_res.timed_out) << format_process_diagnostics("split", split_res);
    ASSERT_EQ(split_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("split", split_res);

    EXPECT_EQ(test_support::list_all_entries(parts_dir).size(), kExpectedSplitPartCount);
    const auto parts = test_support::list_regular_files(parts_dir, ".mp4");
    ASSERT_EQ(parts.size(), kExpectedSplitPartCount);

    const std::array<double, kExpectedSplitPartCount> expected_part_durations{{
        kSplitFirstSec,
        kSplitSecondRelSec,
        src_info.duration_sec - kSplitCumulativeSec,
    }};

    double sum_part_durations = 0.0;
    std::vector<test_support::FileSnapshot> part_snaps;
    part_snaps.reserve(parts.size());

    for (std::size_t idx = 0; idx < parts.size(); ++idx) {
        const auto& part = parts[idx];
        SCOPED_TRACE(test_support::path_to_utf8(part));

        const std::string expected_prefix = std::format(
            "{}_part{}_", test_support::path_to_utf8(main_copy.stem()), idx + 1u);
        const std::string part_filename = test_support::path_to_utf8(part.filename());
        EXPECT_TRUE(part_filename.starts_with(expected_prefix))
            << "Unexpected split part filename: " << part_filename;

        const av::MediaInfo part_info = test_support::probe_valid_media(
            part,
            make_lossless_main_expectation(expected_part_durations[idx], kSplitPartTolSec));
        ASSERT_TRUE(part_info.ok);

        if (idx == 1u) {
            EXPECT_GT(part_info.duration_sec, kSplitSecondMinSec)
                << "Part 2 duration indicates -r relative accumulation did not apply";
        }

        sum_part_durations += part_info.duration_sec;
        part_snaps.emplace_back(part);
    }

    const auto concat_res = run_tool(
        ToolId::kConcat,
        {"-i", test_support::path_to_utf8(parts_dir),
         "-o", test_support::path_to_utf8(joined_dir)},
        sandbox.path());
    ASSERT_FALSE(concat_res.timed_out) << format_process_diagnostics("concat_videos", concat_res);
    ASSERT_EQ(concat_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("concat_videos", concat_res);

    EXPECT_EQ(test_support::list_all_entries(joined_dir).size(), 1u);
    const auto joined_files = test_support::list_regular_files(joined_dir, ".mp4", "concatenated_");
    ASSERT_EQ(joined_files.size(), 1u);

    const fs::path& joined_video = joined_files.front();
    const av::MediaInfo joined_info = test_support::probe_valid_media(
        joined_video,
        make_lossless_main_expectation(src_info.duration_sec, kSplitRoundTripTolSec));
    ASSERT_TRUE(joined_info.ok);
    EXPECT_NEAR(joined_info.duration_sec, sum_part_durations, kSplitSumMatchTolSec);

    const double mid_ts = joined_info.duration_sec * 0.5;
    const double end_ts = std::max(kFrameProbeStartSec,
                                   joined_info.duration_sec - kFrameProbeEndMarginSec);
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        joined_video, kFrameProbeStartSec, frame_scratch, "joined_start.jpg"));
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        joined_video, mid_ts, frame_scratch, "joined_mid.jpg"));
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        joined_video, end_ts, frame_scratch, "joined_end.jpg"));

    EXPECT_TRUE(main_snap.is_unchanged());
    EXPECT_EQ(test_support::list_all_entries(parts_dir).size(), kExpectedSplitPartCount);
    assert_all_unchanged(part_snaps);
}

TEST_F(SystemTest, CutBlurAndStreamCopyToExtractPipeline) {
    const fs::path main_fixture = test_support::require_fixture(test_support::kFixtureMainVideo);
    const fs::path clip_fixture = test_support::require_fixture(test_support::kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(main_fixture));
    ASSERT_TRUE(fs::is_regular_file(clip_fixture));

    const test_support::ScopedTempDir sandbox("st_cut_extract");
    const fs::path src_main_dir = sandbox.create_subdir("src_main");
    const fs::path cut_out_dir = sandbox.create_subdir("cut_out");
    const fs::path src_clip_dir = sandbox.create_subdir("src_clip");
    const fs::path blur_out_dir = sandbox.create_subdir("blur_out");
    const fs::path extract_out_dir = sandbox.create_subdir("extract_out");
    const test_support::ScopedTempDir frame_scratch("st_cut_frames");

    const fs::path main_copy = src_main_dir / main_fixture.filename();
    fs::copy_file(main_fixture, main_copy, fs::copy_options::overwrite_existing);
    const test_support::FileSnapshot main_snap(main_copy);
    const av::MediaInfo main_info = test_support::probe_valid_media(main_copy);
    ASSERT_TRUE(main_info.ok);

    const fs::path cut_output = cut_out_dir / "trimmed.mp4";
    const auto cut_res = run_tool(
        ToolId::kCut,
        {"-i", test_support::path_to_utf8(main_copy),
         "-o", test_support::path_to_utf8(cut_output),
         "-s", "2", "9", "-e", "4", "11"},
        sandbox.path());
    ASSERT_FALSE(cut_res.timed_out) << format_process_diagnostics("cut", cut_res);
    ASSERT_EQ(cut_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("cut", cut_res);

    EXPECT_EQ(test_support::list_all_entries(cut_out_dir).size(), 1u);
    EXPECT_EQ(test_support::list_all_entries(src_main_dir).size(), 1u);
    EXPECT_TRUE(main_snap.is_unchanged());

    auto cut_expected = make_lossless_main_expectation(
        main_info.duration_sec - kCutRemovedTotalSec, kCutDurationTolSec);
    cut_expected.expected_fps = std::nullopt;
    const av::MediaInfo cut_info = test_support::probe_valid_media(cut_output, cut_expected);
    ASSERT_TRUE(cut_info.ok);
    EXPECT_GT(cut_info.duration_sec, kCutMinDurationSec);
    EXPECT_LT(cut_info.duration_sec, kCutMaxDurationSec);
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        cut_output, kCutProbeStartSec, frame_scratch, "cut_start.jpg"));

    const fs::path clip_copy = src_clip_dir / clip_fixture.filename();
    fs::copy_file(clip_fixture, clip_copy, fs::copy_options::overwrite_existing);
    const test_support::FileSnapshot clip_snap(clip_copy);
    const av::MediaInfo clip_info = test_support::probe_valid_media(clip_copy);
    ASSERT_TRUE(clip_info.ok);
    ASSERT_GT(clip_info.width, 0);
    ASSERT_GT(clip_info.height, 0);

    const fs::path blur_output = blur_out_dir / "blurred.mp4";
    const auto blur_res = run_tool(
        ToolId::kCut,
        {"-i", test_support::path_to_utf8(clip_copy),
         "-o", test_support::path_to_utf8(blur_output),
         "-s", "0.5", "-e", "1.5", "--blur", "--sound-off"},
        sandbox.path());
    ASSERT_FALSE(blur_res.timed_out) << format_process_diagnostics("cut --blur", blur_res);
    ASSERT_EQ(blur_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("cut --blur", blur_res);

    EXPECT_EQ(test_support::list_all_entries(blur_out_dir).size(), 1u);
    EXPECT_TRUE(clip_snap.is_unchanged());

    test_support::MediaExpectation blur_expected;
    blur_expected.video_codec = "h264";
    blur_expected.width = clip_info.width;
    blur_expected.height = clip_info.height;
    blur_expected.has_video = true;
    blur_expected.has_audio = true;
    blur_expected.expected_duration_sec = clip_info.duration_sec;
    blur_expected.duration_tolerance_sec = kBlurDurationTolSec;
    const av::MediaInfo blur_info = test_support::probe_valid_media(blur_output, blur_expected);
    ASSERT_TRUE(blur_info.ok);
    EXPECT_FALSE(blur_info.audio_codec.empty());

    const fs::path src_frame_path = frame_scratch.child("blur_src_frame.jpg");
    const fs::path blurred_frame_path = frame_scratch.child("blur_out_frame.jpg");
    ASSERT_TRUE(av::grab_frame(clip_copy, src_frame_path, kBlurSampleSec));
    ASSERT_TRUE(av::grab_frame(blur_output, blurred_frame_path, kBlurSampleSec));

    const cv::Mat src_frame =
        decode_and_verify_jpeg(src_frame_path, clip_info.width, clip_info.height);
    const cv::Mat blurred_frame =
        decode_and_verify_jpeg(blurred_frame_path, clip_info.width, clip_info.height);
    ASSERT_FALSE(src_frame.empty());
    ASSERT_FALSE(blurred_frame.empty());
    ASSERT_EQ(src_frame.size(), blurred_frame.size());
    ASSERT_EQ(src_frame.type(), blurred_frame.type());
    EXPECT_GT(cv::norm(src_frame, blurred_frame, cv::NORM_L2), 0.0)
        << "Blurred frame inside [0.5, 1.5] must differ visually from the unblurred source frame";

    const test_support::FileSnapshot cut_snap(cut_output);
    const auto extract_res = run_tool(
        ToolId::kExtract,
        {"-i", test_support::path_to_utf8(cut_output),
         "-o", test_support::path_to_utf8(extract_out_dir),
         "--image", "3", "--image-detect", "--mosaic", "2"},
        sandbox.path());
    ASSERT_FALSE(extract_res.timed_out) << format_process_diagnostics("extract", extract_res);
    ASSERT_EQ(extract_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("extract", extract_res);
    EXPECT_TRUE(cut_snap.is_unchanged());

    const fs::path stem_dir = extract_out_dir / cut_output.stem();
    ASSERT_NO_FATAL_FAILURE(verify_extract_image_detect_mosaic(
        stem_dir, kMainVideoWidth, kMainVideoHeight, kExtractImageCount));
}

TEST_F(SystemTest, TranscodeMixedFpsToExtractPipeline) {
    const fs::path mixed_fixture = test_support::require_fixture(test_support::kFixtureMixedFpsDir);
    ASSERT_TRUE(fs::is_directory(mixed_fixture));

    const test_support::ScopedTempDir sandbox("st_transcode_extract");
    const fs::path mixed_in_dir = sandbox.create_subdir("mixed_in");
    const fs::path transcode_out_dir = sandbox.create_subdir("transcode_out");
    const fs::path frames_out_dir = sandbox.create_subdir("frames_out");

    const auto source_snaps = copy_directory_mp4s(mixed_fixture, mixed_in_dir);
    ASSERT_GE(source_snaps.size(), kTranscodeMinInputClips);

    const auto tx_res = run_tool(
        ToolId::kTranscode,
        {"-i", test_support::path_to_utf8(mixed_in_dir),
         "-o", test_support::path_to_utf8(transcode_out_dir),
         "-e", "libx264", "-p", "ultrafast", "-q", "28"},
        sandbox.path());
    ASSERT_FALSE(tx_res.timed_out) << format_process_diagnostics("transcode", tx_res);
    ASSERT_EQ(tx_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("transcode", tx_res);

    EXPECT_EQ(test_support::list_all_entries(transcode_out_dir).size(), 1u);
    const auto tx_files =
        test_support::list_regular_files(transcode_out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(tx_files.size(), 1u);

    const fs::path& transcoded_video = tx_files.front();
    test_support::MediaExpectation tx_expected;
    tx_expected.video_codec = "h264";
    tx_expected.audio_codec = "aac";
    tx_expected.has_video = true;
    tx_expected.has_audio = true;
    tx_expected.expected_fps = kExpectedFps;
    tx_expected.fps_tolerance = kFpsTolerance;
    tx_expected.expected_duration_sec = kTranscodeExpectedDurationSec;
    tx_expected.duration_tolerance_sec = kTranscodeDurationTolSec;
    const av::MediaInfo tx_info = test_support::probe_valid_media(transcoded_video, tx_expected);
    ASSERT_TRUE(tx_info.ok);
    ASSERT_GT(tx_info.width, 0);
    ASSERT_GT(tx_info.height, 0);

    EXPECT_EQ(test_support::list_all_entries(mixed_in_dir).size(), source_snaps.size());
    assert_all_unchanged(source_snaps);

    const test_support::FileSnapshot tx_snap(transcoded_video);
    const auto extract_res = run_tool(
        ToolId::kExtract,
        {"-i", test_support::path_to_utf8(transcoded_video),
         "-o", test_support::path_to_utf8(frames_out_dir),
         "--image", "4"},
        sandbox.path());
    ASSERT_FALSE(extract_res.timed_out) << format_process_diagnostics("extract", extract_res);
    ASSERT_EQ(extract_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("extract", extract_res);
    EXPECT_TRUE(tx_snap.is_unchanged());

    const fs::path image_dir = frames_out_dir / transcoded_video.stem() / "image";
    EXPECT_EQ(test_support::list_all_entries(image_dir).size(), kTranscodeExtractCount);
    const auto extracted_frames = test_support::list_regular_files(image_dir, ".jpg", "frame_");
    ASSERT_EQ(extracted_frames.size(), kTranscodeExtractCount);
    verify_all_jpegs_match_dimensions(extracted_frames, tx_info.width, tx_info.height);
}

TEST_F(SystemTest, RepeatRunIdempotenceAndFailureHygiene) {
    const fs::path main_fixture = test_support::require_fixture(test_support::kFixtureMainVideo);
    const fs::path clip_fixture = test_support::require_fixture(test_support::kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(main_fixture));
    ASSERT_TRUE(fs::is_regular_file(clip_fixture));

    const test_support::ScopedTempDir sandbox("st_idempotence_hygiene");
    const fs::path idemp_in_dir = sandbox.create_subdir("idemp_in");
    const fs::path idemp_out_dir = sandbox.create_subdir("idemp_out");

    const fs::path video_copy = idemp_in_dir / "capture.mp4";
    fs::copy_file(main_fixture, video_copy, fs::copy_options::overwrite_existing);
    const test_support::FileSnapshot video_snap(video_copy);

    const auto run1 = run_tool(
        ToolId::kExtract,
        {"-i", test_support::path_to_utf8(video_copy),
         "-o", test_support::path_to_utf8(idemp_out_dir),
         "--image-detect", "--mosaic", "2"},
        sandbox.path());
    ASSERT_FALSE(run1.timed_out) << format_process_diagnostics("extract (run 1)", run1);
    ASSERT_EQ(run1.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("extract (run 1)", run1);

    const fs::path detect_dir = idemp_out_dir / video_copy.stem() / "detect";
    const fs::path mosaics_dir = idemp_out_dir / video_copy.stem() / "mosaics";
    const auto striking_run1 = test_support::list_regular_files(detect_dir, ".jpg", "striking_");
    ASSERT_GE(striking_run1.size(), kExtractMosaicMinStriking);

    const fs::path mosaic_jpg = mosaics_dir / "mosaic.jpg";
    ASSERT_TRUE(fs::is_regular_file(mosaic_jpg));

    std::vector<PinnedArtifact> pinned;
    pinned.reserve(striking_run1.size() + 1u);
    for (const auto& frame : striking_run1) {
        pinned.push_back(pin_artifact(frame));
    }
    pinned.push_back(pin_artifact(mosaic_jpg));

    const std::string detect_hash_before = test_support::read_file_text(detect_dir / ".detect_hash");
    const std::string mosaic_hash_before = test_support::read_file_text(mosaics_dir / ".mosaic_hash");

    const auto run2 = run_tool(
        ToolId::kExtract,
        {"-i", test_support::path_to_utf8(video_copy),
         "-o", test_support::path_to_utf8(idemp_out_dir),
         "--image-detect", "--mosaic", "2"},
        sandbox.path());
    ASSERT_FALSE(run2.timed_out) << format_process_diagnostics("extract (run 2)", run2);
    ASSERT_EQ(run2.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("extract (run 2)", run2);

    const auto striking_run2 = test_support::list_regular_files(detect_dir, ".jpg", "striking_");
    EXPECT_EQ(striking_run2.size(), striking_run1.size());
    EXPECT_EQ(test_support::read_file_text(detect_dir / ".detect_hash"), detect_hash_before);
    EXPECT_EQ(test_support::read_file_text(mosaics_dir / ".mosaic_hash"), mosaic_hash_before);
    EXPECT_TRUE(video_snap.is_unchanged());

    for (const auto& artifact : pinned) {
        SCOPED_TRACE(test_support::path_to_utf8(artifact.path));
        ASSERT_TRUE(fs::is_regular_file(artifact.path));
        EXPECT_EQ(fs::last_write_time(artifact.path), artifact.pinned_mtime)
            << "Cached artifact was rewritten on identical repeat run";
        EXPECT_EQ(test_support::digest_file(artifact.path), artifact.digest)
            << "Cached artifact content changed on identical repeat run";
    }

    const fs::path single_clip_dir = sandbox.create_subdir("single_clip");
    const fs::path concat_fail_out = sandbox.create_subdir("concat_fail_out");
    const fs::path single_clip = single_clip_dir / clip_fixture.filename();
    fs::copy_file(clip_fixture, single_clip, fs::copy_options::overwrite_existing);
    const test_support::FileSnapshot single_clip_snap(single_clip);

    const auto concat_fail = run_tool(
        ToolId::kConcat,
        {"-i", test_support::path_to_utf8(single_clip_dir),
         "-o", test_support::path_to_utf8(concat_fail_out)},
        sandbox.path());
    ASSERT_FALSE(concat_fail.timed_out) << format_process_diagnostics("concat_videos", concat_fail);
    EXPECT_EQ(concat_fail.exit_code, kExpectedFailureExitCode)
        << format_process_diagnostics("concat_videos", concat_fail);
    EXPECT_FALSE((concat_fail.stdout_text + concat_fail.stderr_text).empty())
        << "Failed concat_videos invocation must emit diagnostic output";
    EXPECT_TRUE(test_support::list_all_entries(concat_fail_out).empty())
        << "Failed concat_videos invocation must not create partial outputs";
    EXPECT_TRUE(single_clip_snap.is_unchanged());

    const fs::path cut_fail_out = sandbox.create_subdir("cut_fail_out");
    const fs::path missing_input = sandbox.child("nonexistent_input.mp4");
    const fs::path blocked_output = cut_fail_out / "should_not_exist.mp4";

    const auto cut_fail = run_tool(
        ToolId::kCut,
        {"-i", test_support::path_to_utf8(missing_input),
         "-o", test_support::path_to_utf8(blocked_output),
         "-s", "1", "-e", "2"},
        sandbox.path());
    ASSERT_FALSE(cut_fail.timed_out) << format_process_diagnostics("cut", cut_fail);
    EXPECT_EQ(cut_fail.exit_code, kExpectedFailureExitCode)
        << format_process_diagnostics("cut", cut_fail);
    EXPECT_FALSE((cut_fail.stdout_text + cut_fail.stderr_text).empty())
        << "Failed cut invocation must emit diagnostic output";
    EXPECT_TRUE(test_support::list_all_entries(cut_fail_out).empty())
        << "Failed cut invocation must not create partial outputs";
}

TEST_F(SystemTest, UnicodeNonAsciiPathsEndToEndPipeline) {
    const fs::path main_fixture = test_support::require_fixture(test_support::kFixtureMainVideo);
    const fs::path short_fixture = test_support::require_fixture(test_support::kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(main_fixture));
    ASSERT_TRUE(fs::is_regular_file(short_fixture));

    const test_support::ScopedTempDir sandbox("st_unicode_e2e");
    const test_support::ScopedTempDir frame_scratch("st_unicode_frames");
    const fs::path in_dir = sandbox.create_subdir(u8"in_café_動画");
    const fs::path parts_dir = sandbox.create_subdir(u8"parts_café_動画");
    const fs::path joined_dir = sandbox.create_subdir(u8"joined_café_動画");
    const fs::path cut_dir = sandbox.create_subdir(u8"cut_café_動画");
    const fs::path tx_dir = sandbox.create_subdir(u8"tx_café_動画");
    const fs::path ext_dir = sandbox.create_subdir(u8"ext_café_動画");

    const fs::path main_unicode =
        sandbox.copy_in(main_fixture, u8"in_café_動画/café_動画_2026-01-01 00-00-00.mp4");
    const fs::path short_unicode =
        sandbox.copy_in(short_fixture, u8"in_café_動画/café_short_動画.mp4");
    const std::array<test_support::FileSnapshot, kUnicodeInputCount> input_snaps{{
        test_support::FileSnapshot(main_unicode),
        test_support::FileSnapshot(short_unicode),
    }};

    const av::MediaInfo main_info =
        test_support::probe_valid_media(main_unicode, make_lossless_main_expectation());
    ASSERT_TRUE(main_info.ok);
    ASSERT_GT(main_info.duration_sec, kUnicodeSplitSecondSec + kUnicodeSplitPartTolSec);

    const av::MediaInfo short_info = test_support::probe_valid_media(short_unicode);
    ASSERT_TRUE(short_info.ok);
    ASSERT_GT(short_info.width, 0);
    ASSERT_GT(short_info.height, 0);
    ASSERT_GT(short_info.duration_sec, 0.0);

    const auto split_res = run_tool(
        ToolId::kSplit,
        {"-i", test_support::path_to_utf8(main_unicode),
         "-o", test_support::path_to_utf8(parts_dir),
         "-p", "4", "10"},
        sandbox.path());
    ASSERT_FALSE(split_res.timed_out) << format_process_diagnostics("split", split_res);
    ASSERT_EQ(split_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("split", split_res);

    EXPECT_EQ(test_support::list_all_entries(parts_dir).size(), kExpectedSplitPartCount);
    const auto parts = test_support::list_regular_files(parts_dir, ".mp4");
    ASSERT_EQ(parts.size(), kExpectedSplitPartCount);

    const std::array<double, kExpectedSplitPartCount> expected_part_durations{{
        kUnicodeSplitFirstSec,
        kUnicodeSplitSecondSec - kUnicodeSplitFirstSec,
        main_info.duration_sec - kUnicodeSplitSecondSec,
    }};

    double sum_part_durations = 0.0;
    std::vector<test_support::FileSnapshot> part_snaps;
    part_snaps.reserve(parts.size());

    for (std::size_t idx = 0; idx < parts.size(); ++idx) {
        const auto& part = parts[idx];
        SCOPED_TRACE(test_support::path_to_utf8(part));

        const std::string expected_prefix = std::format(
            "{}_part{}_", test_support::path_to_utf8(main_unicode.stem()), idx + 1u);
        const std::string part_filename = test_support::path_to_utf8(part.filename());
        EXPECT_TRUE(part_filename.starts_with(expected_prefix))
            << "Unexpected Unicode split part filename: " << part_filename;

        const av::MediaInfo part_info = test_support::probe_valid_media(
            part,
            make_lossless_main_expectation(expected_part_durations[idx], kUnicodeSplitPartTolSec));
        ASSERT_TRUE(part_info.ok);

        sum_part_durations += part_info.duration_sec;
        part_snaps.emplace_back(part);
    }

    const auto concat_res = run_tool(
        ToolId::kConcat,
        {"-i", test_support::path_to_utf8(parts_dir),
         "-o", test_support::path_to_utf8(joined_dir)},
        sandbox.path());
    ASSERT_FALSE(concat_res.timed_out) << format_process_diagnostics("concat_videos", concat_res);
    ASSERT_EQ(concat_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("concat_videos", concat_res);

    EXPECT_EQ(test_support::list_all_entries(joined_dir).size(), 1u);
    const auto joined_files = test_support::list_regular_files(joined_dir, ".mp4", "concatenated_");
    ASSERT_EQ(joined_files.size(), 1u);

    const fs::path& joined_video = joined_files.front();
    const av::MediaInfo joined_info = test_support::probe_valid_media(
        joined_video,
        make_lossless_main_expectation(main_info.duration_sec, kUnicodeRoundTripTolSec));
    ASSERT_TRUE(joined_info.ok);
    EXPECT_NEAR(joined_info.duration_sec, sum_part_durations, kSplitSumMatchTolSec);
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        joined_video, kFrameProbeStartSec, frame_scratch, "unicode_joined.jpg"));
    assert_all_unchanged(part_snaps);

    const fs::path cut_output = cut_dir / test_support::path_from_u8(u8"結果_café.mp4");
    const auto cut_res = run_tool(
        ToolId::kCut,
        {"-i", test_support::path_to_utf8(main_unicode),
         "-o", test_support::path_to_utf8(cut_output),
         "-s", "2", "-e", "5"},
        sandbox.path());
    ASSERT_FALSE(cut_res.timed_out) << format_process_diagnostics("cut", cut_res);
    ASSERT_EQ(cut_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("cut", cut_res);

    EXPECT_EQ(test_support::list_all_entries(cut_dir).size(), 1u);
    auto cut_expected = make_lossless_main_expectation(
        main_info.duration_sec - kUnicodeCutRemovedSec, kCutDurationTolSec);
    cut_expected.expected_fps = std::nullopt;
    const av::MediaInfo cut_info = test_support::probe_valid_media(cut_output, cut_expected);
    ASSERT_TRUE(cut_info.ok);
    EXPECT_LT(cut_info.duration_sec, main_info.duration_sec - kUnicodeCutMinShortenSec);
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        cut_output, kCutProbeStartSec, frame_scratch, "unicode_cut.jpg"));

    const auto tx_res = run_tool(
        ToolId::kTranscode,
        {"-i", test_support::path_to_utf8(short_unicode),
         "-o", test_support::path_to_utf8(tx_dir),
         "-e", "libx264", "-p", "ultrafast", "-q", "28"},
        sandbox.path());
    ASSERT_FALSE(tx_res.timed_out) << format_process_diagnostics("transcode", tx_res);
    ASSERT_EQ(tx_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("transcode", tx_res);

    EXPECT_EQ(test_support::list_all_entries(tx_dir).size(), 1u);
    const auto tx_files = test_support::list_regular_files(tx_dir, ".mp4", "transcoded_");
    ASSERT_EQ(tx_files.size(), 1u);

    test_support::MediaExpectation tx_expected;
    tx_expected.video_codec = "h264";
    tx_expected.audio_codec = "aac";
    tx_expected.width = short_info.width;
    tx_expected.height = short_info.height;
    tx_expected.has_video = true;
    tx_expected.has_audio = true;
    tx_expected.expected_duration_sec = short_info.duration_sec;
    tx_expected.duration_tolerance_sec = kTranscodeDurationTolSec;
    const av::MediaInfo tx_info = test_support::probe_valid_media(tx_files.front(), tx_expected);
    ASSERT_TRUE(tx_info.ok);
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        tx_files.front(), kFrameProbeStartSec, frame_scratch, "unicode_tx.jpg"));

    const auto ext_res = run_tool(
        ToolId::kExtract,
        {"-i", test_support::path_to_utf8(main_unicode),
         "-o", test_support::path_to_utf8(ext_dir),
         "--image", "3", "--image-detect", "--mosaic", "2"},
        sandbox.path());
    ASSERT_FALSE(ext_res.timed_out) << format_process_diagnostics("extract", ext_res);
    ASSERT_EQ(ext_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("extract", ext_res);

    const fs::path stem_dir = ext_dir / main_unicode.stem();
    ASSERT_NO_FATAL_FAILURE(verify_extract_image_detect_mosaic(
        stem_dir, kMainVideoWidth, kMainVideoHeight, kExtractImageCount));
    assert_no_temp_dirs_recursive(ext_dir);

    EXPECT_EQ(test_support::list_all_entries(in_dir).size(), input_snaps.size());
    assert_all_unchanged(input_snaps);
}

TEST_F(SystemTest, AudioLessTranscodeEndToEndPipeline) {
    const fs::path mixed_fixture = test_support::require_fixture(test_support::kFixtureMixedFpsDir);
    ASSERT_TRUE(fs::is_directory(mixed_fixture));

    const auto mixed_sources = test_support::list_regular_files(mixed_fixture, ".mp4");
    ASSERT_GE(mixed_sources.size(), kTranscodeMinInputClips);

    const test_support::ScopedTempDir sandbox("st_audioless_transcode");
    const test_support::ScopedTempDir frame_scratch("st_audioless_frames");
    const fs::path silent_in_dir = sandbox.create_subdir("silent_in");
    const fs::path single_out_dir = sandbox.create_subdir("single_out");
    const fs::path multi_out_dir = sandbox.create_subdir("multi_out");
    const fs::path extract_out_dir = sandbox.create_subdir("extract_out");

    std::vector<fs::path> silent_clips;
    std::vector<av::MediaInfo> silent_infos;
    std::vector<test_support::FileSnapshot> silent_snaps;
    silent_clips.reserve(mixed_sources.size());
    silent_infos.reserve(mixed_sources.size());
    silent_snaps.reserve(mixed_sources.size());

    for (const auto& src : mixed_sources) {
        const fs::path dst = silent_in_dir / src.filename();
        const av::MediaInfo info = generate_silent_clip(src, dst);
        ASSERT_TRUE(info.ok);
        ASSERT_TRUE(info.has_video);
        ASSERT_FALSE(info.has_audio);
        ASSERT_TRUE(info.audio_codec.empty());
        ASSERT_GT(info.width, 0);
        ASSERT_GT(info.height, 0);
        silent_clips.push_back(dst);
        silent_infos.push_back(info);
        silent_snaps.emplace_back(dst);
    }

    const fs::path& first_silent = silent_clips.front();
    const av::MediaInfo& first_silent_info = silent_infos.front();

    const auto single_res = run_tool(
        ToolId::kTranscode,
        {"-i", test_support::path_to_utf8(first_silent),
         "-o", test_support::path_to_utf8(single_out_dir),
         "-e", "libx264", "-p", "ultrafast", "-q", "28"},
        sandbox.path());
    ASSERT_FALSE(single_res.timed_out) << format_process_diagnostics("transcode (single silent)", single_res);
    ASSERT_EQ(single_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("transcode (single silent)", single_res);

    EXPECT_EQ(test_support::list_all_entries(single_out_dir).size(), 1u);
    const auto single_files =
        test_support::list_regular_files(single_out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(single_files.size(), 1u);

    test_support::MediaExpectation single_expected;
    single_expected.video_codec = "h264";
    single_expected.width = first_silent_info.width;
    single_expected.height = first_silent_info.height;
    single_expected.has_video = true;
    single_expected.has_audio = false;
    single_expected.expected_duration_sec = first_silent_info.duration_sec;
    single_expected.duration_tolerance_sec = kTranscodeDurationTolSec;
    const av::MediaInfo single_info =
        test_support::probe_valid_media(single_files.front(), single_expected);
    ASSERT_TRUE(single_info.ok);
    EXPECT_TRUE(single_info.audio_codec.empty());
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        single_files.front(), kFrameProbeStartSec, frame_scratch, "silent_single.jpg"));

    const auto multi_res = run_tool(
        ToolId::kTranscode,
        {"-i", test_support::path_to_utf8(silent_in_dir),
         "-o", test_support::path_to_utf8(multi_out_dir),
         "-e", "libx264", "-p", "ultrafast", "-q", "28"},
        sandbox.path());
    ASSERT_FALSE(multi_res.timed_out) << format_process_diagnostics("transcode (multi silent)", multi_res);
    ASSERT_EQ(multi_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("transcode (multi silent)", multi_res);

    EXPECT_EQ(test_support::list_all_entries(multi_out_dir).size(), 1u);
    const auto multi_files =
        test_support::list_regular_files(multi_out_dir, ".mp4", "transcoded_");
    ASSERT_EQ(multi_files.size(), 1u);

    const fs::path& silent_multi_video = multi_files.front();
    test_support::MediaExpectation multi_expected;
    multi_expected.video_codec = "h264";
    multi_expected.width = first_silent_info.width;
    multi_expected.height = first_silent_info.height;
    multi_expected.has_video = true;
    multi_expected.has_audio = false;
    multi_expected.expected_fps = kExpectedFps;
    multi_expected.fps_tolerance = kFpsTolerance;
    multi_expected.expected_duration_sec = kTranscodeExpectedDurationSec;
    multi_expected.duration_tolerance_sec = kTranscodeDurationTolSec;
    const av::MediaInfo multi_info =
        test_support::probe_valid_media(silent_multi_video, multi_expected);
    ASSERT_TRUE(multi_info.ok);
    EXPECT_TRUE(multi_info.audio_codec.empty());

    const double late_ts =
        std::max(kFrameProbeStartSec, multi_info.duration_sec - kFrameProbeEndMarginSec);
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        silent_multi_video, kFrameProbeStartSec, frame_scratch, "silent_multi_start.jpg"));
    ASSERT_NO_FATAL_FAILURE(test_support::verify_decodable_frame_at(
        silent_multi_video, late_ts, frame_scratch, "silent_multi_end.jpg"));

    const test_support::FileSnapshot multi_snap(silent_multi_video);
    const auto extract_res = run_tool(
        ToolId::kExtract,
        {"-i", test_support::path_to_utf8(silent_multi_video),
         "-o", test_support::path_to_utf8(extract_out_dir),
         "--image", "3"},
        sandbox.path());
    ASSERT_FALSE(extract_res.timed_out) << format_process_diagnostics("extract (silent)", extract_res);
    ASSERT_EQ(extract_res.exit_code, kExpectedSuccessExitCode)
        << format_process_diagnostics("extract (silent)", extract_res);
    EXPECT_TRUE(multi_snap.is_unchanged());

    const fs::path image_dir = extract_out_dir / silent_multi_video.stem() / "image";
    EXPECT_EQ(test_support::list_all_entries(image_dir).size(), kSilentExtractImageCount);
    const auto extracted_frames = test_support::list_regular_files(image_dir, ".jpg", "frame_");
    ASSERT_EQ(extracted_frames.size(), kSilentExtractImageCount);
    verify_all_jpegs_match_dimensions(extracted_frames, multi_info.width, multi_info.height);

    EXPECT_EQ(test_support::list_all_entries(silent_in_dir).size(), silent_snaps.size());
    assert_all_unchanged(silent_snaps);
}

TEST_F(SystemTest, ConcatMismatchedStreamsFailsCleanlyWithDiagnostic) {
    const fs::path hevc_fixture = test_support::require_fixture(test_support::kFixtureMainVideo);
    const fs::path h264_fixture = test_support::require_fixture(test_support::kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(hevc_fixture));
    ASSERT_TRUE(fs::is_regular_file(h264_fixture));

    const test_support::ScopedTempDir sandbox("st_concat_mismatch");
    const fs::path in_dir = sandbox.create_subdir("in");
    const fs::path out_dir = sandbox.create_subdir("out");

    const fs::path first_clip = sandbox.copy_in(hevc_fixture, "in/2026-01-01 00-00-00.mp4");
    const fs::path second_clip = sandbox.copy_in(h264_fixture, "in/2026-01-02 00-00-00.mp4");
    const std::array<test_support::FileSnapshot, kMismatchInputCount> input_snaps{{
        test_support::FileSnapshot(first_clip),
        test_support::FileSnapshot(second_clip),
    }};

    const av::MediaInfo first_info = test_support::probe_valid_media(first_clip);
    const av::MediaInfo second_info = test_support::probe_valid_media(second_clip);
    ASSERT_TRUE(first_info.ok);
    ASSERT_TRUE(second_info.ok);
    ASSERT_NE(first_info.video_codec, second_info.video_codec)
        << "Precondition failed: mismatch fixtures must carry distinct video codecs";

    const auto res = run_tool(
        ToolId::kConcat,
        {"-i", test_support::path_to_utf8(in_dir),
         "-o", test_support::path_to_utf8(out_dir)},
        sandbox.path());
    ASSERT_FALSE(res.timed_out) << format_process_diagnostics("concat_videos", res);
    EXPECT_EQ(res.exit_code, kExpectedFailureExitCode)
        << format_process_diagnostics("concat_videos", res);
    EXPECT_TRUE(!res.stderr_text.empty() || !res.stdout_text.empty())
        << "Failed concat_videos invocation must emit diagnostic output";

    const std::string combined_output = res.stdout_text + "\n" + res.stderr_text;
    EXPECT_TRUE(contains_any_case_insensitive(combined_output, kStreamMismatchDiagnosticTerms))
        << "Expected stream incompatibility diagnostic in concat_videos output:\n"
        << format_process_diagnostics("concat_videos", res);
    EXPECT_TRUE(test_support::list_all_entries(out_dir).empty())
        << "Failed concat_videos invocation must not leave partial output files";
    EXPECT_EQ(test_support::list_all_entries(in_dir).size(), input_snaps.size());
    assert_all_unchanged(input_snaps);
}

}  // namespace
