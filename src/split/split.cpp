#include "split.h"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

std::string SplitApp::name() const { return "Split Video"; }

void SplitApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",  po::value<std::string>()->required(),                      "Input video file (relative or absolute path)")
        ("points,p", po::value<std::vector<double>>()->multitoken()->required(), "Split points in seconds (space-separated)")
        ("relative,r", "Treat split points as relative to the previous split (default: absolute from start)")
        ("output,o", po::value<std::string>()->default_value(""),               "Output directory (default: same as input file)")
    ;
}

std::string SplitApp::extra_help() const {
    return "\nSplits a video at the given time points using stream copy (no re-encoding).\n"
           "\nAbsolute mode (default):\n"
           "  -p 10 30 60   splits at 10s, 30s, 60s from the start\n"
           "\nRelative mode (-r):\n"
           "  -p 10 20 30   splits at 10s, 30s (10+20), 60s (30+30)";
}

int SplitApp::execute(const po::variables_map& vm) {
    fs::path input_file  = fs::absolute(vm["input"].as<std::string>());
    auto raw_points      = vm["points"].as<std::vector<double>>();
    bool relative        = vm.count("relative") > 0;

    if (!fs::exists(input_file) || !fs::is_regular_file(input_file)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input file does not exist: " << input_file;
        return 1;
    }

    // Convert to absolute split points
    auto points = compute_absolute_points(raw_points, relative);

    // Validate
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i] <= 0) {
            BOOST_LOG_TRIVIAL(error) << "Error: Split points must be positive (got " << points[i] << " s).";
            return 1;
        }
        if (i > 0 && points[i] <= points[i - 1]) {
            BOOST_LOG_TRIVIAL(error) << "Error: Split points must be strictly increasing ("
                      << points[i - 1] << " >= " << points[i] << ").";
            return 1;
        }
    }

    Ffmpeg runner;

    BOOST_LOG_TRIVIAL(info) << "Probing video duration...";
    double duration = runner.duration(input_file);
    if (duration < 0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine video duration.";
        return 1;
    }

    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(2) << "Video duration: " << duration << " s";

    // Remove points beyond duration
    while (!points.empty() && points.back() >= duration) {
        BOOST_LOG_TRIVIAL(warning) << "Warning: Split point " << points.back() << " s is beyond duration. Ignoring.";
        points.pop_back();
    }

    if (points.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Error: No valid split points within video duration.";
        return 1;
    }

    size_t num_parts = points.size() + 1;
    {
        std::ostringstream msg;
        msg << "Splitting into " << num_parts << " parts at:";
        for (double p : points) msg << " " << p << "s";
        BOOST_LOG_TRIVIAL(info) << msg.str();
    }

    // Determine output directory
    fs::path out_dir;
    std::string out_str = vm["output"].as<std::string>();
    if (out_str.empty()) {
        out_dir = input_file.parent_path();
    } else {
        out_dir = fs::absolute(out_str);
        if (!fs::exists(out_dir)) fs::create_directories(out_dir);
    }

    std::string stem = input_file.stem().string();
    std::string ext  = input_file.extension().string();
    std::string ts   = media::current_timestamp();

    // Extract each segment
    double seg_start = 0.0;
    bool all_ok = true;

    for (size_t i = 0; i <= points.size(); ++i) {
        double seg_end = (i < points.size()) ? points[i] : -1.0;

        std::string part_name = stem + "_part" + std::to_string(i + 1)
                              + "_" + ts + ext;
        fs::path part_path = out_dir / part_name;

        {
            std::ostringstream msg;
            msg << "Part " << (i + 1) << "/" << num_parts << ": [" << seg_start;
            if (seg_end >= 0)
                msg << ", " << seg_end;
            else
                msg << ", " << duration;
            msg << "] s -> " << part_path.filename();
            BOOST_LOG_TRIVIAL(info) << msg.str();
        }

        if (!runner.run_segment(input_file, part_path, seg_start, seg_end)) {
            BOOST_LOG_TRIVIAL(error) << "Error: Failed to extract part " << (i + 1) << ".";
            all_ok = false;
            break;
        }

        auto size = fs::file_size(part_path);
        double size_mb = static_cast<double>(size) / (1024.0 * 1024.0);
        BOOST_LOG_TRIVIAL(info) << "  -> " << size_mb << " MB";

        seg_start = (seg_end >= 0) ? seg_end : duration;
    }

    if (all_ok) {
        BOOST_LOG_TRIVIAL(info) << "Success! " << num_parts << " parts saved to: " << out_dir;
    } else {
        BOOST_LOG_TRIVIAL(error) << "Error: Split failed.";
    }

    return all_ok ? 0 : 1;
}

std::vector<double> SplitApp::compute_absolute_points(const std::vector<double>& points, bool relative) {
    if (!relative) return points;

    std::vector<double> absolute;
    double cumulative = 0.0;
    for (double p : points) {
        cumulative += p;
        absolute.push_back(cumulative);
    }
    return absolute;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    SplitApp app;
    return app.run(argc, argv);
}
#endif
