#include "split.h"

#include "core/format_util.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"

#include <boost/program_options.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace po = boost::program_options;

namespace {
constexpr double kBytesPerMiB = 1024.0 * 1024.0;
}  // namespace

std::string SplitApp::name() const { return "Split Video"; }

void SplitApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",    po::value<std::string>()->required(),                      "Input video file (relative or absolute path)")
        ("points,p",   po::value<std::vector<double>>()->multitoken()->required(), "Split points in seconds (space-separated)")
        ("relative,r", "Treat split points as relative to the previous split (default: absolute from start)")
        ("output,o",   po::value<std::string>()->default_value(""),               "Output directory (default: same as input file)")
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
    const fs::path input_file = fs::absolute(core::from_utf8(vm["input"].as<std::string>()));
    const auto raw_points     = vm["points"].as<std::vector<double>>();
    const bool relative       = vm.count("relative") > 0;
    const std::string out_str = vm["output"].as<std::string>();

    if (!fs::exists(input_file) || !fs::is_regular_file(input_file)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input file does not exist: " << core::to_utf8(input_file);
        return 1;
    }

    auto points = compute_absolute_points(raw_points, relative);
    if (points.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Error: No split points provided.";
        return 1;
    }

    if (const auto non_pos = std::ranges::find_if(points, [](double p) noexcept {
            return !std::isfinite(p) || p <= 0.0;
        }); non_pos != points.end()) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: Split points must be positive (got {} s).", *non_pos);
        return 1;
    }

    if (const auto non_inc = std::ranges::adjacent_find(points, std::greater_equal<>{});
        non_inc != points.end()) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: Split points must be strictly increasing ({} >= {}).",
            *non_inc, *std::next(non_inc));
        return 1;
    }

    Ffmpeg runner;
    BOOST_LOG_TRIVIAL(info) << "Probing video duration...";
    const double duration = runner.duration(input_file);
    if (duration < 0.0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine video duration.";
        return 1;
    }

    BOOST_LOG_TRIVIAL(info) << std::format("Video duration: {:.2f} s", duration);

    // Points are strictly increasing, so all points >= duration form a contiguous tail.
    const auto first_out = std::ranges::lower_bound(points, duration);
    for (const double p : std::ranges::subrange(first_out, points.end())) {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "Warning: Split point {} s is beyond duration. Ignoring.", p);
    }
    points.erase(first_out, points.end());

    if (points.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Error: No valid split points within video duration.";
        return 1;
    }

    const std::size_t num_parts = points.size() + 1;
    const std::string point_list = core::join(
        points | std::views::transform([](double p) { return std::format("{}s", p); }),
        " ");
    BOOST_LOG_TRIVIAL(info) << std::format("Splitting into {} parts at: {}", num_parts, point_list);

    const fs::path out_dir = out_str.empty()
        ? input_file.parent_path()
        : fs::absolute(core::from_utf8(out_str));
    if (!out_str.empty()) {
        fs::create_directories(out_dir);
    }

    const std::string stem = core::to_utf8(input_file.stem());
    const std::string ext  = core::to_utf8(input_file.extension());
    const std::string ts   = media::current_timestamp();

    std::vector<fs::path> written_parts;
    written_parts.reserve(num_parts);
    auto cleanup_on_failure = core::make_scope_guard([&written_parts]() noexcept {
        std::error_code ec;
        for (const auto& part : written_parts) {
            fs::remove(part, ec);
        }
    });

    double seg_start = 0.0;
    for (std::size_t i = 0; i < num_parts; ++i) {
        const double seg_end     = (i < points.size()) ? points[i] : -1.0;
        const double display_end = (seg_end >= 0.0) ? seg_end : duration;
        const fs::path part_path = out_dir / core::from_utf8(std::format("{}_part{}_{}{}", stem, i + 1, ts, ext));

        BOOST_LOG_TRIVIAL(info) << std::format(
            "Part {}/{}: [{}, {}] s -> {}",
            i + 1, num_parts, seg_start, display_end, core::to_utf8(part_path.filename()));

        written_parts.push_back(part_path);
        if (!runner.run_segment(input_file, part_path, seg_start, seg_end)) {
            BOOST_LOG_TRIVIAL(error) << std::format("Error: Failed to extract part {}.", i + 1);
            BOOST_LOG_TRIVIAL(error) << "Error: Split failed.";
            return 1;
        }

        std::error_code size_ec;
        const auto size = fs::file_size(part_path, size_ec);
        const double size_mb = size_ec ? 0.0 : static_cast<double>(size) / kBytesPerMiB;
        BOOST_LOG_TRIVIAL(info) << std::format("  -> {:.2f} MB", size_mb);

        seg_start = display_end;
    }

    cleanup_on_failure.dismiss();
    BOOST_LOG_TRIVIAL(info) << "Success! " << num_parts << " parts saved to: " << core::to_utf8(out_dir);
    return 0;
}

std::vector<double> SplitApp::compute_absolute_points(std::span<const double> points,
                                                      bool relative) {
    if (!relative) {
        return {points.begin(), points.end()};
    }

    std::vector<double> absolute(points.size());
    std::inclusive_scan(points.begin(), points.end(), absolute.begin());
    return absolute;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    SplitApp app;
    return app.run_cli(argc, argv);
}
#endif
