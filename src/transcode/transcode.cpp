#include "transcode.h"

#include "av/av_capabilities.h"
#include "av/media_probe.h"
#include "av/transcoder.h"
#include "ffmpeg/progress_reporter.h"
#include "encode/encode_plan.h"
#include "encode/encoder_catalog.h"

#include <iomanip>
#include <sstream>
#include <utility>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

std::string TranscodeApp::name() const { return "Transcode Video"; }

void TranscodeApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",  po::value<std::string>()->required(),          "Input file or directory containing MP4 files")
        ("output,o", po::value<std::string>()->required(),         "Output directory for transcoded video")
        ("crf,q",    po::value<int>()->default_value(30),                       "Quality (0=best, 51=worst, default: 30)")
        ("preset,p", po::value<std::string>()->default_value(""),               "Encoder preset (av1_nvenc/h264_nvenc: p1-p7, libsvtav1: 0-13, libx264: ultrafast-veryslow, libaom-av1: 0-8)")
        ("encoder,e", po::value<std::string>()->default_value(""),              "Force encoder (av1_nvenc, libsvtav1, libaom-av1, h264_nvenc, libx264). Auto-detects AV1 if empty")
    ;
}

int TranscodeApp::execute(const po::variables_map& vm) {
    fs::path input_path  = fs::absolute(vm["input"].as<std::string>());
    fs::path output_dir  = fs::absolute(vm["output"].as<std::string>());
    int crf              = vm["crf"].as<int>();
    std::string preset   = vm["preset"].as<std::string>();
    std::string encoder  = vm["encoder"].as<std::string>();

    bool input_is_file = fs::is_regular_file(input_path);
    bool input_is_dir  = fs::is_directory(input_path);

    if (!fs::exists(input_path) || (!input_is_file && !input_is_dir)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input path does not exist or is not a file/directory: " << input_path;
        return 1;
    }

    fs::create_directories(output_dir);

    if (encoder.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Detecting best AV1 encoder...";
        encoder = EncoderCatalog::detect("av1");
        if (encoder.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Error: No AV1 encoder found. This build has no usable AV1 encoder.";
            return 1;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Using encoder: " << encoder;

    std::vector<fs::path> files_to_encode;

    if (input_is_file) {
        files_to_encode.push_back(input_path);
        BOOST_LOG_TRIVIAL(info) << "Input is a single file: " << input_path.filename().string();
    } else {
        files_to_encode = media::collect_mp4_files(input_path);

        if (files_to_encode.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Error: No MP4 files found in " << input_path;
            return 1;
        }

        BOOST_LOG_TRIVIAL(info) << "Found " << files_to_encode.size() << " MP4 file(s):";
        for (const auto& f : files_to_encode) {
            BOOST_LOG_TRIVIAL(info) << "  - " << f.filename().string();
        }

        if (files_to_encode.size() == 1) {
            BOOST_LOG_TRIVIAL(info) << "Single file, skipping concat step.";
        } else {
            BOOST_LOG_TRIVIAL(info) << "Multiple files detected. Joining via concat filter during encode...";
        }
    }

    fs::path output_file = output_dir / ("transcoded_" + media::current_timestamp() + ".mp4");

    const bool use_concat_filter = files_to_encode.size() > 1;

    double total_duration_sec = 0.0;
    std::vector<av::FrameRate> frame_rates;
    for (const auto& f : files_to_encode) {
        av::MediaInfo info = av::probe(f);
        if (info.duration_sec > 0) total_duration_sec += info.duration_sec;
        // Only relevant when segments get joined; a lone file cannot be "mixed".
        if (use_concat_filter) frame_rates.push_back(info.frame_rate);
    }
    if (total_duration_sec > 0) {
        int total_h = static_cast<int>(total_duration_sec) / 3600;
        int total_m = (static_cast<int>(total_duration_sec) % 3600) / 60;
        int total_s = static_cast<int>(total_duration_sec) % 60;
        std::ostringstream dur_oss;
        dur_oss << std::setfill('0') << std::setw(2) << total_h << ":"
                << std::setfill('0') << std::setw(2) << total_m << ":"
                << std::setfill('0') << std::setw(2) << total_s;
        BOOST_LOG_TRIVIAL(info) << "Total duration: " << dur_oss.str();
    }

    BOOST_LOG_TRIVIAL(info) << "Encoding with " << encoder << ": " << output_file;

    if (files_to_encode.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Error: run_encode called with no input files.";
        return 1;
    }

    EncodePlan plan;
    plan.inputs  = files_to_encode;
    plan.output  = output_file;
    plan.encoder = encoder;
    plan.crf     = crf;
    plan.preset  = preset;

    // Decoding is the bottleneck for modern codecs — software AV1 in particular
    // is several times slower than the NVENC encode it feeds. CUDA availability
    // is a property of the host, not of any one file, so probe it once.
    plan.hwaccel_cuda = av::can_use_hwaccel("cuda");
    BOOST_LOG_TRIVIAL(info) << (plan.hwaccel_cuda
        ? "CUDA available: decoding on the GPU."
        : "CUDA unavailable: decoding in software.");

    plan.target_fps = lowest_fps_if_mixed(frame_rates);
    if (plan.target_fps.known()) {
        BOOST_LOG_TRIVIAL(info) << "Mixed frame rates; normalizing every segment to "
                                << plan.target_fps.value() << " fps (the lowest).";
    }

    av::TranscodeRequest request = build_request(plan);
    BOOST_LOG_TRIVIAL(info) << "Filter graph: " << request.filter_description;

    IndicatorsProgressReporter rep;
    bool encode_ok = av::transcode(request, rep, total_duration_sec);

    if (encode_ok) {
        media::log_output_size(output_file);
        return 0;
    }

    return 1;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    TranscodeApp app;
    return app.run(argc, argv);
}
#endif
