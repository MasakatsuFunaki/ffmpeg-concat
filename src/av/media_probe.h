#pragma once

// In-process container probing via libavformat returning structured metadata
// without spawning an external process or parsing text output.

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace av {

// Exact frame rate preserved as a rational (e.g. 30000/1001) so filter strings
// stay lossless and immune to locale-dependent decimal formatting.
struct FrameRate {
    static constexpr double kUnknownValue = -1.0;

    int num = 0;
    int den = 0;

    [[nodiscard]] constexpr bool known() const noexcept { return num > 0 && den > 0; }
    [[nodiscard]] constexpr double value() const noexcept {
        return known() ? static_cast<double>(num) / den : kUnknownValue;
    }
    [[nodiscard]] constexpr bool operator==(const FrameRate&) const noexcept = default;
};

struct MediaInfo {
    bool ok = false;             // false when the file could not be opened/parsed
    double duration_sec = -1.0;  // -1.0 when the container declares no duration
    FrameRate frame_rate;        // average rate of the best video stream
    int width = 0;
    int height = 0;
    bool has_video = false;
    bool has_audio = false;
    std::string video_codec;     // e.g. "h264", "av1"
    std::string audio_codec;

    [[nodiscard]] bool operator==(const MediaInfo&) const = default;
};

// Opens `file` and reports its container and stream properties. Never throws;
// returns a MediaInfo with ok == false on failure.
[[nodiscard]] MediaInfo probe(const fs::path& file);

}  // namespace av
