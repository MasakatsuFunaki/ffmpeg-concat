#pragma once

// ---------------------------------------------------------------------------
// media_probe – read a container's properties in-process.
//
// Replaces the old "spawn `ffmpeg -i` and scrape stderr" probe: libavformat
// hands back the same numbers as structured data, so there is nothing to parse
// and no locale to defend against.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace av {

// An exact frame rate, kept as the rational libav reports rather than a double.
// Decimals are both lossy (30000/1001 does not round-trip) and unsafe to put in
// a filter string: libavfilter parses numbers through the C locale, which any
// library in the process can switch to comma-decimal.
struct FrameRate {
    int num = 0;
    int den = 0;

    bool known() const { return num > 0 && den > 0; }
    double value() const { return known() ? static_cast<double>(num) / den : -1.0; }
};

struct MediaInfo {
    bool ok = false;              // false => the file could not be opened/parsed
    double duration_sec = -1.0;   // -1.0 when the container declares no duration
    FrameRate frame_rate;         // average rate of the best video stream
    int width = 0;
    int height = 0;
    bool has_video = false;
    bool has_audio = false;
    std::string video_codec;      // e.g. "h264", "av1"
    std::string audio_codec;
};

// Open `file` and report what it contains. Never throws; a failure to open
// returns a MediaInfo with ok == false and the numeric fields left negative.
MediaInfo probe(const fs::path& file);

}  // namespace av
