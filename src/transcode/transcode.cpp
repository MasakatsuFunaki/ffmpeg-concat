#include "transcode.h"

#include "av/av_capabilities.h"
#include "av/media_probe.h"
#include "av/transcoder.h"
#include "core/format_util.h"
#include "core/scoped_handle.h"
#include "encode/encode_plan.h"
#include "encode/encoder_catalog.h"
#include "ffmpeg/progress_reporter.h"

#include <boost/program_options.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace po = boost::program_options;

namespace {
constexpr int kMinCrf = 0;
constexpr int kMaxCrf = 51;
constexpr int kDefaultCrf = 30;
}  // namespace

std::string TranscodeApp::name() const { return "Transcode Video"; }

void TranscodeApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",   po::value<std::string>()->required(),              "Input file or directory containing MP4 files")
        ("output,o",  po::value<std::string>()->required(),              "Output directory for transcoded video")
        ("crf,q",     po::value<int>()->default_value(kDefaultCrf),      "Quality (0=best, 51=worst, default: 30)")
        ("preset,p",  po::value<std::string>()->default_value(""),       "Encoder preset (av1_nvenc/h264_nvenc: p1-p7, libsvtav1: 0-13, libx264: ultrafast-veryslow, libaom-av1: 0-8)")
        ("encoder,e", po::value<std::string>()->default_value(""),       "Force encoder (av1_nvenc, libsvtav1, libaom-av1, h264_nvenc, libx264). Auto-detects AV1 if empty")
    ;
}

int TranscodeApp::execute(const po::variables_map& vm) {
    const fs::path input_path = fs::absolute(vm["input"].as<std::string>());
    const fs::path output_dir = fs::absolute(vm["output"].as<std::string>());
    const int crf             = vm["crf"].as<int>();
    std::string preset        = vm["preset"].as<std::string>();
    std::string encoder       = vm["encoder"].as<std::string>();

    if (crf < kMinCrf || crf > kMaxCrf) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: CRF must be between {} and {} (got {}).", kMinCrf, kMaxCrf, crf);
        return 1;
    }

    const bool input_is_file = fs::is_regular_file(input_path);
    const bool input_is_dir  = fs::is_directory(input_path);

    if (!fs::exists(input_path) || (!input_is_file && !input_is_dir)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input path does not exist or is not a file/directory: " << input_path;
        return 1;
    }

    if (encoder.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Detecting best AV1 encoder...";
        encoder = EncoderCatalog::detect("av1");
        if (encoder.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Error: No AV1 encoder found. This build has no usable AV1 encoder.";
            return 1;
        }
    } else if (!av::encoder_exists(encoder)) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: Unknown or unavailable encoder: '{}'.", encoder);
        return 1;
    }
    BOOST_LOG_TRIVIAL(info) << std::format("Using encoder: {}", encoder);

    std::vector<fs::path> files_to_encode;
    if (input_is_file) {
        files_to_encode.push_back(input_path);
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Input is a single file: {}", input_path.filename().string());
    } else {
        files_to_encode = media::collect_mp4_files(input_path);
        if (files_to_encode.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Error: No MP4 files found in " << input_path;
            return 1;
        }

        BOOST_LOG_TRIVIAL(info) << std::format("Found {} MP4 file(s):", files_to_encode.size());
        for (const auto& file : files_to_encode) {
            BOOST_LOG_TRIVIAL(info) << std::format("  - {}", file.filename().string());
        }

        if (files_to_encode.size() == 1) {
            BOOST_LOG_TRIVIAL(info) << "Single file, skipping concat step.";
        } else {
            BOOST_LOG_TRIVIAL(info) << "Multiple files detected. Joining via concat filter during encode...";
        }
    }

    fs::create_directories(output_dir);

    const std::string timestamp = media::current_timestamp();
    fs::path output_file = output_dir / std::format("transcoded_{}.mp4", timestamp);
    for (std::size_t suffix = 1; fs::exists(output_file); ++suffix) {
        output_file = output_dir / std::format("transcoded_{}_{}.mp4", timestamp, suffix);
    }

    const bool use_concat_filter = files_to_encode.size() > 1;
    double total_duration_sec = 0.0;
    std::vector<av::FrameRate> frame_rates;
    if (use_concat_filter) {
        frame_rates.reserve(files_to_encode.size());
    }

    for (const auto& file : files_to_encode) {
        const av::MediaInfo info = av::probe(file);
        if (info.duration_sec > 0.0) {
            total_duration_sec += info.duration_sec;
        }
        // Only relevant when segments get joined; a lone file cannot be mixed.
        if (use_concat_filter) {
            frame_rates.push_back(info.frame_rate);
        }
    }

    if (total_duration_sec > 0.0) {
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Total duration: {}", core::format_hms(total_duration_sec));
    }

    BOOST_LOG_TRIVIAL(info) << "Encoding with " << encoder << ": " << output_file;

    // CUDA availability is a host property, so probe it once per run.
    const bool hwaccel_cuda = av::can_use_hwaccel("cuda");
    BOOST_LOG_TRIVIAL(info) << (hwaccel_cuda
        ? "CUDA available: decoding on the GPU."
        : "CUDA unavailable: decoding in software.");

    const av::FrameRate target_fps = lowest_fps_if_mixed(frame_rates);
    if (target_fps.known()) {
        BOOST_LOG_TRIVIAL(info) << std::format(
            "Mixed frame rates; normalizing every segment to {} fps (the lowest).",
            target_fps.value());
    }

    const EncodePlan plan{
        .inputs = std::move(files_to_encode),
        .output = output_file,
        .encoder = std::move(encoder),
        .crf = crf,
        .preset = std::move(preset),
        .hwaccel_cuda = hwaccel_cuda,
        .target_fps = target_fps,
    };

    const av::TranscodeRequest request = build_request(plan);
    BOOST_LOG_TRIVIAL(info) << "Filter graph: " << request.filter_description;

    auto remove_partial_output = core::make_scope_guard([&output_file]() noexcept {
        std::error_code ec;
        fs::remove(output_file, ec);
    });

    IndicatorsProgressReporter rep;
    const bool encode_ok = av::transcode(request, rep, total_duration_sec);
    if (!encode_ok) {
        return 1;
    }

    remove_partial_output.dismiss();
    media::log_output_size(output_file);
    return 0;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    TranscodeApp app;
    return app.run(argc, argv);
}
#endif
