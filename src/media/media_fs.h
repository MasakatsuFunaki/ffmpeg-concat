#pragma once

// media – filesystem-level media helpers: dating files, timestamping outputs,
// collecting sorted video inputs, and logging output sizes.

#include <array>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace media {

inline constexpr std::array<std::string_view, 9> kVideoExtensions{
    ".mp4", ".avi", ".mkv", ".mov", ".wmv", ".flv", ".webm", ".ts", ".m4v"
};

// Extract date from filename ("YYYY-MM-DD HH-MM-SS") or fall back to last write time.
[[nodiscard]] std::time_t file_date(const fs::path& filepath);

// Generate a local timestamp string for output filenames ("YYYY-MM-DD_HH-MM-SS").
[[nodiscard]] std::string current_timestamp();

// Collect video files matching kVideoExtensions (case-insensitively), sorted
// ascending by file_date with deterministic path tie-breaking. Returns empty
// if input_dir does not exist or is not a directory.
[[nodiscard]] std::vector<fs::path> collect_video_files(const fs::path& input_dir);

// Collect .mp4 files (case-insensitively), sorted ascending by file_date with
// deterministic path tie-breaking. Returns empty if input_dir is missing/non-dir.
[[nodiscard]] std::vector<fs::path> collect_mp4_files(const fs::path& input_dir);

// Log the standard success and output-size lines for an output artifact.
void log_output_size(const fs::path& output_file);

}  // namespace media
