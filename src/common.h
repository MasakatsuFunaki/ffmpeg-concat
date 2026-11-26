#pragma once

#include "ffmpeg/ffmpeg.h"
#include "media/media_fs.h"

#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>

#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Template Method base class for all CLI tools.
// run() parses UTF-8 CLI flags, serves --help / no-arg invocation, and
// delegates to execute(). run_cli() acquires UTF-8 process arguments on
// Windows (GetCommandLineW / CommandLineToArgvW) before forwarding to run().
// Subclasses override name(), define_options(), execute(), and optionally
// extra_help().
class ToolApp {
public:
    virtual ~ToolApp();

    ToolApp(const ToolApp&) = delete;
    ToolApp& operator=(const ToolApp&) = delete;
    ToolApp(ToolApp&&) = delete;
    ToolApp& operator=(ToolApp&&) = delete;

    [[nodiscard]] int run(int argc, char* argv[]);
    [[nodiscard]] int run_cli(int argc, char* argv[]);

protected:
    ToolApp() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    virtual void define_options(boost::program_options::options_description& desc) = 0;
    [[nodiscard]] virtual std::string extra_help() const;
    [[nodiscard]] virtual int execute(const boost::program_options::variables_map& vm) = 0;
};

// FFmpegUtils – Facade forwarding into the decomposed ffmpeg/ and media/ layers.
class FFmpegUtils {
public:
    // Extract date from filename ("YYYY-MM-DD HH-MM-SS") or fall back to last write time.
    [[nodiscard]] static std::time_t get_file_date(const fs::path& filepath);

    // Generate a timestamp string for output filenames ("YYYY-MM-DD_HH-MM-SS").
    [[nodiscard]] static std::string current_timestamp();

    // Collect .mp4 files from a directory, sorted ascending by date.
    [[nodiscard]] static std::vector<fs::path> collect_mp4_files(const fs::path& input_dir);

    // Collect all supported video files from a directory, sorted ascending by date.
    [[nodiscard]] static std::vector<fs::path> collect_video_files(const fs::path& input_dir);

    // Extract a single frame from a video at the given timestamp (seconds).
    [[nodiscard]] static bool run_ffmpeg_extract_frame(const fs::path& input_file,
                                                       const fs::path& output_image,
                                                       double timestamp);

    // Concatenate the given files by stream copy. Displays progress if total_duration_sec > 0.
    [[nodiscard]] static bool run_ffmpeg_concat(std::span<const fs::path> input_files,
                                                const fs::path& output_file,
                                                double total_duration_sec = 0.0);

    [[nodiscard]] static bool run_ffmpeg_concat(std::initializer_list<fs::path> input_files,
                                                const fs::path& output_file,
                                                double total_duration_sec = 0.0) {
        return run_ffmpeg_concat(std::span<const fs::path>{input_files.begin(), input_files.size()},
                                 output_file, total_duration_sec);
    }

    // Extract [start_sec, end_sec) using stream copy (end_sec < 0 copies to EOF).
    [[nodiscard]] static bool run_ffmpeg_segment(const fs::path& input_file,
                                                 const fs::path& output_file,
                                                 double start_sec,
                                                 double end_sec);

    // Probe video duration in seconds. Returns -1.0 on failure.
    [[nodiscard]] static double get_video_duration(const fs::path& input_file);

    // Format a double with 6 fixed decimal places in the classic C locale.
    [[nodiscard]] static std::string fmt_double(double v);
};
