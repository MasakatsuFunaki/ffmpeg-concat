#include "cut.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <locale>
#include <sstream>

#include <boost/program_options.hpp>

#include "av/transcoder.h"
#include "ffmpeg/progress_reporter.h"

namespace po = boost::program_options;

namespace {
// Period decimal separator whatever the ambient C locale is.
std::string fixed_seconds(double v) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << v;
    return oss.str();
}
}  // namespace

std::string CutApp::name() const { return "Cut Video"; }

void CutApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",  po::value<std::string>()->required(),                      "Input video file (relative or absolute path)")
        ("start,s",  po::value<std::vector<double>>()->multitoken()->required(), "Start(s) of interval(s) to process (seconds, space-separated)")
        ("end,e",    po::value<std::vector<double>>()->multitoken()->required(), "End(s) of interval(s) to process (seconds, space-separated)")
        ("output,o", po::value<std::string>()->default_value(""),               "Output file path (default: input directory)")
        ("blur,b",   "Apply pixelized blur instead of cutting (keeps the intervals, pixelizes them)")
        ("blur-strength", po::value<int>()->default_value(30),                  "Pixelization block size (higher = more pixelated)")
        ("sound-off",  "Mute audio during blurred intervals (only with --blur)")
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
    Ffmpeg runner;

    fs::path input_file      = fs::absolute(vm["input"].as<std::string>());
    auto starts              = vm["start"].as<std::vector<double>>();
    auto ends                = vm["end"].as<std::vector<double>>();
    bool use_blur             = vm.count("blur") > 0;
    int blur_strength         = vm["blur-strength"].as<int>();
    bool sound_off            = vm.count("sound-off") > 0;

    if (starts.size() != ends.size()) {
        BOOST_LOG_TRIVIAL(error) << "Error: Number of start times (" << starts.size()
                  << ") must match number of end times (" << ends.size() << ").";
        return 1;
    }

    auto intervals = parse_intervals(starts, ends);
    if (intervals.empty()) return 1;

    if (!fs::exists(input_file) || !fs::is_regular_file(input_file)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input file does not exist: " << input_file;
        return 1;
    }

    fs::path output_file;
    std::string out_str = vm["output"].as<std::string>();
    if (out_str.empty()) {
        fs::path parent = input_file.parent_path();
        std::string stem = input_file.stem().string();
        std::string ext  = input_file.extension().string();
        std::string suffix = use_blur ? "_blur_" : "_cut_";
        output_file = parent / (stem + suffix + media::current_timestamp() + ext);
    } else {
        output_file = fs::absolute(out_str);
        if (output_file.has_parent_path()) {
            fs::create_directories(output_file.parent_path());
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Probing video duration...";
    double duration = runner.duration(input_file);
    if (duration < 0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine video duration.";
        return 1;
    }

    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(2) << "Video duration: " << duration << " s";

    // Validate intervals against duration
    for (auto& iv : intervals) {
        if (iv.first >= duration) {
            BOOST_LOG_TRIVIAL(error) << "Error: Start time (" << iv.first << " s) is beyond video duration (" << duration << " s).";
            return 1;
        }
        if (iv.second > duration) {
            BOOST_LOG_TRIVIAL(warning) << "Warning: End time (" << iv.second << " s) exceeds duration. Clamping to " << duration << " s.";
            iv.second = duration;
        }
    }

    if (use_blur) {
        std::ostringstream msg;
        msg << "Pixelizing " << intervals.size() << " interval(s):";
        for (const auto& iv : intervals)
            msg << " [" << iv.first << ", " << iv.second << "]";
        msg << " s (block size: " << blur_strength;
        if (sound_off) msg << ", sound off";
        msg << ")";
        BOOST_LOG_TRIVIAL(info) << msg.str();
    } else {
        std::ostringstream msg;
        msg << "Cutting out " << intervals.size() << " interval(s):";
        for (const auto& iv : intervals)
            msg << " [" << iv.first << ", " << iv.second << "]";
        msg << " s";
        BOOST_LOG_TRIVIAL(info) << msg.str();
    }

    // Blur mode: apply filter and return early
    if (use_blur) {
        BOOST_LOG_TRIVIAL(info) << "Applying pixelization filter...";
        bool ok = run_blur(runner, input_file, output_file, intervals, blur_strength, sound_off, duration);
        if (ok) {
            media::log_output_size(output_file);
        } else {
            BOOST_LOG_TRIVIAL(error) << "Error: Blur filter failed.";
        }
        return ok ? 0 : 1;
    }

    // Cut mode: extract segments between the intervals
    fs::path temp_dir = input_file.parent_path();
    std::vector<fs::path> kept_parts;
    std::error_code ec;

    // Build list of segments to keep: gaps between the sorted intervals
    // e.g. intervals [4,8],[15,20] on a 30s video -> keep [0,4], [8,15], [20,30]
    double prev_end = 0.0;
    for (size_t i = 0; i < intervals.size(); ++i) {
        double gap_start = prev_end;
        double gap_end   = intervals[i].first;
        if (gap_end - gap_start > 0.01) {
            fs::path part = temp_dir / ("_cut_part_" + std::to_string(i) + ".mp4");
            BOOST_LOG_TRIVIAL(info) << "Extracting segment [" << gap_start << ", " << gap_end << "] s ...";
            if (!runner.run_segment(input_file, part, gap_start, gap_end)) {
                BOOST_LOG_TRIVIAL(error) << "Error: Failed to extract segment [" << gap_start << ", " << gap_end << "].";
                for (auto& p : kept_parts) fs::remove(p, ec);
                return 1;
            }
            kept_parts.push_back(part);
        }
        prev_end = intervals[i].second;
    }
    // Segment after the last interval
    if (duration - prev_end > 0.01) {
        fs::path part = temp_dir / ("_cut_part_last.mp4");
        BOOST_LOG_TRIVIAL(info) << "Extracting segment [" << prev_end << ", " << duration << "] s ...";
        if (!runner.run_segment(input_file, part, prev_end, -1.0)) {
            BOOST_LOG_TRIVIAL(error) << "Error: Failed to extract final segment.";
            for (auto& p : kept_parts) fs::remove(p, ec);
            return 1;
        }
        kept_parts.push_back(part);
    }

    if (kept_parts.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Error: The intervals cover the entire video. Nothing left to keep.";
        return 1;
    }

    bool ok = false;
    if (kept_parts.size() == 1) {
        fs::rename(kept_parts[0], output_file, ec);
        ok = !ec;
    } else {
        BOOST_LOG_TRIVIAL(info) << "Reassembling " << kept_parts.size() << " parts...";
        double kept_duration = duration;
        for (const auto& iv : intervals) kept_duration -= (iv.second - iv.first);
        IndicatorsProgressReporter rep;
        ok = runner.concat(kept_parts, output_file, kept_duration, rep);
        for (auto& p : kept_parts) fs::remove(p, ec);
    }

    if (ok) {
        media::log_output_size(output_file);
    } else {
        BOOST_LOG_TRIVIAL(error) << "Error: Failed to produce output file.";
    }

    return ok ? 0 : 1;
}

std::vector<CutApp::Interval> CutApp::parse_intervals(
        const std::vector<double>& starts,
        const std::vector<double>& ends) {
    std::vector<Interval> intervals;
    for (size_t i = 0; i < starts.size(); ++i) {
        if (starts[i] < 0 || ends[i] < 0) {
            BOOST_LOG_TRIVIAL(error) << "Error: Start and end times must be non-negative.";
            return {};
        }
        if (starts[i] >= ends[i]) {
            BOOST_LOG_TRIVIAL(error) << "Error: Start time (" << starts[i]
                      << ") must be less than end time (" << ends[i] << ") for interval " << (i + 1) << ".";
            return {};
        }
        intervals.emplace_back(starts[i], ends[i]);
    }

    // Sort by start time
    std::sort(intervals.begin(), intervals.end());

    // Check for overlaps
    for (size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].first < intervals[i - 1].second) {
            BOOST_LOG_TRIVIAL(error) << "Error: Intervals overlap: ["
                      << intervals[i - 1].first << "," << intervals[i - 1].second << "] and ["
                      << intervals[i].first << "," << intervals[i].second << "].";
            return {};
        }
    }

    return intervals;
}

bool CutApp::run_blur(const Ffmpeg& /*runner*/,
                      const fs::path& input_file,
                      const fs::path& output_file,
                      const std::vector<Interval>& intervals,
                      int blur_strength,
                      bool sound_off,
                      double duration_sec) {
    int block = std::max(2, blur_strength);

    // Build combined enable expression: between(t,s1,e1)+between(t,s2,e2)+...
    // Times are written with a classic-locale formatter because libavfilter
    // parses them through the C locale, which this process may have switched to
    // comma-decimal; av::transcode also pins LC_NUMERIC while it parses.
    std::string enable_expr;
    for (size_t i = 0; i < intervals.size(); ++i) {
        if (i > 0) enable_expr += "+";
        enable_expr += "between(t," + fixed_seconds(intervals[i].first)
                     + "," + fixed_seconds(intervals[i].second) + ")";
    }
    std::string enable = "enable='" + enable_expr + "'";

    std::string video_chain = "split[a][b];"
                         "[b]" + std::string("gblur=sigma=1:") + enable + ","
                         "scale=iw/" + std::to_string(block) + ":ih/" + std::to_string(block) + ","
                         "scale=iw*" + std::to_string(block) + ":ih*" + std::to_string(block) + ":flags=neighbor[px];"
                         "[a][px]overlay=0:0:" + enable;

    av::TranscodeRequest req;
    req.inputs = {input_file};
    req.output = output_file;
    req.video_encoder = "libx264";
    req.filter_description = "[in0v]" + video_chain + "[outv]";
    if (sound_off) {
        // Muting is a filter, so the audio has to be decoded and re-encoded.
        req.filter_description += ";[in0a]volume=0:enable='" + enable_expr + "'[outa]";
    } else {
        req.audio_copy = true;
    }

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
