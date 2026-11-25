#include <gtest/gtest.h>
#include "cut/cut.h"

#include <filesystem>

namespace fs = std::filesystem;

static const fs::path DATA_DIR      = TEST_DATA_DIR;
static const fs::path OUTPUT_DIR    = TEST_OUTPUT_DIR;
static const fs::path TEST_VIDEO    = DATA_DIR / "2026-03-22 00-16-33.mp4";

// Helper to call CutApp::run with a vector of string args
static int run_cut(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    CutApp app;
    return app.run(static_cast<int>(argv.size()), argv.data());
}

// ---------- Help ----------

TEST(CutAppTest, HelpReturns0) {
    EXPECT_EQ(0, run_cut({"cut", "--help"}));
}

// ---------- Missing required args ----------

TEST(CutAppTest, NoArgsShowsHelp) {
    EXPECT_EQ(0, run_cut({"cut"}));
}

// ---------- Nonexistent input file ----------

TEST(CutAppTest, NonexistentInputReturns1) {
    int rc = run_cut({
        "cut",
        "-i", (OUTPUT_DIR / "nonexistent_video_12345.mp4").string(),
        "-s", "1",
        "-e", "5"
    });
    EXPECT_EQ(1, rc);
}

// ---------- Basic cut (stream copy) ----------

TEST(CutAppTest, BasicCut) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_cut_output.mp4";
    fs::remove(out);

    int rc = run_cut({
        "cut",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-s", "2",
        "-e", "5"
    });

    EXPECT_EQ(0, rc);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);

    // Result should be shorter than original (~16s minus ~3s cut)
    double dur = FFmpegUtils::get_video_duration(out);
    EXPECT_GT(dur, 0.0);
    EXPECT_LT(dur, 16.0);

    fs::remove(out);
}

// ---------- Cut with blur ----------

TEST(CutAppTest, CutWithBlur) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_cut_blur_output.mp4";
    fs::remove(out);

    int rc = run_cut({
        "cut",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-s", "2",
        "-e", "5",
        "--blur"
    });

    EXPECT_EQ(0, rc);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);

    // With blur, duration should be similar to original (~16s)
    double dur = FFmpegUtils::get_video_duration(out);
    EXPECT_NEAR(dur, 16.1, 2.0);

    fs::remove(out);
}

// ---------- Multi-interval cut ----------

TEST(CutAppTest, MultiIntervalCut) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_cut_multi_output.mp4";
    fs::remove(out);

    int rc = run_cut({
        "cut",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-s", "1", "8",
        "-e", "3", "10"
    });

    EXPECT_EQ(0, rc);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);

    // Removed ~4s total, so ~12s remaining
    double dur = FFmpegUtils::get_video_duration(out);
    EXPECT_GT(dur, 0.0);
    EXPECT_LT(dur, 16.0);

    fs::remove(out);
}

// ---------- Cut with blur + sound-off ----------

TEST(CutAppTest, CutWithBlurSoundOff) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_cut_blur_soundoff.mp4";
    fs::remove(out);

    int rc = run_cut({
        "cut",
        "-i", TEST_VIDEO.string(),
        "-o", out.string(),
        "-s", "2",
        "-e", "5",
        "--blur",
        "--sound-off"
    });

    EXPECT_EQ(0, rc);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);

    fs::remove(out);
}

// ---------- Same input and output directory ----------
// When -o is empty the output goes next to the input file,
// effectively the same directory. Filenames differ so this should succeed.

TEST(CutAppTest, SameInputOutputDir) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // Copy input into a temp dir, then cut with output in the same dir
    fs::path same_dir = OUTPUT_DIR / "test_cut_same_dir";
    fs::remove_all(same_dir);
    fs::create_directories(same_dir);

    fs::path input_copy = same_dir / TEST_VIDEO.filename();
    fs::copy_file(TEST_VIDEO, input_copy, fs::copy_options::overwrite_existing);

    // Explicitly set output inside the same directory
    fs::path out = same_dir / "same_dir_cut_output.mp4";

    int rc = run_cut({
        "cut",
        "-i", input_copy.string(),
        "-o", out.string(),
        "-s", "2",
        "-e", "5"
    });

    EXPECT_EQ(0, rc);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);

    // Original should still be intact
    EXPECT_TRUE(fs::exists(input_copy));

    fs::remove_all(same_dir);
}
