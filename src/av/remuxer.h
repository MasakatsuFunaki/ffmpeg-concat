#pragma once

// ---------------------------------------------------------------------------
// av::remuxer – container surgery without re-encoding.
//
// Both operations copy compressed packets straight from input to output, so the
// video is bit-identical and the cost is I/O rather than CPU. Cutting can only
// start on a keyframe, so a segment's real start may precede the requested one.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

class ProgressReporter;

namespace av {

// Copy [start_sec, end_sec) of `in` into `out`. end_sec < 0 means "to the end".
// Returns true on success.
bool copy_segment(const fs::path& in, const fs::path& out, double start_sec, double end_sec);

// Copy `inputs` into `out` back to back, shifting each file's timestamps to
// continue from the previous one. Every input must carry the same codecs and
// parameters, which is what stream copying requires. Progress is reported
// against total_sec when it is > 0. Returns true on success.
bool concat_copy(const std::vector<fs::path>& inputs, const fs::path& out,
                 ProgressReporter& reporter, double total_sec);

}  // namespace av
