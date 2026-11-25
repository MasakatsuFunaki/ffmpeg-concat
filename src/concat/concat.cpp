#include "concat.h"

#include "ffmpeg/progress_reporter.h"

#include <iostream>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

std::string ConcatApp::name() const { return "Concat Videos"; }

void ConcatApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",  po::value<std::string>()->required(),  "Input directory containing MP4 files")
        ("output,o", po::value<std::string>()->required(), "Output directory for concatenated video")
    ;
}

int ConcatApp::execute(const po::variables_map& vm) {
    fs::path input_dir  = fs::absolute(vm["input"].as<std::string>());
    fs::path output_dir = fs::absolute(vm["output"].as<std::string>());

    if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input directory does not exist: " << input_dir;
        return 1;
    }

    fs::create_directories(output_dir);

    auto mp4_files = media::collect_mp4_files(input_dir);

    if (mp4_files.size() < 2) {
        BOOST_LOG_TRIVIAL(info) << "Found " << mp4_files.size()
                  << " MP4 file(s). Need at least 2 files to concatenate.";
        return 1;
    }

    BOOST_LOG_TRIVIAL(info) << "Found " << mp4_files.size() << " MP4 files to concatenate:";
    for (const auto& f : mp4_files) {
        BOOST_LOG_TRIVIAL(info) << "  - " << f.filename().string();
    }

    fs::path output_file = output_dir / ("concatenated_" + media::current_timestamp() + ".mp4");

    Ffmpeg runner;
    double total_duration = 0;
    for (const auto& f : mp4_files) {
        double d = runner.duration(f);
        if (d > 0) total_duration += d;
    }

    BOOST_LOG_TRIVIAL(info) << "Concatenating to: " << output_file;

    IndicatorsProgressReporter rep;
    bool ok = runner.concat(mp4_files, output_file, total_duration, rep);

    if (ok) {
        media::log_output_size(output_file);
    }

    return ok ? 0 : 1;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    ConcatApp app;
    return app.run(argc, argv);
}
#endif
