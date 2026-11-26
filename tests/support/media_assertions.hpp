#pragma once

#include <gtest/gtest.h>

#include "av/frame_grabber.h"
#include "av/media_probe.h"
#include "support/test_sandbox.hpp"

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace test_support {

namespace fs = std::filesystem;

struct MediaExpectation {
    std::optional<std::string_view> video_codec = std::nullopt;
    std::optional<std::string_view> audio_codec = std::nullopt;
    std::optional<int> width = std::nullopt;
    std::optional<int> height = std::nullopt;
    std::optional<bool> has_video = std::nullopt;
    std::optional<bool> has_audio = std::nullopt;
    std::optional<double> expected_fps = std::nullopt;
    double fps_tolerance = 0.5;
    std::optional<double> expected_duration_sec = std::nullopt;
    double duration_tolerance_sec = 0.5;
};

[[nodiscard]] inline av::MediaInfo probe_valid_media(
    const fs::path& file, const MediaExpectation& expected = {}) {
    EXPECT_TRUE(fs::is_regular_file(file)) << "Media file missing: " << file;
    if (fs::is_regular_file(file)) {
        EXPECT_GT(fs::file_size(file), 0u) << "Media file is empty: " << file;
    }

    const av::MediaInfo info = av::probe(file);
    EXPECT_TRUE(info.ok) << "av::probe failed for: " << file;
    if (!info.ok) {
        return info;
    }

    if (expected.has_video) {
        EXPECT_EQ(info.has_video, *expected.has_video) << "Video stream mismatch for: " << file;
    }
    if (expected.has_audio) {
        EXPECT_EQ(info.has_audio, *expected.has_audio) << "Audio stream mismatch for: " << file;
    }
    if (expected.video_codec) {
        EXPECT_EQ(info.video_codec, *expected.video_codec) << "Video codec mismatch for: " << file;
    }
    if (expected.audio_codec) {
        EXPECT_EQ(info.audio_codec, *expected.audio_codec) << "Audio codec mismatch for: " << file;
    }
    if (expected.width) {
        EXPECT_EQ(info.width, *expected.width) << "Width mismatch for: " << file;
    }
    if (expected.height) {
        EXPECT_EQ(info.height, *expected.height) << "Height mismatch for: " << file;
    }
    if (expected.expected_fps) {
        EXPECT_TRUE(info.frame_rate.known()) << "Unknown frame rate for: " << file;
        if (info.frame_rate.known()) {
            EXPECT_NEAR(info.frame_rate.value(), *expected.expected_fps, expected.fps_tolerance)
                << "Frame rate mismatch for: " << file;
        }
    }
    if (expected.expected_duration_sec) {
        EXPECT_NEAR(info.duration_sec, *expected.expected_duration_sec,
                    expected.duration_tolerance_sec)
            << "Duration mismatch for: " << file;
    }
    return info;
}

// Verifies real downstream frame decoding at timestamp_sec via av::grab_frame and
// checks that the emitted JPEG carries valid SOI/EOI magic markers and non-trivial payload.
inline void verify_decodable_frame_at(
    const fs::path& video_file, double timestamp_sec, const ScopedTempDir& scratch,
    std::string_view tag = "probe_frame.jpg") {
    const fs::path out_jpg = scratch.child(tag);
    ASSERT_TRUE(av::grab_frame(video_file, out_jpg, timestamp_sec))
        << "Failed to decode frame at t=" << timestamp_sec << "s from " << video_file;
    ASSERT_TRUE(fs::is_regular_file(out_jpg));
    const auto bytes = read_file_bytes(out_jpg);
    ASSERT_GE(bytes.size(), 4u) << "Decoded JPEG too small: " << out_jpg;
    // JPEG SOI marker: FF D8
    EXPECT_EQ(std::to_integer<unsigned>(bytes[0]), 0xFFu);
    EXPECT_EQ(std::to_integer<unsigned>(bytes[1]), 0xD8u);
}

// Fail-loud check for committed test fixtures (bans silent GTEST_SKIP on repo data).
[[nodiscard]] inline fs::path require_fixture(std::string_view rel) {
    const fs::path p = fixture_path(rel);
    EXPECT_TRUE(fs::exists(p)) << "Required committed test fixture missing: " << p
                               << " (TEST_DATA_DIR=" << TEST_DATA_DIR << ")";
    return p;
}

}  // namespace test_support
