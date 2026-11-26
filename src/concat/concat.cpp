#include "concat.h"

#include "core/scoped_handle.h"
#include "ffmpeg/progress_reporter.h"

#include <boost/program_options.hpp>

#include <cstddef>
#include <format>
#include <functional>
#include <numeric>
#include <string>
#include <system_error>

namespace po = boost::program_options;

namespace {
constexpr std::size_t kMinConcatFiles = 2;
}  // namespace

std::string ConcatApp::name() const { return "Concat Videos"; }

void ConcatApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",  po::value<std::string>()->required(), "Input directory containing MP4 files")
        ("output,o", po::value<std::string>()->required(), "Output directory for concatenated video")
    ;
}

int ConcatApp::execute(const po::variables_map& vm) {
    const fs::path input_dir  = fs::absolute(vm["input"].as<std::string>());
    const fs::path output_dir = fs::absolute(vm["output"].as<std::string>());

    if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input directory does not exist: " << input_dir;
        return 1;
    }

    const auto mp4_files = media::collect_mp4_files(input_dir);
    if (mp4_files.size() < kMinConcatFiles) {
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Found {} MP4 file(s). Need at least {} files to concatenate.",
            mp4_files.size(), kMinConcatFiles);
        return 1;
    }

    fs::create_directories(output_dir);

    BOOST_LOG_TRIVIAL(info) << std::format("Found {} MP4 files to concatenate:", mp4_files.size());
    for (const auto& file : mp4_files) {
        BOOST_LOG_TRIVIAL(info) << std::format("  - {}", file.filename().string());
    }

    const std::string timestamp = media::current_timestamp();
    fs::path output_file = output_dir / std::format("concatenated_{}.mp4", timestamp);
    for (std::size_t suffix = 1; fs::exists(output_file); ++suffix) {
        output_file = output_dir / std::format("concatenated_{}_{}.mp4", timestamp, suffix);
    }

    Ffmpeg runner;
    const double total_duration = std::transform_reduce(
        mp4_files.begin(), mp4_files.end(), 0.0, std::plus<>{},
        [&runner](const fs::path& file) {
            const double d = runner.duration(file);
            return d > 0.0 ? d : 0.0;
        });

    BOOST_LOG_TRIVIAL(info) << "Concatenating to: " << output_file;

    auto remove_partial_output = core::make_scope_guard([&output_file]() noexcept {
        std::error_code ec;
        fs::remove(output_file, ec);
    });

    IndicatorsProgressReporter rep;
    const bool ok = runner.concat(mp4_files, output_file, total_duration, rep);
    if (!ok) {
        return 1;
    }

    remove_partial_output.dismiss();
    media::log_output_size(output_file);
    return 0;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    ConcatApp app;
    return app.run(argc, argv);
}
#endif
