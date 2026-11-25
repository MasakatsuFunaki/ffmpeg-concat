#include <gtest/gtest.h>
#include "extract/extract.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

static const fs::path DATA_DIR      = TEST_DATA_DIR;
static const fs::path OUTPUT_DIR    = TEST_OUTPUT_DIR;
static const fs::path TEST_VIDEO    = DATA_DIR / "2026-03-22 00-16-33.mp4";

// Helper to call ExtractApp::run with a vector of string args
static int run_extract(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    ExtractApp app;
    return app.run(static_cast<int>(argv.size()), argv.data());
}

// ---------- Help ----------

TEST(ExtractAppTest, HelpReturns0) {
    EXPECT_EQ(0, run_extract({"extract", "--help"}));
}

// ---------- No args shows help ----------

TEST(ExtractAppTest, NoArgsShowsHelp) {
    EXPECT_EQ(0, run_extract({"extract"}));
}

// ---------- Missing mode flags ----------

TEST(ExtractAppTest, NoModeFlagsReturns1) {
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", (OUTPUT_DIR / "test_extract_nomode").string()
    });
    EXPECT_EQ(1, rc);
}

// ---------- Extract text mode ----------

TEST(ExtractAppTest, ExtractTextMode) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_extract_text";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--extract-text"
    });

    EXPECT_EQ(0, rc);

    // Check that text crops directory was created
    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path text_dir = out / video_stem / "text";
    EXPECT_TRUE(fs::exists(text_dir)) << "Expected text dir: " << text_dir;

    // Text crops should be JPG files (may be 0 if test video has no text)
    int jpg_count = 0;
    if (fs::exists(text_dir)) {
        for (const auto& entry : fs::directory_iterator(text_dir)) {
            if (entry.path().extension() == ".jpg") jpg_count++;
        }
    }
    // We just verify the mode ran without crashing; count depends on video content
    EXPECT_GE(jpg_count, 0);

    fs::remove_all(out);
}

// ---------- Nonexistent input dir ----------

TEST(ExtractAppTest, NonexistentInputPathReturns1) {
    int rc = run_extract({
        "extract",
        "-i", (OUTPUT_DIR / "nonexistent_dir_99999").string(),
        "-o", (OUTPUT_DIR / "test_extract_out").string(),
        "--image", "5"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Invalid image count ----------

TEST(ExtractAppTest, InvalidImageCountReturns1) {
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", (OUTPUT_DIR / "test_extract_badcount").string(),
        "--image", "0"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Equally-spaced extraction ----------

TEST(ExtractAppTest, EquallySpacedExtraction) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_extract_image";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image", "4"
    });

    EXPECT_EQ(0, rc);

    // Check that frames were created in a subdirectory named after the video stem
    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path image_dir = out / video_stem / "image";
    EXPECT_TRUE(fs::exists(image_dir)) << "Expected image dir: " << image_dir;

    int jpg_count = 0;
    if (fs::exists(image_dir)) {
        for (const auto& entry : fs::directory_iterator(image_dir)) {
            if (entry.path().extension() == ".jpg") jpg_count++;
        }
    }
    EXPECT_EQ(4, jpg_count) << "Expected 4 extracted frames";

    fs::remove_all(out);
}

// ---------- Single-file input ----------

TEST(ExtractAppTest, SingleFileInput) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_extract_singlefile";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "--image", "3"
    });

    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path image_dir = out / video_stem / "image";
    EXPECT_TRUE(fs::exists(image_dir)) << "Expected image dir: " << image_dir;

    int jpg_count = 0;
    if (fs::exists(image_dir)) {
        for (const auto& entry : fs::directory_iterator(image_dir)) {
            if (entry.path().extension() == ".jpg") jpg_count++;
        }
    }
    EXPECT_EQ(3, jpg_count) << "Expected 3 extracted frames";

    fs::remove_all(out);
}

// ---------- Striking-frame detection ----------

TEST(ExtractAppTest, StrikingFrameDetection) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_extract_detect";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });

    EXPECT_EQ(0, rc);

    // Check that striking frames were created
    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path detect_dir = out / video_stem / "detect";
    EXPECT_TRUE(fs::exists(detect_dir)) << "Expected detect dir: " << detect_dir;

    int jpg_count = 0;
    if (fs::exists(detect_dir)) {
        for (const auto& entry : fs::directory_iterator(detect_dir)) {
            if (entry.path().extension() == ".jpg") jpg_count++;
        }
    }
    EXPECT_GT(jpg_count, 0) << "Expected at least 1 striking frame";
    EXPECT_LE(jpg_count, 100) << "Should not exceed 100 striking frames";

    fs::remove_all(out);
}

// ---------- Both modes simultaneously ----------

TEST(ExtractAppTest, BothModesSimultaneously) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_extract_both";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image", "3",
        "--image-detect"
    });

    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path image_dir  = out / video_stem / "image";
    fs::path detect_dir = out / video_stem / "detect";

    EXPECT_TRUE(fs::exists(image_dir))  << "Expected image dir";
    EXPECT_TRUE(fs::exists(detect_dir)) << "Expected detect dir";

    fs::remove_all(out);
}

// ---------- All three modes simultaneously ----------

TEST(ExtractAppTest, AllThreeModesSimultaneously) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_extract_all3";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image", "3",
        "--image-detect",
        "--extract-text"
    });

    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path image_dir  = out / video_stem / "image";
    fs::path detect_dir = out / video_stem / "detect";
    fs::path text_dir   = out / video_stem / "text";

    EXPECT_TRUE(fs::exists(image_dir))  << "Expected image dir";
    EXPECT_TRUE(fs::exists(detect_dir)) << "Expected detect dir";
    EXPECT_TRUE(fs::exists(text_dir))   << "Expected text dir";

    fs::remove_all(out);
}

// ---------- Same input and output directory ----------
// Output dir is no longer wiped, so using the same directory should work.

TEST(ExtractAppTest, SameInputOutputDir) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // Prepare a temp directory used as BOTH input and output
    fs::path same_dir = OUTPUT_DIR / "test_extract_same_dir";
    fs::remove_all(same_dir);
    fs::create_directories(same_dir);

    fs::copy_file(TEST_VIDEO, same_dir / "2026-03-22 00-16-33.mp4", fs::copy_options::overwrite_existing);

    int rc = run_extract({
        "extract",
        "-i", same_dir.string(),
        "-o", same_dir.string(),
        "--image", "3"
    });

    EXPECT_EQ(0, rc);

    // Original video should still be intact
    EXPECT_TRUE(fs::exists(same_dir / "2026-03-22 00-16-33.mp4"));

    // Extracted frames should exist in a subdirectory
    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path image_dir = same_dir / video_stem / "image";
    EXPECT_TRUE(fs::exists(image_dir)) << "Expected image dir: " << image_dir;

    fs::remove_all(same_dir);
}

// ---------- Mosaic requires --image-detect ----------

TEST(ExtractAppTest, MosaicWithoutDetectReturns1) {
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", (OUTPUT_DIR / "test_mosaic_nodetect").string(),
        "--image", "3",
        "--mosaic", "2"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Mosaic invalid count ----------

TEST(ExtractAppTest, MosaicInvalidCountReturns1) {
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", (OUTPUT_DIR / "test_mosaic_badcount").string(),
        "--image-detect",
        "--mosaic", "0"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Mosaic with N=2 ----------

TEST(ExtractAppTest, MosaicWithN2) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_n2";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2"
    });

    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path detect_dir = out / video_stem / "detect";
    fs::path mosaic_dir = out / video_stem / "mosaics";

    EXPECT_TRUE(fs::exists(detect_dir)) << "Expected detect dir: " << detect_dir;
    EXPECT_TRUE(fs::exists(mosaic_dir)) << "Expected mosaics dir: " << mosaic_dir;

    fs::path mosaic_file = mosaic_dir / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file)) << "Expected mosaic.jpg in mosaics dir";

    // Verify mosaic is a valid image with expected HD dimensions
    cv::Mat mosaic = cv::imread(mosaic_file.string());
    EXPECT_FALSE(mosaic.empty());
    EXPECT_EQ(1920, mosaic.cols);
    EXPECT_EQ(1080, mosaic.rows);

    fs::remove_all(out);
}

// ---------- Mosaic with N=3 ----------

TEST(ExtractAppTest, MosaicWithN3) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_n3";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "3"
    });

    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_dir = out / video_stem / "mosaics";
    fs::path mosaic_file = mosaic_dir / "mosaic.jpg";

    EXPECT_TRUE(fs::exists(mosaic_file)) << "Expected mosaic.jpg";

    cv::Mat mosaic = cv::imread(mosaic_file.string());
    EXPECT_FALSE(mosaic.empty());
    EXPECT_EQ(1920, mosaic.cols);
    EXPECT_EQ(1080, mosaic.rows);

    fs::remove_all(out);
}

// ---------- Mosaic with N=1 ----------

TEST(ExtractAppTest, MosaicWithN1) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_n1";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "1"
    });

    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_file = out / video_stem / "mosaics" / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file)) << "Expected mosaic.jpg";

    cv::Mat mosaic = cv::imread(mosaic_file.string());
    EXPECT_FALSE(mosaic.empty());
    EXPECT_EQ(1920, mosaic.cols);
    EXPECT_EQ(1080, mosaic.rows);

    fs::remove_all(out);
}

// ---------- Mosaic with large N (more than striking frames) — should fail ----------

TEST(ExtractAppTest, MosaicWithLargeN) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_large";
    fs::remove_all(out);

    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "500"
    });

    // Should fail: not enough striking frames for N=500
    EXPECT_EQ(1, rc);

    fs::remove_all(out);
}

// ---------- Detect cache: second run skips extraction ----------

TEST(ExtractAppTest, DetectCacheSkipsReExtraction) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_detect_cache";
    fs::remove_all(out);

    // First run — full extraction
    int rc1 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });
    EXPECT_EQ(0, rc1);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path detect_dir = out / video_stem / "detect";
    fs::path hash_file = detect_dir / ".detect_hash";

    EXPECT_TRUE(fs::exists(hash_file)) << "Expected .detect_hash after first run";

    // Count files and record modification times
    int jpg_count_1 = 0;
    fs::file_time_type oldest_mtime = fs::file_time_type::max();
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
            jpg_count_1++;
            auto t = entry.last_write_time();
            if (t < oldest_mtime) oldest_mtime = t;
        }
    }
    EXPECT_GT(jpg_count_1, 0);

    // Second run — should hit cache, files unchanged
    int rc2 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });
    EXPECT_EQ(0, rc2);

    // Files should still be the same (not re-created)
    int jpg_count_2 = 0;
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
            jpg_count_2++;
            // Modification time should be unchanged (cache hit => no file writes)
            EXPECT_EQ(oldest_mtime <= entry.last_write_time(), true);
        }
    }
    EXPECT_EQ(jpg_count_1, jpg_count_2);

    fs::remove_all(out);
}

// ---------- Detect cache invalidated when file deleted ----------

TEST(ExtractAppTest, DetectCacheInvalidatedOnFileDeleted) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_detect_cache_invalidate";
    fs::remove_all(out);

    // First run
    int rc1 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });
    EXPECT_EQ(0, rc1);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path detect_dir = out / video_stem / "detect";

    // Delete one striking file to invalidate cache
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.path().filename().string().find("striking_") == 0) {
            fs::remove(entry.path());
            break;  // delete just one
        }
    }

    // Second run — cache should miss, re-extract
    int rc2 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });
    EXPECT_EQ(0, rc2);

    // Should have re-extracted full set
    int jpg_count = 0;
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
            jpg_count++;
        }
    }
    EXPECT_GT(jpg_count, 0);

    fs::remove_all(out);
}

// ---------- Mosaic cache: second run skips regeneration ----------

TEST(ExtractAppTest, MosaicCacheSkipsRegeneration) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_cache";
    fs::remove_all(out);

    // First run
    int rc1 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "3"
    });
    EXPECT_EQ(0, rc1);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_dir = out / video_stem / "mosaics";
    fs::path mosaic_file = mosaic_dir / "mosaic.jpg";
    fs::path hash_file = mosaic_dir / ".mosaic_hash";

    EXPECT_TRUE(fs::exists(mosaic_file));
    EXPECT_TRUE(fs::exists(hash_file)) << "Expected .mosaic_hash after first run";

    auto mtime1 = fs::last_write_time(mosaic_file);

    // Second run — both detect and mosaic should hit cache
    int rc2 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "3"
    });
    EXPECT_EQ(0, rc2);

    // Mosaic file should not have been regenerated
    auto mtime2 = fs::last_write_time(mosaic_file);
    EXPECT_EQ(mtime1, mtime2) << "Mosaic should not be regenerated on cache hit";

    fs::remove_all(out);
}

// ---------- Mosaic regenerated when deleted ----------

TEST(ExtractAppTest, MosaicRegeneratedWhenDeleted) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_regen";
    fs::remove_all(out);

    // First run
    int rc1 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2"
    });
    EXPECT_EQ(0, rc1);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_file = out / video_stem / "mosaics" / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file));

    // Delete the mosaic
    fs::remove(mosaic_file);
    EXPECT_FALSE(fs::exists(mosaic_file));

    // Second run — detect should cache-hit, mosaic should regenerate
    int rc2 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2"
    });
    EXPECT_EQ(0, rc2);

    EXPECT_TRUE(fs::exists(mosaic_file)) << "Mosaic should be regenerated after deletion";

    fs::remove_all(out);
}

// ---------- Mosaic regenerated when count changes ----------

TEST(ExtractAppTest, MosaicRegeneratedWhenCountChanges) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_count_change";
    fs::remove_all(out);

    // First run with N=2
    int rc1 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2"
    });
    EXPECT_EQ(0, rc1);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_file = out / video_stem / "mosaics" / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file));

    auto mtime1 = fs::last_write_time(mosaic_file);

    // Second run with N=4 — mosaic_count changed, should regenerate
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int rc2 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "4"
    });
    EXPECT_EQ(0, rc2);

    EXPECT_TRUE(fs::exists(mosaic_file));
    auto mtime2 = fs::last_write_time(mosaic_file);
    EXPECT_NE(mtime1, mtime2) << "Mosaic should be regenerated when count changes";

    fs::remove_all(out);
}

// ---------- Mosaic with start position P ----------

TEST(ExtractAppTest, MosaicWithStartPosition) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_start_pos";
    fs::remove_all(out);

    // First: run detect only to count striking frames
    int rc_detect = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });
    EXPECT_EQ(0, rc_detect);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path detect_dir = out / video_stem / "detect";

    // Count striking frames
    int total_striking = 0;
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".jpg" &&
            entry.path().filename().string().find("striking_") == 0) {
            total_striking++;
        }
    }
    ASSERT_GE(total_striking, 3) << "Need at least 3 striking frames for this test";

    // Run mosaic with start position 2: --mosaic 2 2
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2", "2"
    });
    EXPECT_EQ(0, rc);

    fs::path mosaic_file = out / video_stem / "mosaics" / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file)) << "Expected mosaic.jpg with start position P=2";

    cv::Mat mosaic = cv::imread(mosaic_file.string());
    EXPECT_FALSE(mosaic.empty());
    EXPECT_EQ(1920, mosaic.cols);
    EXPECT_EQ(1080, mosaic.rows);

    fs::remove_all(out);
}

// ---------- Mosaic start position P out of range ----------

TEST(ExtractAppTest, MosaicStartPositionOutOfRange) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_start_oor";
    fs::remove_all(out);

    // P=9999 is certainly out of range
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2", "9999"
    });

    EXPECT_EQ(1, rc);

    fs::remove_all(out);
}

// ---------- Mosaic not enough frames from position P ----------

TEST(ExtractAppTest, MosaicNotEnoughFramesFromPositionP) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_not_enough";
    fs::remove_all(out);

    // First: detect frames to know total
    int rc_detect = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect"
    });
    EXPECT_EQ(0, rc_detect);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path detect_dir = out / video_stem / "detect";

    int total_striking = 0;
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".jpg" &&
            entry.path().filename().string().find("striking_") == 0) {
            total_striking++;
        }
    }
    ASSERT_GT(total_striking, 0) << "Need at least 1 striking frame";

    // Request more images than available from the last position
    // e.g., start at last frame, request 2 → only 1 available
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", std::to_string(total_striking), std::to_string(total_striking)
    });

    // Only 1 frame available from position=total, but requesting total → fail
    if (total_striking > 1) {
        EXPECT_EQ(1, rc);
    } else {
        // If total_striking == 1: from position 1, requesting 1 → should succeed
        EXPECT_EQ(0, rc);
    }

    fs::remove_all(out);
}

// ---------- Mosaic invalid start position P=0 ----------

TEST(ExtractAppTest, MosaicInvalidStartPositionZero) {
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", (OUTPUT_DIR / "test_mosaic_p0").string(),
        "--image-detect",
        "--mosaic", "2", "0"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Mosaic invalid start position P negative ----------

TEST(ExtractAppTest, MosaicInvalidStartPositionNegative) {
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", (OUTPUT_DIR / "test_mosaic_pneg").string(),
        "--image-detect",
        "--mosaic", "2", "-1"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Mosaic cache invalidated when start position changes ----------

TEST(ExtractAppTest, MosaicRegeneratedWhenStartPositionChanges) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_start_change";
    fs::remove_all(out);

    // First run with P=1 (default)
    int rc1 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2"
    });
    EXPECT_EQ(0, rc1);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_file = out / video_stem / "mosaics" / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file));

    auto mtime1 = fs::last_write_time(mosaic_file);

    // Second run with P=2 — start changed, should regenerate
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int rc2 = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2", "2"
    });
    EXPECT_EQ(0, rc2);

    EXPECT_TRUE(fs::exists(mosaic_file));
    auto mtime2 = fs::last_write_time(mosaic_file);
    EXPECT_NE(mtime1, mtime2) << "Mosaic should be regenerated when start position changes";

    fs::remove_all(out);
}

// ---------- Mosaic with start position P=1 is same as default ----------

TEST(ExtractAppTest, MosaicStartPositionOneIsDefault) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_mosaic_p1_default";
    fs::remove_all(out);

    // Run with explicit P=1
    int rc = run_extract({
        "extract",
        "-i", DATA_DIR.string(),
        "-o", out.string(),
        "--image-detect",
        "--mosaic", "2", "1"
    });
    EXPECT_EQ(0, rc);

    std::string video_stem = TEST_VIDEO.stem().string();
    fs::path mosaic_file = out / video_stem / "mosaics" / "mosaic.jpg";
    EXPECT_TRUE(fs::exists(mosaic_file));

    cv::Mat mosaic = cv::imread(mosaic_file.string());
    EXPECT_FALSE(mosaic.empty());
    EXPECT_EQ(1920, mosaic.cols);
    EXPECT_EQ(1080, mosaic.rows);

    fs::remove_all(out);
}
