#pragma once

// Ffmpeg – domain Facade forwarding media operations into the in-process av/
// layer without exposing libav* types.

#include <filesystem>
#include <initializer_list>
#include <span>

namespace fs = std::filesystem;

class ProgressReporter;

class Ffmpeg {
public:
    // Probe video duration in seconds. Returns -1.0 on failure.
    [[nodiscard]] double duration(const fs::path& input) const;

    // Probe the average frame rate of the first video stream. Returns -1.0 on
    // failure (no video stream, or the file could not be opened).
    [[nodiscard]] double frame_rate(const fs::path& input) const;

    // Decode the frame at the given timestamp (seconds) and write it as an
    // image. Returns true on success.
    [[nodiscard]] bool extract_frame(const fs::path& in, const fs::path& out_img, double ts) const;

    // Copy [start_sec, end_sec) into `out` without re-encoding. If end_sec < 0,
    // copies through to the end of the file. Returns true on success.
    [[nodiscard]] bool run_segment(const fs::path& in, const fs::path& out,
                                   double start_sec, double end_sec) const;

    // Copy `inputs` into `out` back to back without re-encoding. If total_sec is
    // > 0 the reporter draws a progress bar. Returns true on success.
    [[nodiscard]] bool concat(std::span<const fs::path> inputs, const fs::path& out,
                              double total_sec, ProgressReporter& rep) const;

    [[nodiscard]] bool concat(std::initializer_list<fs::path> inputs, const fs::path& out,
                              double total_sec, ProgressReporter& rep) const {
        return concat(std::span<const fs::path>{inputs.begin(), inputs.size()},
                      out, total_sec, rep);
    }
};
