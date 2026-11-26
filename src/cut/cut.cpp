#include "cut.h"

#include "av/transcoder.h"
#include "core/format_util.h"
#include "core/scoped_handle.h"
#include "ffmpeg/progress_reporter.h"

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
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace po = boost::program_options;

namespace {
constexpr double kMinGapSec = 0.01;
constexpr int kMinBlurBlock = 2;
constexpr int kDefaultBlurStrength = 30;
}  // namespace

std::string CutApp::name() const { return "Cut Video"; }

void CutApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",       po::value<std::string>()->required(),                      "Input video file (relative or absolute path)")
        ("start,s",       po::value<std::vector<double>>()->multitoken()->required(), "Start(s) of interval(s) to process (seconds, space-separated)")
        ("end,e",         po::value<std::vector<double>>()->multitoken()->required(), "End(s) of interval(s) to process (seconds, space-separated)")
        ("output,o",      po::value<std::string>()->default_value(""),               "Output file path (default: input directory)")
        ("blur,b",        "Apply pixelized blur instead of cutting (keeps the intervals, pixelizes them)")
        ("blur-strength", po::value<int>()->default_value(kDefaultBlurStrength),     "Pixelization block size (higher = more pixelated)")
        ("sound-off",     "Mute audio during blurred intervals (only with --blur)")
    ;
}

std::string CutApp::extra_help() const {
    return "\nModes:\n"
           "  Default:  Cuts out the intervals and reassembles the remaining parts.\n"
           "  --blur:   Pixelizes the intervals instead of removing them.\n"
           "\nMultiple intervals: use space-separated values\n"
           "  -s 4 15 30 -e 8 20 35   (three intervals: [4,8], [15,20], [30,35])";
}

int CutApp::execute(const po::variables_map& vm) {
    const fs::path input_file   = fs::absolute(vm["input"].as<std::string>());
    const auto starts           = vm["start"].as<std::vector<double>>();
    const auto ends             = vm["end"].as<std::vector<double>>();
    const bool use_blur         = vm.count("blur") > 0;
    const int blur_strength     = vm["blur-strength"].as<int>();
    const bool sound_off        = vm.count("sound-off") > 0;
    const std::string out_str   = vm["output"].as<std::string>();

    if (starts.size() != ends.size()) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: Number of start times ({}) must match number of end times ({}).",
            starts.size(), ends.size());
        return 1;
    }

    auto intervals = parse_intervals(starts, ends);
    if (intervals.empty()) {
        return 1;
    }

    if (!fs::exists(input_file) || !fs::is_regular_file(input_file)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input file does not exist: " << input_file;
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

    for (auto& iv : intervals) {
        if (iv.start >= duration) {
            BOOST_LOG_TRIVIAL(error) << std::format(
                "Error: Start time ({} s) is beyond video duration ({} s).",
                iv.start, duration);
            return 1;
        }
        if (iv.end > duration) {
            BOOST_LOG_TRIVIAL(warning) << std::format(
                "Warning: End time ({} s) exceeds duration. Clamping to {} s.",
                iv.end, duration);
            iv.end = duration;
        }
    }

    fs::path output_file;
    if (out_str.empty()) {
        const fs::path parent        = input_file.parent_path();
        const std::string stem       = input_file.stem().string();
        const std::string ext        = input_file.extension().string();
        const std::string_view tag   = use_blur ? "_blur_" : "_cut_";
        const std::string timestamp  = media::current_timestamp();
        output_file = parent / std::format("{}{}{}{}", stem, tag, timestamp, ext);
        for (std::size_t n = 1; fs::exists(output_file); ++n) {
            output_file = parent / std::format("{}{}{}_{}{}", stem, tag, timestamp, n, ext);
        }
    } else {
        output_file = fs::absolute(out_str);
    }

    const std::string interval_list = core::join(
        intervals | std::views::transform([](const Interval& iv) {
            return std::format("[{}, {}]", iv.start, iv.end);
        }),
        " ");

    if (use_blur) {
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Pixelizing {} interval(s): {} s (block size: {}{})",
            intervals.size(), interval_list, blur_strength,
            sound_off ? ", sound off" : "");

        if (output_file.has_parent_path()) {
            fs::create_directories(output_file.parent_path());
        }
        const bool existed_before = fs::exists(output_file);
        auto remove_partial_output = core::make_scope_guard([&output_file, existed_before]() noexcept {
            if (!existed_before) {
                std::error_code ec;
                fs::remove(output_file, ec);
            }
        });

        BOOST_LOG_TRIVIAL(info) << "Applying pixelization filter...";
        const bool ok = run_blur(input_file, output_file, intervals, blur_strength, sound_off, duration);
        if (!ok) {
            BOOST_LOG_TRIVIAL(error) << "Error: Blur filter failed.";
            return 1;
        }
        remove_partial_output.dismiss();
        media::log_output_size(output_file);
        return 0;
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "Cutting out {} interval(s): {} s", intervals.size(), interval_list);

    // Compute kept gaps between sorted intervals before creating directories or temp files.
    std::vector<Interval> kept_gaps;
    kept_gaps.reserve(intervals.size());
    double prev_end = 0.0;
    for (const auto& iv : intervals) {
        if (iv.start - prev_end > kMinGapSec) {
            kept_gaps.push_back(Interval{prev_end, iv.start});
        }
        prev_end = iv.end;
    }
    const bool has_tail = (duration - prev_end > kMinGapSec);

    if (kept_gaps.empty() && !has_tail) {
        BOOST_LOG_TRIVIAL(error) << "Error: The intervals cover the entire video. Nothing left to keep.";
        return 1;
    }

    const fs::path temp_dir = output_file.parent_path();
    if (!temp_dir.empty()) {
        fs::create_directories(temp_dir);
    }

    const bool existed_before = fs::exists(output_file);
    std::vector<fs::path> kept_parts;
    kept_parts.reserve(kept_gaps.size() + (has_tail ? 1 : 0));

    auto cleanup_parts = core::make_scope_guard([&kept_parts]() noexcept {
        std::error_code ec;
        for (const auto& part : kept_parts) {
            fs::remove(part, ec);
        }
    });

    for (std::size_t i = 0; i < kept_gaps.size(); ++i) {
        const auto& gap = kept_gaps[i];
        const fs::path part = temp_dir / std::format("_cut_part_{}.mp4", i);
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Extracting segment [{}, {}] s ...", gap.start, gap.end);
        kept_parts.push_back(part);
        if (!runner.run_segment(input_file, part, gap.start, gap.end)) {
            BOOST_LOG_TRIVIAL(error) << std::format(
                "Error: Failed to extract segment [{}, {}].", gap.start, gap.end);
            return 1;
        }
    }

    if (has_tail) {
        const fs::path part = temp_dir / "_cut_part_last.mp4";
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Extracting segment [{}, {}] s ...", prev_end, duration);
        kept_parts.push_back(part);
        if (!runner.run_segment(input_file, part, prev_end, -1.0)) {
            BOOST_LOG_TRIVIAL(error) << "Error: Failed to extract final segment.";
            return 1;
        }
    }

    auto remove_partial_output = core::make_scope_guard([&output_file, existed_before]() noexcept {
        if (!existed_before) {
            std::error_code ec;
            fs::remove(output_file, ec);
        }
    });

    bool ok = false;
    if (kept_parts.size() == 1) {
        std::error_code ec;
        fs::rename(kept_parts[0], output_file, ec);
        if (!ec) {
            cleanup_parts.dismiss();
            ok = true;
        } else {
            ec.clear();
            ok = fs::copy_file(kept_parts[0], output_file,
                               fs::copy_options::overwrite_existing, ec);
        }
    } else {
        BOOST_LOG_TRIVIAL(info) << std::format("Reassembling {} parts...", kept_parts.size());
        const double cut_duration = std::transform_reduce(
            intervals.begin(), intervals.end(), 0.0, std::plus<>{},
            [](const Interval& iv) noexcept { return iv.length(); });
        const double kept_duration = std::max(0.0, duration - cut_duration);
        IndicatorsProgressReporter rep;
        ok = runner.concat(kept_parts, output_file, kept_duration, rep);
    }

    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "Error: Failed to produce output file.";
        return 1;
    }

    remove_partial_output.dismiss();
    media::log_output_size(output_file);
    return 0;
}

std::vector<CutApp::Interval> CutApp::parse_intervals(std::span<const double> starts,
                                                      std::span<const double> ends) {
    if (starts.size() != ends.size() || starts.empty()) {
        return {};
    }

    std::vector<Interval> intervals;
    intervals.reserve(starts.size());

    for (std::size_t i = 0; i < starts.size(); ++i) {
        const double s = starts[i];
        const double e = ends[i];
        if (!std::isfinite(s) || !std::isfinite(e) || s < 0.0 || e < 0.0) {
            BOOST_LOG_TRIVIAL(error) << "Error: Start and end times must be non-negative.";
            return {};
        }
        if (s >= e) {
            BOOST_LOG_TRIVIAL(error) << std::format(
                "Error: Start time ({}) must be less than end time ({}) for interval {}.",
                s, e, i + 1);
            return {};
        }
        intervals.push_back(Interval{s, e});
    }

    std::ranges::sort(intervals);

    if (const auto overlap = std::ranges::adjacent_find(
            intervals,
            [](const Interval& lhs, const Interval& rhs) noexcept { return rhs.start < lhs.end; });
        overlap != intervals.end()) {
        const auto& next = *std::next(overlap);
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: Intervals overlap: [{},{}] and [{},{}].",
            overlap->start, overlap->end, next.start, next.end);
        return {};
    }

    return intervals;
}

bool CutApp::run_blur(const fs::path& input_file,
                      const fs::path& output_file,
                      std::span<const Interval> intervals,
                      int blur_strength,
                      bool sound_off,
                      double duration_sec) {
    const int block = std::max(kMinBlurBlock, blur_strength);

    // Classic-locale decimals are required because libavfilter parses expressions under LC_NUMERIC="C".
    const std::string enable_expr = core::join(
        intervals | std::views::transform([](const Interval& iv) {
            return std::format("between(t,{},{})",
                               core::format_fixed(iv.start),
                               core::format_fixed(iv.end));
        }),
        "+");
    const std::string enable = std::format("enable='{}'", enable_expr);

    const std::string video_chain = std::format(
        "split[a][b];"
        "[b]gblur=sigma=1:{0},"
        "scale=iw/{1}:ih/{1},"
        "scale=iw*{1}:ih*{1}:flags=neighbor[px];"
        "[a][px]overlay=0:0:{0}",
        enable, block);

    std::string filter_description = std::format("[in0v]{}[outv]", video_chain);
    if (sound_off) {
        // Muting is a filter, so audio must be decoded and re-encoded.
        filter_description += std::format(";[in0a]volume=0:{}[outa]", enable);
    }

    const av::TranscodeRequest req{
        .inputs = {input_file},
        .output = output_file,
        .filter_description = std::move(filter_description),
        .video_encoder = "libx264",
        .video_options = {},
        .audio_copy = !sound_off,
    };

    BOOST_LOG_TRIVIAL(info) << "Filter graph: " << req.filter_description;

    IndicatorsProgressReporter rep;
    return av::transcode(req, rep, duration_sec);
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    CutApp app;
    return app.run(argc, argv);
}
#endif
