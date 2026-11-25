#include <gtest/gtest.h>
#include "common.h"
#include "av/av_capabilities.h"
#include "av/media_probe.h"

#include <regex>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static const fs::path PROJECT_ROOT  = PROJECT_SOURCE_DIR;
static const fs::path DATA_DIR      = TEST_DATA_DIR;
static const fs::path OUTPUT_DIR    = TEST_OUTPUT_DIR;
static const fs::path TEST_VIDEO    = DATA_DIR / "2026-03-22 00-16-33.mp4";

// ---------- current_timestamp ----------

TEST(FFmpegUtilsTest, CurrentTimestampFormat) {
    std::string ts = FFmpegUtils::current_timestamp();
    // Expected: "YYYY-MM-DD_HH-MM-SS"
    std::regex pattern(R"(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})");
    EXPECT_TRUE(std::regex_match(ts, pattern))
        << "Timestamp '" << ts << "' does not match YYYY-MM-DD_HH-MM-SS";
}

TEST(FFmpegUtilsTest, CurrentTimestampNotEmpty) {
    EXPECT_FALSE(FFmpegUtils::current_timestamp().empty());
}

// ---------- get_file_date ----------

TEST(FFmpegUtilsTest, GetFileDateParsesFilename) {
    // The test video filename encodes the date "2026-03-22 00-16-33"
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    std::time_t t = FFmpegUtils::get_file_date(TEST_VIDEO);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    EXPECT_EQ(tm.tm_year + 1900, 2026);
    EXPECT_EQ(tm.tm_mon + 1, 3);
    EXPECT_EQ(tm.tm_mday, 22);
}

TEST(FFmpegUtilsTest, GetFileDateFallback) {
    // A file whose name doesn't match the date pattern should still return a time_t
    // Use any existing file that doesn't match "YYYY-MM-DD HH-MM-SS"
    fs::path cmake = PROJECT_ROOT / "CMakeLists.txt";
    if (!fs::exists(cmake)) GTEST_SKIP() << "CMakeLists.txt not found";

    std::time_t t = FFmpegUtils::get_file_date(cmake);
    EXPECT_GT(t, 0);
}

// ---------- No ffmpeg executable is needed or shipped ----------

TEST(NoSubprocessTest, ShipsNoFfmpegBinary) {
    // The codecs are linked into these executables. If a bin/ffmpeg reappears,
    // something has started shipping a subprocess again.
    EXPECT_FALSE(fs::exists(PROJECT_ROOT / "bin" / "ffmpeg.exe"));
    EXPECT_FALSE(fs::exists(PROJECT_ROOT / "bin" / "ffmpeg"));
}

// ---------- collect_mp4_files ----------

TEST(FFmpegUtilsTest, CollectMp4Files) {
    if (!fs::exists(DATA_DIR)) GTEST_SKIP() << "test data directory not found";

    auto files = FFmpegUtils::collect_mp4_files(DATA_DIR);
    EXPECT_GE(files.size(), 1u) << "Expected at least 1 mp4 file in test data dir";
    for (const auto& f : files) {
        EXPECT_EQ(f.extension(), ".mp4");
    }
}

TEST(FFmpegUtilsTest, CollectMp4FilesEmptyDir) {
    fs::path empty_dir = OUTPUT_DIR / "test_empty_mp4_dir";
    fs::create_directories(empty_dir);
    auto files = FFmpegUtils::collect_mp4_files(empty_dir);
    EXPECT_TRUE(files.empty());
    fs::remove(empty_dir);
}

// ---------- get_video_duration ----------

TEST(FFmpegUtilsTest, GetVideoDuration) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    double dur = FFmpegUtils::get_video_duration(TEST_VIDEO);
    EXPECT_GT(dur, 0.0) << "Duration should be positive";
    // Test video is ~16.10s
    EXPECT_NEAR(dur, 16.1, 1.0) << "Expected ~16.1 seconds";
}

TEST(FFmpegUtilsTest, GetVideoDurationBadFile) {

    double dur = FFmpegUtils::get_video_duration("nonexistent.mp4");
    EXPECT_LT(dur, 0.0) << "Should return -1 for a missing file";
}

// ---------- run_ffmpeg_segment ----------

TEST(FFmpegUtilsTest, RunFfmpegSegment) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    fs::path out = OUTPUT_DIR / "test_segment_output.mp4";
    fs::remove(out); // clean up from previous runs

    bool ok = FFmpegUtils::run_ffmpeg_segment(TEST_VIDEO, out, 1.0, 5.0);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);

    // Verify duration of segment is roughly 4 seconds
    double dur = FFmpegUtils::get_video_duration(out);
    EXPECT_NEAR(dur, 4.0, 1.0);

    fs::remove(out);
}

// ---------- av::probe (in-process, no ffmpeg binary) ----------

TEST(AvProbeTest, ReadsRealCapture) {
    if (!fs::exists(TEST_VIDEO)) GTEST_SKIP() << "Test video not found";

    // A 16 s, 1080p30 HEVC OBS capture with AAC audio.
    av::MediaInfo info = av::probe(TEST_VIDEO);

    EXPECT_TRUE(info.ok);
    EXPECT_TRUE(info.frame_rate.known());
    EXPECT_NEAR(30.0, info.frame_rate.value(), 0.01);
    EXPECT_NEAR(16.1, info.duration_sec, 0.5);
    EXPECT_EQ(1920, info.width);
    EXPECT_EQ(1080, info.height);
    EXPECT_TRUE(info.has_video);
    EXPECT_TRUE(info.has_audio);
    EXPECT_EQ("hevc", info.video_codec);
    EXPECT_EQ("aac", info.audio_codec);
}

TEST(AvProbeTest, ReportsFailureForMissingFile) {
    av::MediaInfo info = av::probe(OUTPUT_DIR / "nonexistent_98765.mp4");

    EXPECT_FALSE(info.ok);
    EXPECT_FALSE(info.frame_rate.known());
    EXPECT_LT(info.duration_sec, 0.0);
}

// ---------- av::can_use_encoder / can_use_hwaccel ----------

TEST(AvCapabilitiesTest, RejectsUnknownEncoder) {
    EXPECT_FALSE(av::encoder_exists("definitely_not_an_encoder"));
    EXPECT_FALSE(av::can_use_encoder("definitely_not_an_encoder"));
}

TEST(AvCapabilitiesTest, FindsTheSoftwareEncodersThisBuildLinks) {
    // libx264 is compiled in and needs no driver, so it must open anywhere.
    EXPECT_TRUE(av::encoder_exists("libx264"));
    EXPECT_TRUE(av::can_use_encoder("libx264"));
    EXPECT_TRUE(av::encoder_exists("aac"));
}

TEST(AvCapabilitiesTest, NvencIsLinkedInEvenWhereItCannotRun) {
    // The distinction the catalog relies on: present in the build is not the
    // same as usable on this host.
    EXPECT_TRUE(av::encoder_exists("h264_nvenc"));
    EXPECT_NO_THROW((void)av::can_use_encoder("h264_nvenc"));
}

TEST(AvCapabilitiesTest, RejectsUnknownHwaccel) {
    // Guards against a probe that answers "yes" to everything.
    EXPECT_FALSE(av::can_use_hwaccel("definitely_not_a_hwaccel"));
}

TEST(AvCapabilitiesTest, HwaccelProbeIsStable) {
    EXPECT_EQ(av::can_use_hwaccel("cuda"), av::can_use_hwaccel("cuda"));
}
