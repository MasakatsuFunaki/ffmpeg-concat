#include "extract/crop_deduplicator.h"
#include "extract/crop_filter.h"
#include "extract/fingerprint_cache.h"
#include "extract/frame_scorer.h"
#include "extract/mosaic_builder.h"
#include "extract/text_region_detector.h"
#include "support/test_sandbox.hpp"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kSha256HexLen = 64;
constexpr int kMosaicWidth = 1920;
constexpr int kMosaicHeight = 1080;
constexpr int kJpegQuality = 95;

[[nodiscard]] bool is_lowercase_sha256_hex(std::string_view hex) noexcept {
    if (hex.size() != kSha256HexLen) {
        return false;
    }
    return std::ranges::all_of(hex, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Builds an even-sized grayscale 2-level checkerboard with exact mean 128.0
// and exact population standard deviation equal to `delta`.
[[nodiscard]] cv::Mat make_exact_stddev_patch(int side, int delta) {
    cv::Mat patch(side, side, CV_8UC1);
    const auto lo = static_cast<std::uint8_t>(128 - delta);
    const auto hi = static_cast<std::uint8_t>(128 + delta);
    for (int r = 0; r < side; ++r) {
        auto* row = patch.ptr<std::uint8_t>(r);
        for (int c = 0; c < side; ++c) {
            row[c] = ((r + c) % 2 == 0) ? lo : hi;
        }
    }
    return patch;
}

[[nodiscard]] cv::Mat make_stripe_pattern(int height, int width, bool horizontal, int channels) {
    cv::Mat gray(height, width, CV_8UC1);
    for (int r = 0; r < height; ++r) {
        auto* row = gray.ptr<std::uint8_t>(r);
        for (int c = 0; c < width; ++c) {
            const int band = horizontal ? (r / 8) : (c / 8);
            row[c] = (band % 2 == 0) ? 20 : 235;
        }
    }
    if (channels == 1) {
        return gray;
    }
    cv::Mat out;
    if (channels == 3) {
        cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);
        return out;
    }
    cv::cvtColor(gray, out, cv::COLOR_GRAY2BGRA);
    return out;
}

[[nodiscard]] cv::Mat make_diagonal_checker(int height, int width) {
    cv::Mat img(height, width, CV_8UC3);
    for (int r = 0; r < height; ++r) {
        auto* row = img.ptr<cv::Vec3b>(r);
        for (int c = 0; c < width; ++c) {
            const bool even = (((r / 12) + (c / 12)) % 2) == 0;
            row[c] = even ? cv::Vec3b(20, 40, 220) : cv::Vec3b(230, 210, 30);
        }
    }
    return img;
}

[[nodiscard]] cv::Mat make_vibrant_checkerboard(int side, int tile) {
    cv::Mat img(side, side, CV_8UC3);
    const std::array<cv::Vec3b, 4> palette{{
        {0, 0, 255},
        {255, 255, 0},
        {0, 255, 255},
        {255, 0, 0},
    }};
    for (int r = 0; r < side; ++r) {
        auto* row = img.ptr<cv::Vec3b>(r);
        for (int c = 0; c < side; ++c) {
            const std::size_t idx =
                static_cast<std::size_t>(((r / tile) * 2 + (c / tile)) % 4);
            row[c] = palette[idx];
        }
    }
    return img;
}

[[nodiscard]] bool write_jpeg(const fs::path& path, const cv::Mat& img) {
    return cv::imwrite(path.string(), img, {cv::IMWRITE_JPEG_QUALITY, kJpegQuality});
}

template <typename Pred>
[[nodiscard]] int count_bgr_pixels(const cv::Mat& bgr, Pred pred) {
    int count = 0;
    for (int r = 0; r < bgr.rows; ++r) {
        const auto* row = bgr.ptr<cv::Vec3b>(r);
        for (int c = 0; c < bgr.cols; ++c) {
            if (pred(row[c])) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

// FingerprintCache

TEST(FingerprintCacheTest, Sha256FileMatchesNistVectorsAndSignBitBoundaries) {
    test_support::ScopedTempDir sandbox("ut_sha256");

    // Official NIST FIPS 180-4 SHA-256 test vectors.
    struct NistVector {
        std::string_view filename;
        std::string_view content;
        std::string_view expected_hex;
    };
    constexpr std::array<NistVector, 3> kShortVectors{{
        {"empty.bin",
         "",
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc.bin",
         "abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"block_seam_56.bin",
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    }};

    for (const auto& vec : kShortVectors) {
        SCOPED_TRACE(vec.filename);
        const fs::path p = sandbox.write_text(vec.filename, vec.content);
        EXPECT_EQ(FingerprintCache::sha256_file(p), vec.expected_hex);
    }

    // NIST FIPS 180-4 1,000,000 'a's vector exercises >122 8KB streaming chunks.
    constexpr std::size_t kOneMillion = 1'000'000;
    const std::string one_million_a(kOneMillion, 'a');
    const fs::path large_file = sandbox.write_text("million_a.bin", one_million_a);
    EXPECT_EQ(FingerprintCache::sha256_file(large_file),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Missing file returns empty digest.
    EXPECT_EQ(FingerprintCache::sha256_file(sandbox.child("missing.bin")), "");

    // Sign-bit bytes (0x00, 0x7F, 0x80, 0xFF) and Windows Ctrl-Z (0x1A) binary-mode seam.
    const std::array<std::vector<std::byte>, 6> binary_payloads{{
        test_support::to_bytes({0x00, 0x7F, 0x80, 0xFF}),
        test_support::to_bytes({0x00, 0x7F, 0x00, 0xFF}),
        test_support::to_bytes({0x00, 0x7F, 0x7F, 0xFF}),
        test_support::to_bytes({0x00, 0x7F, 0x80, 0x7F}),
        test_support::to_bytes({0x41, 0x1A, 0x00, 0x42}),
        test_support::to_bytes({0x41, 0x1A, 0x00, 0x43}),
    }};

    std::array<std::string, binary_payloads.size()> digests;
    for (std::size_t i = 0; i < binary_payloads.size(); ++i) {
        const fs::path p = sandbox.write_bytes("bin_" + std::to_string(i) + ".dat",
                                               binary_payloads[i]);
        digests[i] = FingerprintCache::sha256_file(p);
        SCOPED_TRACE(i);
        EXPECT_TRUE(is_lowercase_sha256_hex(digests[i]));
    }
    for (std::size_t i = 0; i < digests.size(); ++i) {
        for (std::size_t j = i + 1; j < digests.size(); ++j) {
            EXPECT_NE(digests[i], digests[j]);
        }
    }
}

TEST(FingerprintCacheTest, ComputeDirFingerprintIsOrderIndependentPrefixFilteredAndSensitive) {
    test_support::ScopedTempDir sandbox("ut_dir_fp");
    const fs::path dir_a = sandbox.create_subdir("order_a");
    const fs::path dir_b = sandbox.create_subdir("order_b");

    // Different creation order, identical jpg contents -> identical sorted "name:sha256" lines.
    sandbox.write_text("order_a/striking_0002.jpg", "");
    sandbox.write_text("order_a/striking_0001.jpg", "abc");
    sandbox.write_text("order_b/striking_0001.jpg", "abc");
    sandbox.write_text("order_b/striking_0002.jpg", "");

    const std::string fp_a = FingerprintCache::compute_dir_fingerprint(dir_a);
    const std::string fp_b = FingerprintCache::compute_dir_fingerprint(dir_b);
    EXPECT_EQ(fp_a, fp_b);

    constexpr std::string_view kEntry1 =
        "striking_0001.jpg:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr std::string_view kEntry2 =
        "striking_0002.jpg:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const std::size_t pos1 = fp_a.find(kEntry1);
    const std::size_t pos2 = fp_a.find(kEntry2);
    ASSERT_NE(pos1, std::string::npos);
    ASSERT_NE(pos2, std::string::npos);
    EXPECT_LT(pos1, pos2);

    // Non-.jpg files and subdirectories must not alter the fingerprint.
    sandbox.write_text("order_a/notes.txt", "ignored");
    sandbox.write_text("order_a/.detect_hash", "ignored");
    (void)sandbox.create_subdir("order_a/subdir.jpg");
    EXPECT_EQ(FingerprintCache::compute_dir_fingerprint(dir_a), fp_a);

    // Prefix filtering ignores non-matching .jpg files and tracks matching ones.
    sandbox.write_text("order_a/other_0001.jpg", "unrelated_v1");
    const std::string fp_prefix_v1 =
        FingerprintCache::compute_dir_fingerprint(dir_a, "striking_");
    EXPECT_EQ(fp_prefix_v1, fp_a);
    EXPECT_NE(FingerprintCache::compute_dir_fingerprint(dir_a, ""), fp_prefix_v1);

    sandbox.write_text("order_a/other_0001.jpg", "unrelated_v2_modified");
    EXPECT_EQ(FingerprintCache::compute_dir_fingerprint(dir_a, "striking_"), fp_prefix_v1);

    // Mutating a single byte in a matching .jpg changes the fingerprint.
    sandbox.write_text("order_a/striking_0002.jpg", "x");
    const std::string fp_mutated =
        FingerprintCache::compute_dir_fingerprint(dir_a, "striking_");
    EXPECT_NE(fp_mutated, fp_prefix_v1);

    // Deleting a matching .jpg changes the fingerprint again.
    fs::remove(dir_a / "striking_0002.jpg");
    const std::string fp_deleted =
        FingerprintCache::compute_dir_fingerprint(dir_a, "striking_");
    EXPECT_NE(fp_deleted, fp_mutated);
    EXPECT_NE(fp_deleted, fp_prefix_v1);

    // Missing and empty directories are handled cleanly.
    EXPECT_EQ(FingerprintCache::compute_dir_fingerprint(sandbox.child("missing_dir")), "");
    const fs::path empty_dir = sandbox.create_subdir("empty_dir");
    const fs::path non_jpg_dir = sandbox.create_subdir("non_jpg_dir");
    sandbox.write_text("non_jpg_dir/readme.txt", "no images here");
    EXPECT_EQ(FingerprintCache::compute_dir_fingerprint(empty_dir), "");
    EXPECT_EQ(FingerprintCache::compute_dir_fingerprint(non_jpg_dir), "");
}

TEST(FingerprintCacheTest, SaveAndLoadRoundTripAndOverwrite) {
    test_support::ScopedTempDir sandbox("ut_fp_io");
    const fs::path hash_file = sandbox.child(".cache_hash");

    EXPECT_EQ(FingerprintCache::load(hash_file), "");

    constexpr std::string_view kFirstValue =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    FingerprintCache::save(hash_file, kFirstValue);
    EXPECT_EQ(FingerprintCache::load(hash_file), kFirstValue);

    // Overwriting with a shorter string must truncate prior bytes.
    constexpr std::string_view kSecondValue = "short_v2";
    FingerprintCache::save(hash_file, kSecondValue);
    EXPECT_EQ(FingerprintCache::load(hash_file), kSecondValue);

    // Round-trip an actual multi-file directory fingerprint.
    const fs::path img_dir = sandbox.create_subdir("imgs");
    sandbox.write_text("imgs/a.jpg", "aaa");
    sandbox.write_text("imgs/b.jpg", "bbb");
    const std::string dir_fp = FingerprintCache::compute_dir_fingerprint(img_dir);
    FingerprintCache::save(hash_file, dir_fp);
    EXPECT_EQ(FingerprintCache::load(hash_file), dir_fp);
}

// CropFilter Chain of Responsibility

TEST(CropFilterTest, ContrastFilterEnforcesExactStddevBoundary) {
    constexpr int kPatchSide = 64;
    const ContrastFilter filter;

    EXPECT_FALSE(filter.accept(cv::Mat{}));
    EXPECT_FALSE(filter.accept(cv::Mat(kPatchSide, kPatchSide, CV_8UC1, cv::Scalar(128))));

    struct StddevCase {
        int delta;
        bool expected_accept;
    };
    constexpr std::array<StddevCase, 3> kBoundaryCases{{
        {24, false},
        {25, true},
        {26, true},
    }};

    for (const auto& tc : kBoundaryCases) {
        SCOPED_TRACE(tc.delta);
        const cv::Mat patch = make_exact_stddev_patch(kPatchSide, tc.delta);
        EXPECT_EQ(filter.accept(patch), tc.expected_accept);
    }
}

TEST(CropFilterTest, EdgeDensityFilterRejectsFlatAndLowEdgeAcceptsDenseGrid) {
    const EdgeDensityFilter filter;

    EXPECT_FALSE(filter.accept(cv::Mat{}));
    EXPECT_FALSE(filter.accept(cv::Mat(64, 128, CV_8UC1, cv::Scalar(128))));

    // Single sharp vertical split on a 200x200 image produces < 1.5% edges (< 5% threshold).
    cv::Mat single_edge(200, 200, CV_8UC1, cv::Scalar(30));
    single_edge(cv::Rect(100, 0, 100, 200)).setTo(cv::Scalar(225));
    EXPECT_FALSE(filter.accept(single_edge));

    // Dense 6px black/white checkerboard avoids 3x3 Sobel/Canny NMS aliasing
    // while yielding well over 5% edge density (~25-30%).
    cv::Mat dense_grid(64, 128, CV_8UC1);
    for (int r = 0; r < dense_grid.rows; ++r) {
        auto* row = dense_grid.ptr<std::uint8_t>(r);
        for (int c = 0; c < dense_grid.cols; ++c) {
            row[c] = (((r / 6) + (c / 6)) % 2 == 0) ? 0 : 255;
        }
    }
    EXPECT_TRUE(filter.accept(dense_grid));
}

TEST(CropFilterTest, ComponentCountFilterRejectsUniformAndSingleBlobAcceptsTextRow) {
    const ComponentCountFilter filter;

    EXPECT_FALSE(filter.accept(cv::Mat{}));
    EXPECT_FALSE(filter.accept(cv::Mat(64, 200, CV_8UC1, cv::Scalar(220))));

    // One isolated rectangle on a 300px-wide canvas: 1 * 100 / 300 = 0.33 < 1.0.
    cv::Mat single_blob(64, 300, CV_8UC1, cv::Scalar(240));
    cv::rectangle(single_blob, cv::Rect(130, 16, 20, 32), cv::Scalar(15), cv::FILLED);
    EXPECT_FALSE(filter.accept(single_blob));

    // Eight well-separated hollow dark glyph rings on a 200px-wide canvas:
    // produces >= 8 components (>= 4.0 per 100px) under either Otsu threshold polarity.
    cv::Mat glyph_row(64, 200, CV_8UC1, cv::Scalar(240));
    constexpr int kGlyphCount = 8;
    for (int i = 0; i < kGlyphCount; ++i) {
        const int x = 10 + (i * 22);
        cv::rectangle(glyph_row, cv::Rect(x, 16, 14, 30), cv::Scalar(15), 3);
    }
    EXPECT_TRUE(filter.accept(glyph_row));
}

// CropDeduplicator

TEST(CropDeduplicatorTest, HandlesEmptyMultiItemResizeThresholdAndChannelCount) {
    const CropDeduplicator dedup(0.85);

    const cv::Mat h_bgr = make_stripe_pattern(80, 120, true, 3);
    const cv::Mat v_bgr = make_stripe_pattern(80, 120, false, 3);
    const cv::Mat diag_bgr = make_diagonal_checker(80, 120);
    const cv::Mat h_gray = make_stripe_pattern(80, 120, true, 1);
    const cv::Mat h_bgra = make_stripe_pattern(80, 120, true, 4);

    // Empty candidate always rejected; valid candidate against empty set accepted.
    const std::span<const cv::Mat> empty_saved{};
    EXPECT_FALSE(dedup.is_unique(cv::Mat{}, empty_saved));
    EXPECT_TRUE(dedup.is_unique(h_bgr, empty_saved));
    EXPECT_FALSE(dedup.is_unique(cv::Mat{}, {h_bgr}));

    // Identical images rejected across 1ch, 3ch, and 4ch inputs.
    EXPECT_FALSE(dedup.is_unique(h_bgr.clone(), {h_bgr}));
    EXPECT_FALSE(dedup.is_unique(h_gray.clone(), {h_gray}));
    EXPECT_FALSE(dedup.is_unique(h_bgra.clone(), {h_bgra}));
    EXPECT_FALSE(dedup.is_unique(h_gray, {h_bgr}));
    EXPECT_FALSE(dedup.is_unique(h_bgra, {h_bgr}));

    // Multi-item set: duplicate matches item #2 of 3 (middle element).
    const std::vector<cv::Mat> saved_three{h_bgr, v_bgr, diag_bgr};
    EXPECT_FALSE(dedup.is_unique(v_bgr.clone(), std::span<const cv::Mat>{saved_three}));

    // Distinct patterns accepted as unique.
    EXPECT_TRUE(dedup.is_unique(v_bgr, {h_bgr}));

    // Resize invariance: exact same visual pattern scaled 2x must deduplicate.
    cv::Mat h_double;
    cv::resize(h_bgr, h_double, cv::Size(240, 160), 0.0, 0.0, cv::INTER_NEAREST);
    EXPECT_FALSE(dedup.is_unique(h_double, {h_bgr}));

    // Threshold seam: 50/50 blend of two orthogonal half-split patterns has
    // Pearson r ~= 0.707 and uncentered cosine ~= 0.866 — strictly in (0.50, 0.999).
    cv::Mat half_h(64, 64, CV_8UC3, cv::Scalar(0, 0, 0));
    half_h(cv::Rect(0, 32, 64, 32)).setTo(cv::Scalar(255, 255, 255));
    cv::Mat half_v(64, 64, CV_8UC3, cv::Scalar(0, 0, 0));
    half_v(cv::Rect(32, 0, 32, 64)).setTo(cv::Scalar(255, 255, 255));
    cv::Mat blended;
    cv::addWeighted(half_h, 0.5, half_v, 0.5, 0.0, blended);

    const CropDeduplicator loose(0.50);
    const CropDeduplicator strict(0.999);
    EXPECT_FALSE(loose.is_unique(blended, {half_h}));
    EXPECT_TRUE(strict.is_unique(blended, {half_h}));
}

// FrameScorer

TEST(FrameScorerTest, RejectsInvalidFilesAndRanksSharpAboveFlatAndBlurred) {
    test_support::ScopedTempDir sandbox("ut_frame_scorer");
    const FrameScorer scorer;

    EXPECT_DOUBLE_EQ(scorer.score(sandbox.child("missing.jpg")), 0.0);
    EXPECT_DOUBLE_EQ(scorer.score(sandbox.write_bytes("empty.jpg", {})), 0.0);
    EXPECT_DOUBLE_EQ(scorer.score(sandbox.write_text("corrupt.jpg", "not_a_jpeg")), 0.0);

    constexpr int kImgSide = 256;
    const cv::Mat flat(kImgSide, kImgSide, CV_8UC3, cv::Scalar(128, 128, 128));
    const cv::Mat sharp = make_vibrant_checkerboard(kImgSide, 8);
    cv::Mat blurred;
    cv::GaussianBlur(sharp, blurred, cv::Size(21, 21), 8.0);

    const fs::path flat_path = sandbox.child("flat.jpg");
    const fs::path sharp_path = sandbox.child("sharp.jpg");
    const fs::path blurred_path = sandbox.child("blurred.jpg");
    ASSERT_TRUE(write_jpeg(flat_path, flat));
    ASSERT_TRUE(write_jpeg(sharp_path, sharp));
    ASSERT_TRUE(write_jpeg(blurred_path, blurred));

    const double score_flat = scorer.score(flat_path);
    const double score_sharp = scorer.score(sharp_path);
    const double score_blurred = scorer.score(blurred_path);

    EXPECT_GT(score_sharp, 0.0);
    EXPECT_GT(score_sharp, score_flat);
    EXPECT_GT(score_sharp, score_blurred);
}

// MosaicBuilder

TEST(MosaicBuilderTest, BuildsHonorsStartOffsetCachesHitsAndRejectsInvalidParams) {
    test_support::ScopedTempDir sandbox("ut_mosaic");
    const fs::path detect_dir = sandbox.create_subdir("detect");
    const fs::path mosaic_dir = sandbox.create_subdir("mosaic");

    // Six solid distinct-color frames: 1=Red, 2=Green, 3=Blue, 4=Yellow, 5=Cyan, 6=Magenta.
    const std::array<cv::Scalar, 6> colors{{
        cv::Scalar(0, 0, 255),
        cv::Scalar(0, 255, 0),
        cv::Scalar(255, 0, 0),
        cv::Scalar(0, 255, 255),
        cv::Scalar(255, 255, 0),
        cv::Scalar(255, 0, 255),
    }};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const cv::Mat frame(180, 320, CV_8UC3, colors[i]);
        const fs::path frame_path =
            detect_dir / ("striking_000" + std::to_string(i + 1) + ".jpg");
        ASSERT_TRUE(write_jpeg(frame_path, frame));
    }

    const auto is_red = [](const cv::Vec3b& p) { return p[2] > 180 && p[1] < 60 && p[0] < 60; };
    const auto is_green = [](const cv::Vec3b& p) { return p[1] > 180 && p[2] < 60 && p[0] < 60; };
    const auto is_later_palette = [](const cv::Vec3b& p) {
        const bool blue = p[0] > 180 && p[1] < 60 && p[2] < 60;
        const bool yellow = p[2] > 180 && p[1] > 180 && p[0] < 60;
        const bool cyan = p[0] > 180 && p[1] > 180 && p[2] < 60;
        const bool magenta = p[0] > 180 && p[2] > 180 && p[1] < 60;
        return blue || yellow || cyan || magenta;
    };

    const MosaicBuilder builder;
    const fs::path mosaic_path = mosaic_dir / "mosaic.jpg";
    const fs::path hash_path = mosaic_dir / ".mosaic_hash";

    // Build with count=2, start=1: must include frames from start=1 (red/green region present).
    ASSERT_TRUE(builder.build(detect_dir, mosaic_dir, 2, 1));
    ASSERT_TRUE(fs::is_regular_file(mosaic_path));
    ASSERT_TRUE(fs::is_regular_file(hash_path));
    const std::string hash_start1 = FingerprintCache::load(hash_path);
    EXPECT_FALSE(hash_start1.empty());

    const cv::Mat img_start1 = cv::imread(mosaic_path.string());
    ASSERT_FALSE(img_start1.empty());
    EXPECT_EQ(img_start1.cols, kMosaicWidth);
    EXPECT_EQ(img_start1.rows, kMosaicHeight);
    EXPECT_GT(count_bgr_pixels(img_start1, is_red) + count_bgr_pixels(img_start1, is_green),
              100'000);

    // Build with count=2, start=3: frames 1 and 2 excluded; hash and pixels must change.
    ASSERT_TRUE(builder.build(detect_dir, mosaic_dir, 2, 3));
    const std::string hash_start3 = FingerprintCache::load(hash_path);
    EXPECT_FALSE(hash_start3.empty());
    EXPECT_NE(hash_start3, hash_start1);

    const cv::Mat img_start3 = cv::imread(mosaic_path.string());
    ASSERT_FALSE(img_start3.empty());
    EXPECT_EQ(img_start3.cols, kMosaicWidth);
    EXPECT_EQ(img_start3.rows, kMosaicHeight);
    EXPECT_LT(count_bgr_pixels(img_start3, is_red) + count_bgr_pixels(img_start3, is_green),
              1000);
    EXPECT_GT(count_bgr_pixels(img_start3, is_later_palette), 200'000);
    EXPECT_GT(cv::norm(img_start1, img_start3, cv::NORM_L1), 0.0);

    // Cache hit: pin mtime in past and re-run identical parameters -> zero rewrite.
    const auto pinned_mtime = test_support::pin_mtime_in_past(mosaic_path);
    ASSERT_TRUE(builder.build(detect_dir, mosaic_dir, 2, 3));
    EXPECT_EQ(fs::last_write_time(mosaic_path), pinned_mtime);

    // Invalid and out-of-range parameters return false and produce no mosaic.jpg.
    struct InvalidCase {
        std::string_view label;
        int count;
        int start;
    };
    constexpr std::array<InvalidCase, 6> kInvalidCases{{
        {"zero_count", 0, 1},
        {"negative_count", -1, 1},
        {"zero_start", 2, 0},
        {"negative_start", 2, -1},
        {"start_past_end", 2, 7},
        {"insufficient_frames_from_start", 5, 3},
    }};

    for (const auto& tc : kInvalidCases) {
        SCOPED_TRACE(tc.label);
        const fs::path out = sandbox.create_subdir(std::string("bad_") + std::string(tc.label));
        EXPECT_FALSE(builder.build(detect_dir, out, tc.count, tc.start));
        EXPECT_FALSE(fs::exists(out / "mosaic.jpg"));
    }

    const fs::path out_missing = sandbox.create_subdir("bad_missing_dir");
    EXPECT_FALSE(builder.build(sandbox.child("nonexistent_detect"), out_missing, 2, 1));
    EXPECT_FALSE(fs::exists(out_missing / "mosaic.jpg"));

    const fs::path empty_detect = sandbox.create_subdir("empty_detect");
    const fs::path out_empty = sandbox.create_subdir("bad_empty_dir");
    EXPECT_FALSE(builder.build(empty_detect, out_empty, 2, 1));
    EXPECT_FALSE(fs::exists(out_empty / "mosaic.jpg"));
}

// TextRegionDetector

TEST(TextRegionDetectorTest, RejectsEmptyAndUniformAndLocatesSyntheticTextBand) {
    const TextRegionDetector detector;

    EXPECT_TRUE(detector.detect(cv::Mat{}).empty());
    EXPECT_TRUE(detector.detect(cv::Mat(200, 800, CV_8UC3, cv::Scalar(128, 128, 128))).empty());

    constexpr int kCanvasWidth = 800;
    constexpr int kCanvasHeight = 200;
    constexpr double kFontScale = 1.4;
    constexpr int kThickness = 3;
    const cv::Point origin(120, 115);
    const std::string text = "HELLO WORLD 2026";

    cv::Mat canvas(kCanvasHeight, kCanvasWidth, CV_8UC3, cv::Scalar(245, 245, 245));
    int baseline = 0;
    const cv::Size text_size =
        cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, kFontScale, kThickness, &baseline);
    cv::putText(canvas, text, origin, cv::FONT_HERSHEY_SIMPLEX, kFontScale,
                cv::Scalar(15, 15, 15), kThickness, cv::LINE_AA);

    const cv::Rect text_band(origin.x, origin.y - text_size.height,
                             text_size.width, text_size.height + baseline);

    const std::vector<cv::Rect> rects = detector.detect(canvas);
    ASSERT_GE(rects.size(), 1u);

    bool overlaps_text_band = false;
    for (const auto& r : rects) {
        EXPECT_GT(r.width, 0);
        EXPECT_GT(r.height, 0);
        EXPECT_GE(r.x, 0);
        EXPECT_GE(r.y, 0);
        EXPECT_LE(r.x + r.width, kCanvasWidth);
        EXPECT_LE(r.y + r.height, kCanvasHeight);
        if ((r & text_band).area() > 0) {
            overlaps_text_band = true;
        }
    }
    EXPECT_TRUE(overlaps_text_band);
}
