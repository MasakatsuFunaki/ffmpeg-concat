#pragma once

#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <ctime>

// New core-layer headers (Phase 1 decomposition). Existing translation units
// that #include "common.h" keep getting the fs alias, Boost logging and
// program_options, plus the decomposed ffmpeg/ and media/ facades that the
// FFmpegUtils compatibility facade now forwards to.
//
// progress_reporter.h is deliberately NOT pulled in here: it includes indicators
// (<windows.h>), and leaking that through common.h would drag a Windows header
// into every translation unit that only wants the domain types.
#include "ffmpeg/ffmpeg.h"
#include "media/media_fs.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Template Method base class for all CLI tools (GoF "Template Method" pattern).
//
// The run() method defines the fixed algorithm skeleton:
//   1. Parse CLI (--help plus tool-specific options)
//   2. Handle --help
//   3. Delegate to execute() for tool-specific work
//
// Subclasses override three hooks:
//   name()           – tool title shown in help
//   define_options()  – add tool-specific CLI flags
//   execute()        – the actual tool logic
//   extra_help()     – (optional) additional help text after the option table
// ---------------------------------------------------------------------------
class ToolApp {
public:
    virtual ~ToolApp() = default;
    int run(int argc, char* argv[]);

protected:
    virtual std::string name() const = 0;
    virtual void define_options(boost::program_options::options_description& desc) = 0;
    virtual std::string extra_help() const { return ""; }
    virtual int execute(const boost::program_options::variables_map& vm) = 0;
};

// ---------------------------------------------------------------------------
// FFmpegUtils – compatibility Facade (GoF "Facade" pattern). Every static is a
// one-line forwarder into the decomposed av/, ffmpeg/ and media/ layers, so
// tools and tests keep calling FFmpegUtils::x(...) unchanged.
//
// The methods that used to take a path to the ffmpeg executable no longer do:
// the codecs are linked into this binary and everything runs in-process.
// ---------------------------------------------------------------------------
class FFmpegUtils {
public:
    // Extract date from filename (format: "YYYY-MM-DD HH-MM-SS") or fall back to last write time
    static std::time_t get_file_date(const fs::path& filepath);

    // Generate a timestamp string for output filenames (format: "YYYY-MM-DD_HH-MM-SS")
    static std::string current_timestamp();

    // Collect .mp4 files from a directory, sorted by date extracted from filename
    static std::vector<fs::path> collect_mp4_files(const fs::path& input_dir);

    // Collect all video files (mp4, avi, mkv, mov, wmv, flv, webm, ts, m4v) sorted by date
    static std::vector<fs::path> collect_video_files(const fs::path& input_dir);

    // Extract a single frame from a video at the given timestamp (seconds).
    // Returns true on success.
    static bool run_ffmpeg_extract_frame(const fs::path& input_file,
                                         const fs::path& output_image,
                                         double timestamp);

    // Concatenate the given files by stream copy. Returns true on success.
    // If total_duration_sec > 0, displays a progress bar.
    static bool run_ffmpeg_concat(const std::vector<fs::path>& input_files,
                                  const fs::path& output_file,
                                  double total_duration_sec = 0);

    // Extract a segment from a video file using stream copy.
    // If end_sec < 0, extracts from start_sec to end of file.
    // Returns true on success.
    static bool run_ffmpeg_segment(const fs::path& input_file,
                                   const fs::path& output_file,
                                   double start_sec,
                                   double end_sec);

    // Probe video duration in seconds. Returns -1.0 on failure.
    static double get_video_duration(const fs::path& input_file);

    // Format a double with a period decimal separator regardless of the current
    // C locale, which some libraries (indicators among them) switch to
    // comma-decimal. Anything handed to libavfilter must go through this.
    static std::string fmt_double(double v);
};
