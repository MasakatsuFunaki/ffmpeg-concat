#include "extract/extract.h"

#include "av/media_probe.h"
#include "support/cli_runner.hpp"
#include "support/media_assertions.hpp"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kMosaicWidth = 1920;
constexpr int kMosaicHeight = 1080;
constexpr std::size_t kBgrChannelCount = 3;
constexpr std::size_t kMinStrikingFrames = 1;
constexpr std::size_t kMaxStrikingFrames = 100;
constexpr std::size_t kMinLifecycleStrikingFrames = 3;
constexpr std::size_t kDetectHashFileCount = 1;
constexpr std::size_t kSha256HexLength = 64;
constexpr std::size_t kMinJpegBytes = 4;
constexpr unsigned kJpegSoiFirst = 0xFFu;
constexpr unsigned kJpegSoiSecond = 0xD8u;
constexpr double kMinImageStdDev = 1.0;
constexpr auto kPinnedMtimeAge = std::chrono::seconds{120};
constexpr std::size_t kMultiVideoInputCount = 2;
constexpr std::size_t kMultiVideoImageCount = 4;
constexpr std::size_t kCombinedImageCount = 3;
constexpr std::size_t kSameDirImageCount = 3;
constexpr std::size_t kSameDirExpectedEntries = 2;
constexpr std::size_t kExpectedSingleSubdir = 1;
constexpr std::size_t kExpectedMosaicEntries = 2;
constexpr std::size_t kExpectedCombinedSubdirs = 3;
constexpr std::size_t kMaxInvalidCliExtraArgs = 4;

struct PinnedFile {
    fs::path path;
    fs::file_time_type pinned_mtime;
    test_support::FileSnapshot snapshot;
};

struct InvalidCliCase {
    std::string_view label;
    bool use_missing_input = false;
    std::array<std::string_view, kMaxInvalidCliExtraArgs> extra_args{};
};

[[nodiscard]] constexpr bool is_hex_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] double max_channel_stddev(const cv::Mat& image) {
    cv::Scalar mean{};
    cv::Scalar stddev{};
    cv::meanStdDev(image, mean, stddev);
    const std::array<double, kBgrChannelCount> channels{stddev[0], stddev[1], stddev[2]};
    return std::ranges::max(channels);
}

[[nodiscard]] std::string read_trimmed_text(const fs::path& file) {
    std::string text = test_support::read_file_text(file);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] std::string read_non_empty_cache_file(const fs::path& hash_file) {
    EXPECT_TRUE(fs::is_regular_file(hash_file)) << "Missing cache file: " << hash_file;
    if (!fs::is_regular_file(hash_file)) {
        return {};
    }
    const std::string text = read_trimmed_text(hash_file);
    EXPECT_FALSE(text.empty()) << "Empty cache file: " << hash_file;
    return text;
}

[[nodiscard]] std::string read_verified_dir_fingerprint_file(
    const fs::path& hash_file, std::size_t expected_entry_count) {
    const std::string text = read_non_empty_cache_file(hash_file);
    if (text.empty()) {
        return {};
    }
    std::size_t line_count = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const std::string_view raw_line =
            (nl == std::string::npos) ? std::string_view(text).substr(pos)
                                      : std::string_view(text).substr(pos, nl - pos);
        const std::string_view line =
            (!raw_line.empty() && raw_line.back() == '\r') ? raw_line.substr(0, raw_line.size() - 1)
                                                           : raw_line;
        if (!line.empty()) {
            const auto colon = line.rfind(':');
            EXPECT_NE(colon, std::string_view::npos)
                << "Expected name:sha256 entry in " << hash_file << ": " << line;
            if (colon != std::string_view::npos) {
                const auto name = line.substr(0, colon);
                const auto hex = line.substr(colon + 1);
                EXPECT_FALSE(name.empty()) << "Empty filename in fingerprint line: " << line;
                EXPECT_EQ(hex.size(), kSha256HexLength)
                    << "Unexpected SHA-256 hex length in " << hash_file << ": " << line;
                EXPECT_TRUE(std::ranges::all_of(hex, is_hex_char))
                    << "Non-hex characters in " << hash_file << ": " << line;
            }
            ++line_count;
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    EXPECT_EQ(line_count, expected_entry_count)
        << "Fingerprint line count mismatch in: " << hash_file;
    return text;
}

// Verifies raw JPEG SOI markers and independent OpenCV BGR decoding.
[[nodiscard]] cv::Mat decode_verified_jpeg(
    const fs::path& file, std::optional<cv::Size> expected_size = std::nullopt) {
    EXPECT_TRUE(fs::is_regular_file(file)) << "Missing JPEG: " << file;
    if (!fs::is_regular_file(file)) {
        return {};
    }

    const auto bytes = test_support::read_file_bytes(file);
    EXPECT_GE(bytes.size(), kMinJpegBytes) << "Truncated JPEG: " << file;
    if (bytes.size() < kMinJpegBytes) {
        return {};
    }
    EXPECT_EQ(std::to_integer<unsigned>(bytes[0]), kJpegSoiFirst) << "Bad JPEG SOI[0]: " << file;
    EXPECT_EQ(std::to_integer<unsigned>(bytes[1]), kJpegSoiSecond) << "Bad JPEG SOI[1]: " << file;

    cv::Mat image = cv::imread(file.string(), cv::IMREAD_COLOR);
    EXPECT_FALSE(image.empty()) << "OpenCV failed to decode JPEG: " << file;
    if (image.empty()) {
        return {};
    }

    EXPECT_EQ(image.channels(), static_cast<int>(kBgrChannelCount))
        << "Unexpected channel count: " << file;
    EXPECT_GT(image.cols, 0) << "Invalid width: " << file;
    EXPECT_GT(image.rows, 0) << "Invalid height: " << file;
    if (expected_size) {
        EXPECT_EQ(image.cols, expected_size->width) << "Width mismatch: " << file;
        EXPECT_EQ(image.rows, expected_size->height) << "Height mismatch: " << file;
    }
    return image;
}

[[nodiscard]] cv::Mat decode_verified_mosaic(const fs::path& mosaic_file) {
    cv::Mat mosaic = decode_verified_jpeg(mosaic_file, cv::Size{kMosaicWidth, kMosaicHeight});
    if (mosaic.empty()) {
        return {};
    }
    EXPECT_GT(max_channel_stddev(mosaic), kMinImageStdDev)
        << "Mosaic appears visually uniform: " << mosaic_file;
    return mosaic;
}

[[nodiscard]] std::vector<cv::Mat> verify_equally_spaced_frames(
    const fs::path& image_dir, std::size_t expected_count, cv::Size expected_size) {
    EXPECT_TRUE(fs::is_directory(image_dir)) << "Expected image dir: " << image_dir;
    const auto entries = test_support::list_all_entries(image_dir);
    EXPECT_EQ(entries.size(), expected_count) << "Unexpected entry count in: " << image_dir;

    std::vector<cv::Mat> decoded;
    decoded.reserve(expected_count);
    for (const auto index : std::views::iota(std::size_t{1}, expected_count + 1)) {
        const fs::path frame_path = image_dir / std::format("frame_{:04d}.jpg", index);
        decoded.push_back(decode_verified_jpeg(frame_path, expected_size));
    }
    return decoded;
}

[[nodiscard]] std::vector<fs::path> collect_verified_striking_frames(
    const fs::path& detect_dir, cv::Size expected_size) {
    EXPECT_TRUE(fs::is_directory(detect_dir)) << "Expected detect dir: " << detect_dir;
    const auto striking_files = test_support::list_regular_files(detect_dir, ".jpg", "striking_");
    EXPECT_GE(striking_files.size(), kMinStrikingFrames) << "Expected at least 1 striking frame";
    EXPECT_LE(striking_files.size(), kMaxStrikingFrames)
        << "Striking frame count exceeded documented limit";

    const auto all_entries = test_support::list_all_entries(detect_dir);
    EXPECT_EQ(all_entries.size(), striking_files.size() + kDetectHashFileCount)
        << "Unexpected stray files or temp dirs in: " << detect_dir;

    for (const auto& frame_path : striking_files) {
        const cv::Mat frame = decode_verified_jpeg(frame_path, expected_size);
        EXPECT_FALSE(frame.empty()) << "Failed decoding striking frame: " << frame_path;
    }
    return striking_files;
}

void verify_pinned_files_untouched(std::span<const PinnedFile> pinned, std::string_view phase) {
    SCOPED_TRACE(std::string(phase));
    for (const auto& item : pinned) {
        EXPECT_TRUE(fs::is_regular_file(item.path)) << "Pinned file missing: " << item.path;
        if (!fs::is_regular_file(item.path)) {
            continue;
        }
        EXPECT_EQ(fs::last_write_time(item.path), item.pinned_mtime)
            << "File rewritten despite expected cache hit: " << item.path;
        EXPECT_TRUE(item.snapshot.is_unchanged())
            << "File content changed despite expected cache hit: " << item.path;
    }
}

[[nodiscard]] bool has_any_files_recursively(const fs::path& dir) {
    if (!fs::exists(dir)) {
        return false;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(ExtractAppTest, MultiVideoDirectoryEquallySpacedExtraction) {
    const fs::path clip1_src = test_support::require_fixture(test_support::kFixtureUniformClip1);
    const fs::path clip2_src = test_support::require_fixture(test_support::kFixtureUniformClip2);
    ASSERT_TRUE(fs::is_regular_file(clip1_src));
    ASSERT_TRUE(fs::is_regular_file(clip2_src));

    const test_support::ScopedTempDir sandbox("extract_multi_video");
    const fs::path in_dir = sandbox.create_subdir("input");
    const fs::path out_dir = sandbox.child("output");

    const fs::path clip1 = sandbox.copy_in(clip1_src, "input/scene alpha.mp4");
    const fs::path clip2 = sandbox.copy_in(clip2_src, "input/scene_beta.mp4");
    const fs::path ignored_txt = sandbox.write_text("input/notes.txt", "non-video metadata\n");

    const av::MediaInfo info1 = av::probe(clip1);
    const av::MediaInfo info2 = av::probe(clip2);
    ASSERT_TRUE(info1.ok);
    ASSERT_TRUE(info2.ok);
    ASSERT_GT(info1.width, 0);
    ASSERT_GT(info1.height, 0);
    ASSERT_GT(info2.width, 0);
    ASSERT_GT(info2.height, 0);

    const test_support::FileSnapshot snap1(clip1);
    const test_support::FileSnapshot snap2(clip2);
    const test_support::FileSnapshot txt_snap(ignored_txt);

    const std::string in_arg = in_dir.string();
    const std::string out_arg = out_dir.string();
    const int rc = test_support::run_tool_app<ExtractApp>(
        {"extract", "-i", in_arg, "-o", out_arg, "--image", "4"});
    ASSERT_EQ(rc, 0);

    const auto out_entries = test_support::list_all_entries(out_dir);
    EXPECT_EQ(out_entries.size(), kMultiVideoInputCount);
    EXPECT_FALSE(fs::exists(out_dir / "notes"));

    const fs::path stem1_dir = out_dir / clip1.stem();
    const fs::path stem2_dir = out_dir / clip2.stem();
    EXPECT_EQ(test_support::list_all_entries(stem1_dir).size(), kExpectedSingleSubdir);
    EXPECT_EQ(test_support::list_all_entries(stem2_dir).size(), kExpectedSingleSubdir);

    const auto frames1 = verify_equally_spaced_frames(
        stem1_dir / "image", kMultiVideoImageCount, cv::Size{info1.width, info1.height});
    const auto frames2 = verify_equally_spaced_frames(
        stem2_dir / "image", kMultiVideoImageCount, cv::Size{info2.width, info2.height});
    EXPECT_EQ(frames1.size(), kMultiVideoImageCount);
    EXPECT_EQ(frames2.size(), kMultiVideoImageCount);

    EXPECT_TRUE(snap1.is_unchanged());
    EXPECT_TRUE(snap2.is_unchanged());
    EXPECT_TRUE(txt_snap.is_unchanged());
}

TEST(ExtractAppTest, SingleFileCombinedImageDetectAndMosaic) {
    const fs::path video = test_support::require_fixture(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(video));

    const av::MediaInfo src_info = av::probe(video);
    ASSERT_TRUE(src_info.ok);
    ASSERT_EQ(src_info.width, kMosaicWidth);
    ASSERT_EQ(src_info.height, kMosaicHeight);
    const cv::Size src_size{src_info.width, src_info.height};

    const test_support::FileSnapshot video_snap(video);
    const test_support::ScopedTempDir sandbox("extract_combined");
    const fs::path out_dir = sandbox.child("output");

    const std::string in_arg = video.string();
    const std::string out_arg = out_dir.string();
    const int rc = test_support::run_tool_app<ExtractApp>(
        {"extract", "-i", in_arg, "-o", out_arg, "--image", "3", "--image-detect", "--mosaic",
         "3"});
    ASSERT_EQ(rc, 0);

    const fs::path stem_dir = out_dir / video.stem();
    EXPECT_EQ(test_support::list_all_entries(stem_dir).size(), kExpectedCombinedSubdirs);

    const auto image_frames =
        verify_equally_spaced_frames(stem_dir / "image", kCombinedImageCount, src_size);
    ASSERT_EQ(image_frames.size(), kCombinedImageCount);
    ASSERT_FALSE(image_frames.front().empty());
    ASSERT_FALSE(image_frames.back().empty());
    EXPECT_GT(cv::norm(image_frames.front(), image_frames.back(), cv::NORM_L2), 0.0);

    const fs::path detect_dir = stem_dir / "detect";
    const auto striking_files = collect_verified_striking_frames(detect_dir, src_size);
    EXPECT_FALSE(striking_files.empty());
    const std::string detect_hash =
        read_verified_dir_fingerprint_file(detect_dir / ".detect_hash", striking_files.size());
    EXPECT_FALSE(detect_hash.empty());

    const fs::path mosaics_dir = stem_dir / "mosaics";
    EXPECT_EQ(test_support::list_all_entries(mosaics_dir).size(), kExpectedMosaicEntries);
    const cv::Mat mosaic = decode_verified_mosaic(mosaics_dir / "mosaic.jpg");
    EXPECT_FALSE(mosaic.empty());
    const std::string mosaic_hash = read_non_empty_cache_file(mosaics_dir / ".mosaic_hash");
    EXPECT_FALSE(mosaic_hash.empty());

    EXPECT_TRUE(video_snap.is_unchanged());
}

TEST(ExtractAppTest, DetectAndMosaicCacheHitAndInvalidationLifecycle) {
    const fs::path video = test_support::require_fixture(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(video));

    const av::MediaInfo src_info = av::probe(video);
    ASSERT_TRUE(src_info.ok);
    ASSERT_EQ(src_info.width, kMosaicWidth);
    ASSERT_EQ(src_info.height, kMosaicHeight);
    const cv::Size src_size{src_info.width, src_info.height};

    const test_support::FileSnapshot video_snap(video);
    const test_support::ScopedTempDir sandbox("extract_cache_lifecycle");
    const fs::path out_dir = sandbox.child("output");
    const std::string in_arg = video.string();
    const std::string out_arg = out_dir.string();

    const fs::path stem_dir = out_dir / video.stem();
    const fs::path detect_dir = stem_dir / "detect";
    const fs::path mosaics_dir = stem_dir / "mosaics";
    const fs::path detect_hash_file = detect_dir / ".detect_hash";
    const fs::path mosaic_file = mosaics_dir / "mosaic.jpg";
    const fs::path mosaic_hash_file = mosaics_dir / ".mosaic_hash";

    // Cold detect + mosaic(N=2, P=1).
    ASSERT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", in_arg, "-o", out_arg, "--image-detect", "--mosaic", "2"}),
              0);

    const auto initial_striking = collect_verified_striking_frames(detect_dir, src_size);
    ASSERT_GE(initial_striking.size(), kMinLifecycleStrikingFrames);
    const std::string detect_hash_1 =
        read_verified_dir_fingerprint_file(detect_hash_file, initial_striking.size());
    const std::string mosaic_hash_n2_p1 = read_non_empty_cache_file(mosaic_hash_file);
    const cv::Mat mosaic_n2_p1 = decode_verified_mosaic(mosaic_file);
    ASSERT_FALSE(mosaic_n2_p1.empty());

    std::vector<PinnedFile> pinned_striking;
    pinned_striking.reserve(initial_striking.size());
    for (const auto& path : initial_striking) {
        const auto mtime = test_support::pin_mtime_in_past(path, kPinnedMtimeAge);
        pinned_striking.push_back(PinnedFile{path, mtime, test_support::FileSnapshot(path)});
    }
    const auto pinned_mosaic_mtime = test_support::pin_mtime_in_past(mosaic_file, kPinnedMtimeAge);
    const auto mosaic_digest_1 = test_support::digest_file(mosaic_file);

    // Repeat identical invocation hits both detect and mosaic caches without rewrites.
    ASSERT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", in_arg, "-o", out_arg, "--image-detect", "--mosaic", "2"}),
              0);
    EXPECT_EQ(test_support::list_regular_files(detect_dir, ".jpg", "striking_"), initial_striking);
    verify_pinned_files_untouched(pinned_striking, "full cache hit");
    ASSERT_TRUE(fs::is_regular_file(mosaic_file));
    EXPECT_EQ(fs::last_write_time(mosaic_file), pinned_mosaic_mtime);
    EXPECT_EQ(test_support::digest_file(mosaic_file), mosaic_digest_1);
    EXPECT_EQ(read_verified_dir_fingerprint_file(detect_hash_file, initial_striking.size()),
              detect_hash_1);
    EXPECT_EQ(read_non_empty_cache_file(mosaic_hash_file), mosaic_hash_n2_p1);

    // Deleting mosaic.jpg regenerates mosaic only while detect frames remain cached.
    ASSERT_TRUE(fs::remove(mosaic_file));
    ASSERT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", in_arg, "-o", out_arg, "--image-detect", "--mosaic", "2"}),
              0);
    ASSERT_FALSE(decode_verified_mosaic(mosaic_file).empty());
    verify_pinned_files_untouched(pinned_striking, "mosaic-only regeneration after deletion");

    // Changing mosaic count (N=2 -> N=3) invalidates mosaic cache only.
    const auto pinned_before_n3 = test_support::pin_mtime_in_past(mosaic_file, kPinnedMtimeAge);
    ASSERT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", in_arg, "-o", out_arg, "--image-detect", "--mosaic", "3"}),
              0);
    const std::string mosaic_hash_n3_p1 = read_non_empty_cache_file(mosaic_hash_file);
    EXPECT_NE(mosaic_hash_n3_p1, mosaic_hash_n2_p1);
    ASSERT_TRUE(fs::is_regular_file(mosaic_file));
    EXPECT_NE(fs::last_write_time(mosaic_file), pinned_before_n3);
    const cv::Mat mosaic_n3_p1 = decode_verified_mosaic(mosaic_file);
    ASSERT_FALSE(mosaic_n3_p1.empty());
    EXPECT_GT(cv::norm(mosaic_n2_p1, mosaic_n3_p1, cv::NORM_L2), 0.0);
    verify_pinned_files_untouched(pinned_striking, "mosaic invalidation on count change");

    // Changing start position (P=1 -> P=2) invalidates mosaic cache only.
    const auto pinned_before_p2 = test_support::pin_mtime_in_past(mosaic_file, kPinnedMtimeAge);
    ASSERT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", in_arg, "-o", out_arg, "--image-detect", "--mosaic", "2", "2"}),
              0);
    const std::string mosaic_hash_n2_p2 = read_non_empty_cache_file(mosaic_hash_file);
    EXPECT_NE(mosaic_hash_n2_p2, mosaic_hash_n3_p1);
    EXPECT_NE(mosaic_hash_n2_p2, mosaic_hash_n2_p1);
    ASSERT_TRUE(fs::is_regular_file(mosaic_file));
    EXPECT_NE(fs::last_write_time(mosaic_file), pinned_before_p2);
    const cv::Mat mosaic_n2_p2 = decode_verified_mosaic(mosaic_file);
    ASSERT_FALSE(mosaic_n2_p2.empty());
    EXPECT_GT(cv::norm(mosaic_n2_p1, mosaic_n2_p2, cv::NORM_L2), 0.0);
    verify_pinned_files_untouched(pinned_striking, "mosaic invalidation on start position change");

    // Deleting one striking frame invalidates detect cache and restores the full set.
    const fs::path deleted_frame = pinned_striking.front().path;
    const auto deleted_pinned_mtime = pinned_striking.front().pinned_mtime;
    ASSERT_TRUE(fs::remove(deleted_frame));
    ASSERT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", in_arg, "-o", out_arg, "--image-detect"}),
              0);

    const auto restored_striking = test_support::list_regular_files(detect_dir, ".jpg", "striking_");
    EXPECT_EQ(restored_striking.size(), initial_striking.size());
    ASSERT_TRUE(fs::is_regular_file(deleted_frame));
    EXPECT_NE(fs::last_write_time(deleted_frame), deleted_pinned_mtime);
    EXPECT_FALSE(decode_verified_jpeg(deleted_frame, src_size).empty());
    EXPECT_EQ(
        test_support::list_all_entries(detect_dir).size(),
        restored_striking.size() + kDetectHashFileCount);
    const std::string detect_hash_2 =
        read_verified_dir_fingerprint_file(detect_hash_file, restored_striking.size());
    EXPECT_FALSE(detect_hash_2.empty());

    EXPECT_TRUE(video_snap.is_unchanged());
}

TEST(ExtractAppTest, SameInputOutputDirPreservesSourceVideo) {
    const fs::path clip_src = test_support::require_fixture(test_support::kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(clip_src));

    const test_support::ScopedTempDir sandbox("extract_same_dir");
    const fs::path video_copy = sandbox.copy_in(clip_src, "same_dir_clip.mp4");

    const av::MediaInfo src_info = av::probe(video_copy);
    ASSERT_TRUE(src_info.ok);
    ASSERT_GT(src_info.width, 0);
    ASSERT_GT(src_info.height, 0);

    const test_support::FileSnapshot snap(video_copy);
    const std::string dir_arg = sandbox.path().string();
    const int rc = test_support::run_tool_app<ExtractApp>(
        {"extract", "-i", dir_arg, "-o", dir_arg, "--image", "3"});
    ASSERT_EQ(rc, 0);

    EXPECT_TRUE(snap.is_unchanged());
    EXPECT_EQ(test_support::list_all_entries(sandbox.path()).size(), kSameDirExpectedEntries);

    const fs::path stem_dir = sandbox.path() / video_copy.stem();
    EXPECT_EQ(test_support::list_all_entries(stem_dir).size(), kExpectedSingleSubdir);
    const auto frames = verify_equally_spaced_frames(
        stem_dir / "image", kSameDirImageCount, cv::Size{src_info.width, src_info.height});
    EXPECT_EQ(frames.size(), kSameDirImageCount);
}

TEST(ExtractAppTest, ExtractTextModeProducesOnlyValidCropsAndPreservesInput) {
    const fs::path video = test_support::require_fixture(test_support::kFixtureMainVideo);
    ASSERT_TRUE(fs::is_regular_file(video));

    const av::MediaInfo src_info = av::probe(video);
    ASSERT_TRUE(src_info.ok);
    ASSERT_GT(src_info.width, 0);
    ASSERT_GT(src_info.height, 0);

    const test_support::FileSnapshot video_snap(video);
    const test_support::ScopedTempDir sandbox("extract_text");
    const fs::path out_dir = sandbox.child("output");

    const std::string in_arg = video.string();
    const std::string out_arg = out_dir.string();
    const int rc = test_support::run_tool_app<ExtractApp>(
        {"extract", "-i", in_arg, "-o", out_arg, "--extract-text"});
    ASSERT_EQ(rc, 0);

    const fs::path stem_dir = out_dir / video.stem();
    EXPECT_EQ(test_support::list_all_entries(stem_dir).size(), kExpectedSingleSubdir);

    const fs::path text_dir = stem_dir / "text";
    ASSERT_TRUE(fs::is_directory(text_dir));

    const auto all_entries = test_support::list_all_entries(text_dir);
    const auto jpg_crops = test_support::list_regular_files(text_dir, ".jpg", "text_");
    EXPECT_EQ(all_entries.size(), jpg_crops.size())
        << "Stray non-crop files found in text output dir: " << text_dir;
    ASSERT_GE(jpg_crops.size(), 1u)
        << "Expected at least one detected text crop from real capture in: " << text_dir;

    for (const auto& crop_path : jpg_crops) {
        const cv::Mat crop = decode_verified_jpeg(crop_path);
        ASSERT_FALSE(crop.empty()) << "Failed to decode text crop: " << crop_path;
        EXPECT_LE(crop.cols, src_info.width);
        EXPECT_LE(crop.rows, src_info.height);
        EXPECT_GT(max_channel_stddev(crop), 0.0)
            << "Text crop has zero pixel contrast: " << crop_path;
    }

    EXPECT_TRUE(video_snap.is_unchanged());
}

TEST(ExtractAppTest, RejectsInvalidCliOptionsWithoutLeakingOutputs) {
    const fs::path clip = test_support::require_fixture(test_support::kFixtureUniformClip1);
    ASSERT_TRUE(fs::is_regular_file(clip));

    const test_support::FileSnapshot clip_snap(clip);
    const test_support::ScopedTempDir sandbox("extract_invalid_cli");
    const fs::path missing_path = sandbox.child("does_not_exist_input");

    constexpr auto kInvalidCases = std::to_array<InvalidCliCase>({
        {"missing_mode_flags", false, {}},
        {"nonexistent_input", true, {{"--image", "2"}}},
        {"zero_image_count", false, {{"--image", "0"}}},
        {"mosaic_without_detect", false, {{"--image", "2", "--mosaic", "2"}}},
        {"mosaic_zero_count", false, {{"--image-detect", "--mosaic", "0"}}},
        {"mosaic_zero_start", false, {{"--image-detect", "--mosaic", "2", "0"}}},
        {"mosaic_negative_start", false, {{"--image-detect", "--mosaic", "2", "-1"}}},
    });

    for (const auto& tc : kInvalidCases) {
        SCOPED_TRACE(std::string(tc.label));
        const fs::path case_out = sandbox.child(tc.label);
        std::vector<std::string> args{
            "extract",
            "-i",
            (tc.use_missing_input ? missing_path : clip).string(),
            "-o",
            case_out.string(),
        };
        for (const std::string_view arg : tc.extra_args) {
            if (!arg.empty()) {
                args.emplace_back(arg);
            }
        }

        const int rc = test_support::run_tool_app<ExtractApp>(args);
        EXPECT_EQ(rc, 1);
        EXPECT_FALSE(has_any_files_recursively(case_out))
            << "Invalid CLI case leaked output files into: " << case_out;
    }

    const fs::path overflow_out = sandbox.child("mosaic_overflow");
    const std::string clip_arg = clip.string();
    const std::string overflow_arg = overflow_out.string();
    const fs::path mosaics_dir = overflow_out / clip.stem() / "mosaics";

    EXPECT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", clip_arg, "-o", overflow_arg, "--image-detect", "--mosaic",
                   "500"}),
              1);
    EXPECT_FALSE(fs::exists(mosaics_dir / "mosaic.jpg"));
    EXPECT_FALSE(fs::exists(mosaics_dir / ".mosaic_hash"));

    EXPECT_EQ(test_support::run_tool_app<ExtractApp>(
                  {"extract", "-i", clip_arg, "-o", overflow_arg, "--image-detect", "--mosaic",
                   "2", "9999"}),
              1);
    EXPECT_FALSE(fs::exists(mosaics_dir / "mosaic.jpg"));
    EXPECT_FALSE(fs::exists(mosaics_dir / ".mosaic_hash"));

    EXPECT_TRUE(clip_snap.is_unchanged());
}
