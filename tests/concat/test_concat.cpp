#include <gtest/gtest.h>
#include "concat/concat.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static const fs::path DATA_DIR      = TEST_DATA_DIR;
static const fs::path OUTPUT_DIR    = TEST_OUTPUT_DIR;
static const fs::path TEST_VIDEO    = DATA_DIR / "2026-03-22 00-16-33.mp4";

// Helper to call ConcatApp::run with a vector of string args
static int run_concat(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    ConcatApp app;
    return app.run(static_cast<int>(argv.size()), argv.data());
}

// ---------- Help ----------

TEST(ConcatAppTest, HelpReturns0) {
    EXPECT_EQ(0, run_concat({"concat_videos", "--help"}));
}

// ---------- Missing input ----------

TEST(ConcatAppTest, NoArgsShowsHelp) {
    EXPECT_EQ(0, run_concat({"concat_videos"}));
}

// ---------- Nonexistent input dir ----------

TEST(ConcatAppTest, NonexistentInputDirReturns1) {
    int rc = run_concat({
        "concat_videos",
        "-i", (OUTPUT_DIR / "nonexistent_dir_12345").string(),
        "-o", (OUTPUT_DIR / "tmp_out").string()
    });
    EXPECT_EQ(1, rc);
}

// ---------- Same input and output directory ----------

TEST(ConcatAppTest, SameInputOutputDir) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // Prepare a temp directory used as BOTH input and output
    fs::path same_dir = OUTPUT_DIR / "test_concat_same_dir";
    fs::remove_all(same_dir);
    fs::create_directories(same_dir);

    fs::copy_file(TEST_VIDEO, same_dir / "2026-03-22 00-16-33.mp4", fs::copy_options::overwrite_existing);
    fs::copy_file(TEST_VIDEO, same_dir / "2026-03-22 00-16-34.mp4", fs::copy_options::overwrite_existing);

    int rc = run_concat({
        "concat_videos",
        "-i", same_dir.string(),
        "-o", same_dir.string()
    });

    EXPECT_EQ(0, rc);

    // The concatenated file should appear alongside the originals
    int mp4_count = 0;
    bool found_concat = false;
    for (const auto& entry : fs::directory_iterator(same_dir)) {
        if (entry.path().extension() == ".mp4") {
            mp4_count++;
            if (entry.path().filename().string().rfind("concatenated_", 0) == 0) {
                found_concat = true;
                EXPECT_GT(fs::file_size(entry.path()), 0u);
            }
        }
    }
    EXPECT_TRUE(found_concat) << "Expected a concatenated_*.mp4 in " << same_dir;
    EXPECT_EQ(3, mp4_count) << "Expected 2 originals + 1 concatenated output";

    fs::remove_all(same_dir);
}

// ---------- Actual concat with single file ----------

TEST(ConcatAppTest, ConcatTwoFiles) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // Create temp input/output directories inside the build test output folder
    fs::path tmp_in  = OUTPUT_DIR / "test_concat_in";
    fs::path tmp_out = OUTPUT_DIR / "test_concat_out";
    fs::remove_all(tmp_in);
    fs::remove_all(tmp_out);
    fs::create_directories(tmp_in);
    fs::create_directories(tmp_out);

    fs::copy_file(TEST_VIDEO, tmp_in / "2026-03-22 00-16-33.mp4", fs::copy_options::overwrite_existing);
    fs::copy_file(TEST_VIDEO, tmp_in / "2026-03-22 00-16-34.mp4", fs::copy_options::overwrite_existing);

    int rc = run_concat({
        "concat_videos",
        "-i", tmp_in.string(),
        "-o", tmp_out.string()
    });

    EXPECT_EQ(0, rc);

    // Check that an output file was created
    bool found_output = false;
    for (const auto& entry : fs::directory_iterator(tmp_out)) {
        if (entry.path().extension() == ".mp4") {
            found_output = true;
            EXPECT_GT(fs::file_size(entry.path()), 0u);
        }
    }
    EXPECT_TRUE(found_output) << "Expected an output mp4 in " << tmp_out;

    // Cleanup
    fs::remove_all(tmp_in);
    fs::remove_all(tmp_out);
}
