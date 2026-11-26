#pragma once

// Stream-copy container operations (segment extraction and concatenation).
// Packets are remuxed without re-encoding; cutting begins on the keyframe at
// or before the requested start timestamp.

#include <filesystem>
#include <initializer_list>
#include <span>

namespace fs = std::filesystem;

class ProgressReporter;

namespace av {

// Copies [start_sec, end_sec) of `in` into `out` via stream copy.
// end_sec < 0 copies through the end of the file. Returns true on success.
[[nodiscard]] bool copy_segment(const fs::path& in,
                                const fs::path& out,
                                double start_sec,
                                double end_sec);

// Concatenates `inputs` into `out` via stream copy, shifting timestamps across
// segment boundaries. Every input must share compatible codecs and parameters.
// Reports progress against `total_sec` when > 0. Returns true on success.
[[nodiscard]] bool concat_copy(std::span<const fs::path> inputs,
                               const fs::path& out,
                               ProgressReporter& reporter,
                               double total_sec);

[[nodiscard]] inline bool concat_copy(std::initializer_list<fs::path> inputs,
                                      const fs::path& out,
                                      ProgressReporter& reporter,
                                      double total_sec) {
    return concat_copy(std::span<const fs::path>{inputs.begin(), inputs.size()},
                       out, reporter, total_sec);
}

}  // namespace av
