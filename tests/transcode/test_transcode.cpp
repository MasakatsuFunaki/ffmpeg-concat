#include <gtest/gtest.h>
#include "transcode/transcode.h"
#include "av/media_probe.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static const fs::path DATA_DIR      = TEST_DATA_DIR;
static const fs::path OUTPUT_DIR    = TEST_OUTPUT_DIR;
static const fs::path TEST_VIDEO    = DATA_DIR / "2026-03-22 00-16-33.mp4";

// Committed 2-second clips with both a video and an audio stream, named so the
// FASTER one sorts first: the lowest rate has to win on merit, not by being the
// first segment. fps_mixed is 60 then 30; fps_uniform is 30 and 30.
static const fs::path MIXED_FPS_DIR   = DATA_DIR / "fps_mixed";
static const fs::path UNIFORM_FPS_DIR = DATA_DIR / "fps_uniform";

// Helper to call TranscodeApp::run with a vector of string args
static int run_transcode(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    TranscodeApp app;
    return app.run(static_cast<int>(argv.size()), argv.data());
}

// The single transcoded_*.mp4 the tool wrote into dir, or an empty path.
static fs::path find_transcoded(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".mp4" &&
            entry.path().filename().string().rfind("transcoded_", 0) == 0) {
            return entry.path();
        }
    }
    return {};
}

// ---------- Help ----------

TEST(TranscodeAppTest, HelpReturns0) {
    EXPECT_EQ(0, run_transcode({"transcode", "--help"}));
}

// ---------- Missing required args ----------

TEST(TranscodeAppTest, NoArgsShowsHelp) {
    EXPECT_EQ(0, run_transcode({"transcode"}));
}

// ---------- Nonexistent input file ----------

TEST(TranscodeAppTest, NonexistentInputReturns1) {
    int rc = run_transcode({
        "transcode",
        "-i", (OUTPUT_DIR / "nonexistent_video_12345").string(),
        "-o", (OUTPUT_DIR / "tmp_out_12345").string()
    });
    EXPECT_EQ(1, rc);
}

// ---------- Same input and output directory ----------

TEST(TranscodeAppTest, SameInputOutputDir) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // Prepare a temp directory used as BOTH input and output
    fs::path same_dir = OUTPUT_DIR / "test_transcode_same_dir";
    fs::remove_all(same_dir);
    fs::create_directories(same_dir);

    fs::copy_file(TEST_VIDEO, same_dir / "2026-03-22 00-16-33.mp4", fs::copy_options::overwrite_existing);

    // Force a software encoder + fast preset: this test exercises the
    // same-in/out-directory behavior, not encoder selection, and must not depend
    // on an NVIDIA GPU (auto-detect would pick av1_nvenc, which needs one).
    int rc = run_transcode({
        "transcode",
        "-i", same_dir.string(),
        "-o", same_dir.string(),
        "-e", "libx264",
        "-p", "ultrafast"
    });

    EXPECT_EQ(0, rc);

    // The transcoded file should appear alongside the original
    bool found_transcoded = false;
    for (const auto& entry : fs::directory_iterator(same_dir)) {
        if (entry.path().extension() == ".mp4" &&
            entry.path().filename().string().rfind("transcoded_", 0) == 0) {
            found_transcoded = true;
            EXPECT_GT(fs::file_size(entry.path()), 0u);
        }
    }
    EXPECT_TRUE(found_transcoded) << "Expected a transcoded_*.mp4 in " << same_dir;

    fs::remove_all(same_dir);
}

// ---------- Frame-rate normalization across joined segments ----------

TEST(TranscodeAppTest, MixedFrameRatesNormalizeToLowest) {
    if (!fs::exists(MIXED_FPS_DIR)) GTEST_SKIP() << "Mixed-fps fixtures not found";

    fs::path out_dir = OUTPUT_DIR / "test_transcode_mixed_fps";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    int rc = run_transcode({
        "transcode",
        "-i", MIXED_FPS_DIR.string(),
        "-o", out_dir.string(),
        "-e", "libx264",       // no GPU required for this behavior
        "-p", "ultrafast"
    });
    EXPECT_EQ(0, rc);

    fs::path produced = find_transcoded(out_dir);
    ASSERT_FALSE(produced.empty()) << "No transcoded_*.mp4 in " << out_dir;

    // Unnormalized, 120 frames @60 + 60 frames @30 over 4s averages ~45 fps.
    // Normalized to the lowest rate it is 60 + 60 frames over 4s, i.e. ~30.
    av::MediaInfo info = av::probe(produced);
    ASSERT_TRUE(info.ok);
    EXPECT_NEAR(30.0, info.frame_rate.value(), 1.0);
    EXPECT_NEAR(4.0, info.duration_sec, 0.4);

    fs::remove_all(out_dir);
}

TEST(TranscodeAppTest, UniformFrameRatesAreJoinedUnchanged) {
    if (!fs::exists(UNIFORM_FPS_DIR)) GTEST_SKIP() << "Uniform-fps fixtures not found";

    fs::path out_dir = OUTPUT_DIR / "test_transcode_uniform_fps";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    int rc = run_transcode({
        "transcode",
        "-i", UNIFORM_FPS_DIR.string(),
        "-o", out_dir.string(),
        "-e", "libx264",
        "-p", "ultrafast"
    });
    EXPECT_EQ(0, rc);

    fs::path produced = find_transcoded(out_dir);
    ASSERT_FALSE(produced.empty()) << "No transcoded_*.mp4 in " << out_dir;

    // Matching rates emit no fps filter at all; the join must still come out at 30.
    av::MediaInfo info = av::probe(produced);
    ASSERT_TRUE(info.ok);
    EXPECT_NEAR(30.0, info.frame_rate.value(), 1.0);
    EXPECT_NEAR(4.0, info.duration_sec, 0.4);

    fs::remove_all(out_dir);
}

// Note: Full AV1 transcode test is omitted because it is very slow.
// The encoder detection is a private method. Integration testing is done
// through the CLI with the --help and error-path tests above.
