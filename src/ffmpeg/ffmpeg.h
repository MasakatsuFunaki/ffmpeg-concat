#pragma once

// ---------------------------------------------------------------------------
// Ffmpeg – domain Facade (GoF "Facade" pattern): the single surface the tools
// use for media work. It forwards into the av/ layer, which links libavformat,
// libavcodec, libavfilter and libswscale directly into this executable.
//
// This class used to spawn an ffmpeg process per operation. The interface
// survived that change because it was already phrased in terms of the domain
// (durations, segments, frames) rather than command lines.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Forward-declared so this header stays free of the indicators (<windows.h>)
// dependency; the full Strategy definition lives in progress_reporter.h.
class ProgressReporter;

class Ffmpeg {
public:
    // Probe video duration in seconds. Returns -1.0 on failure.
    double duration(const fs::path& input) const;

    // Probe the average frame rate of the first video stream. Returns -1.0 on
    // failure (no video stream, or the file could not be opened).
    double frame_rate(const fs::path& input) const;

    // Decode the frame at the given timestamp (seconds) and write it as an
    // image. Returns true on success.
    bool extract_frame(const fs::path& in, const fs::path& out_img, double ts) const;

    // Copy [start_sec, end_sec) into `out` without re-encoding. If end_sec < 0,
    // copies through to the end of the file. Returns true on success.
    bool run_segment(const fs::path& in, const fs::path& out,
                     double start_sec, double end_sec) const;

    // Copy `inputs` into `out` back to back without re-encoding. If total_sec is
    // > 0 the reporter draws a progress bar. Returns true on success.
    bool concat(const std::vector<fs::path>& inputs, const fs::path& out,
                double total_sec, ProgressReporter& rep) const;
};
