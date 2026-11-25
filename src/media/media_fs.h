#pragma once

// ---------------------------------------------------------------------------
// media – free functions (deliberately NOT a pattern) for the filesystem-level
// media chores: dating files, timestamping outputs, gathering video files,
// building an ffmpeg concat list, and reporting an output's size.
// ---------------------------------------------------------------------------

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace media {

// Extract date from filename (format: "YYYY-MM-DD HH-MM-SS") or fall back to last write time.
std::time_t file_date(const fs::path& filepath);

// Generate a timestamp string for output filenames (format: "YYYY-MM-DD_HH-MM-SS").
std::string current_timestamp();

// Collect all video files (mp4, avi, mkv, mov, wmv, flv, webm, ts, m4v) sorted by date.
std::vector<fs::path> collect_video_files(const fs::path& input_dir);

// Collect .mp4 files from a directory, sorted by date extracted from filename.
std::vector<fs::path> collect_mp4_files(const fs::path& input_dir);

// Create the FFmpeg concat demuxer list file. Returns path to the list file.

// Log the standard success + output-size lines. Used by concat/cut/transcode.
void log_output_size(const fs::path& output_file);

}  // namespace media
