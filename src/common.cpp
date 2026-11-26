#include "common.h"

#include "core/format_util.h"
#include "ffmpeg/progress_reporter.h"

namespace po = boost::program_options;

ToolApp::~ToolApp() = default;

std::string ToolApp::extra_help() const {
    return {};
}

int ToolApp::run(int argc, char* argv[]) {
    if (argc <= 0 || argv == nullptr) {
        return 1;
    }

    po::options_description desc(name() + " - Options");
    desc.add_options()
        ("help,h", "Show help message")
    ;
    define_options(desc);

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (argc == 1 || vm.count("help") > 0) {
            BOOST_LOG_TRIVIAL(info) << desc;
            if (const std::string help = extra_help(); !help.empty()) {
                BOOST_LOG_TRIVIAL(info) << help;
            }
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        BOOST_LOG_TRIVIAL(error) << "Error: " << e.what() << "\n" << desc;
        return 1;
    }

    return execute(vm);
}

std::time_t FFmpegUtils::get_file_date(const fs::path& filepath) {
    return media::file_date(filepath);
}

std::string FFmpegUtils::current_timestamp() {
    return media::current_timestamp();
}

std::vector<fs::path> FFmpegUtils::collect_mp4_files(const fs::path& input_dir) {
    return media::collect_mp4_files(input_dir);
}

std::vector<fs::path> FFmpegUtils::collect_video_files(const fs::path& input_dir) {
    return media::collect_video_files(input_dir);
}

bool FFmpegUtils::run_ffmpeg_extract_frame(const fs::path& input_file,
                                           const fs::path& output_image,
                                           double timestamp) {
    return Ffmpeg().extract_frame(input_file, output_image, timestamp);
}

bool FFmpegUtils::run_ffmpeg_concat(std::span<const fs::path> input_files,
                                    const fs::path& output_file,
                                    double total_duration_sec) {
    IndicatorsProgressReporter reporter;
    return Ffmpeg().concat(input_files, output_file, total_duration_sec, reporter);
}

bool FFmpegUtils::run_ffmpeg_segment(const fs::path& input_file,
                                     const fs::path& output_file,
                                     double start_sec,
                                     double end_sec) {
    return Ffmpeg().run_segment(input_file, output_file, start_sec, end_sec);
}

double FFmpegUtils::get_video_duration(const fs::path& input_file) {
    return Ffmpeg().duration(input_file);
}

std::string FFmpegUtils::fmt_double(double v) {
    return core::format_fixed(v);
}
