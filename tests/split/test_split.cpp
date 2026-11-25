#include <gtest/gtest.h>
#include "split/split.h"

#include <filesystem>

namespace fs = std::filesystem;

static const fs::path DATA_DIR      = TEST_DATA_DIR;
static const fs::path OUTPUT_DIR    = TEST_OUTPUT_DIR;
static const fs::path TEST_VIDEO    = DATA_DIR / "2026-03-22 00-16-33.mp4";

// Helper to call SplitApp::run with a vector of string args
static int run_split(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    SplitApp app;
    return app.run(static_cast<int>(argv.size()), argv.data());
}

// ---------- Help ----------

TEST(SplitAppTest, HelpReturns0) {
    EXPECT_EQ(0, run_split({"split", "--help"}));
}

// ---------- Missing required args ----------

TEST(SplitAppTest, NoArgsShowsHelp) {
    EXPECT_EQ(0, run_split({"split"}));
}

// ---------- Nonexistent input file ----------

TEST(SplitAppTest, NonexistentInputReturns1) {
    int rc = run_split({
        "split",
        "-i", (OUTPUT_DIR / "nonexistent_video_12345.mp4").string(),
        "-p", "5"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Single split point (absolute) ----------

TEST(SplitAppTest, SingleSplitAbsolute) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_split_abs";
    fs::remove_all(out);
    fs::create_directories(out);

    int rc = run_split({
        "split",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-p", "8"
    });

    EXPECT_EQ(0, rc);

    // Should produce 2 output files
    int mp4_count = 0;
    for (const auto& entry : fs::directory_iterator(out)) {
        if (entry.path().extension() == ".mp4") {
            mp4_count++;
            EXPECT_GT(fs::file_size(entry.path()), 0u);
        }
    }
    EXPECT_EQ(2, mp4_count) << "Split at one point should produce 2 parts";

    fs::remove_all(out);
}

// ---------- Multiple split points ----------

TEST(SplitAppTest, MultipleSplitPoints) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_split_multi";
    fs::remove_all(out);
    fs::create_directories(out);

    int rc = run_split({
        "split",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-p", "4", "10"
    });

    EXPECT_EQ(0, rc);

    // Should produce 3 output files
    int mp4_count = 0;
    for (const auto& entry : fs::directory_iterator(out)) {
        if (entry.path().extension() == ".mp4") mp4_count++;
    }
    EXPECT_EQ(3, mp4_count) << "Split at two points should produce 3 parts";

    fs::remove_all(out);
}

// ---------- Relative split mode ----------

TEST(SplitAppTest, RelativeSplitMode) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_split_rel";
    fs::remove_all(out);
    fs::create_directories(out);

    // 0.5 = split 0.5s from start in relative mode
    int rc = run_split({
        "split",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-p", "0.5",
        "-r"
    });

    EXPECT_EQ(0, rc);

    int mp4_count = 0;
    for (const auto& entry : fs::directory_iterator(out)) {
        if (entry.path().extension() == ".mp4") mp4_count++;
    }
    EXPECT_EQ(2, mp4_count) << "Relative split at 0.5 should produce 2 parts";

    fs::remove_all(out);
}

// ---------- Same input and output directory ----------
// Output dir is set to the same folder that contains the input file.
// The split parts will appear alongside the original. This is fine
// because the part filenames include a unique timestamp suffix.

TEST(SplitAppTest, SameInputOutputDir) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // Copy input into a temp dir, then split with output in the same dir
    fs::path same_dir = OUTPUT_DIR / "test_split_same_dir";
    fs::remove_all(same_dir);
    fs::create_directories(same_dir);

    fs::path input_copy = same_dir / TEST_VIDEO.filename();
    fs::copy_file(TEST_VIDEO, input_copy, fs::copy_options::overwrite_existing);

    int rc = run_split({
        "split",
        "-i", input_copy.string(),
        "-o", same_dir.string(),
        "-p", "8"
    });

    EXPECT_EQ(0, rc);

    // Should have the original + 2 split parts
    int mp4_count = 0;
    for (const auto& entry : fs::directory_iterator(same_dir)) {
        if (entry.path().extension() == ".mp4") mp4_count++;
    }
    EXPECT_EQ(3, mp4_count) << "Expected 1 original + 2 split parts";

    // Original should still be intact
    EXPECT_TRUE(fs::exists(input_copy));

    fs::remove_all(same_dir);
}
