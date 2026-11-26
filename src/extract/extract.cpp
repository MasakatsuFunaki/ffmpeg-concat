#include "extract.h"

#include "extract/equally_spaced_extractor.h"
#include "extract/frame_extractor.h"
#include "extract/striking_frame_extractor.h"
#include "extract/text_crop_extractor.h"
#include "core/path_util.h"
#include "core/profiler.h"

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace {

constexpr std::size_t kMaxMosaicArgs = 2;
constexpr int kMinMosaicCount = 1;
constexpr int kMinMosaicStart = 1;
constexpr int kDefaultMosaicStart = 1;
constexpr std::size_t kMaxExtractors = 3;

}  // namespace

std::string ExtractApp::name() const { return "Extract Frames"; }

void ExtractApp::define_options(po::options_description& desc) {
    desc.add_options()
        ("input,i",    po::value<std::string>()->required(),          "Input directory containing video files, or path to a single video file")
        ("output,o",   po::value<std::string>()->required(),         "Output directory for extracted frames")
        ("image",      po::value<int>(),                                          "Extract N equally-spaced frames per video")
        ("image-detect", "Auto-detect visually striking frames (max 100 per video)")
        ("mosaic",     po::value<std::vector<int>>()->multitoken(),   "Create mosaic of N time-distributed striking photos (requires --image-detect).\n"
                                                                              "  Usage: --mosaic <N> [P]  where P = 1-indexed start position among striking frames (default: 1)")
        ("extract-text", "Extract unique text crops from frames (Latin/Western text)")
        ("profile",    "Print per-stage profiling summary after extraction")
    ;
}

std::string ExtractApp::extra_help() const {
    return "\nModes:\n"
           "  --image N        Extract N equally-spaced frames from each video.\n"
           "  --image-detect   Auto-detect visually striking frames (max 100 per video).\n"
           "  --mosaic N [P]   Create a mosaic of N time-distributed striking photos (requires --image-detect).\n"
           "                   Optional P = 1-indexed start position among striking frames (default: 1).\n"
           "                   Only frames from position P onward are considered for the mosaic.\n"
           "  --extract-text   Extract unique text crops from frames (Latin/Western text).\n"
           "\nAll modes can be used simultaneously.\n"
           "Input can be a directory (scanned for mp4, avi, mkv, mov, wmv, flv, webm, ts, m4v) or a path to a single video file.";
}

int ExtractApp::execute(const po::variables_map& vm) {
    const bool do_image = vm.count("image") > 0;
    const bool do_detect = vm.count("image-detect") > 0;
    const bool do_text = vm.count("extract-text") > 0;
    const bool do_mosaic = vm.count("mosaic") > 0;
    const bool do_profile = vm.count("profile") > 0;
    const int num_samples = do_image ? vm["image"].as<int>() : 0;

    std::optional<MosaicSpec> mosaic;
    if (do_mosaic) {
        const auto& mosaic_args = vm["mosaic"].as<std::vector<int>>();
        if (mosaic_args.empty() || mosaic_args.size() > kMaxMosaicArgs) {
            BOOST_LOG_TRIVIAL(error) << std::format(
                "Error: --mosaic expects 1 or 2 arguments: <N> [P] (got {}).",
                mosaic_args.size());
            return 1;
        }
        const int count = mosaic_args[0];
        const int start =
            (mosaic_args.size() == kMaxMosaicArgs) ? mosaic_args[1] : kDefaultMosaicStart;
        mosaic = MosaicSpec{count, start};
    }

    if (!do_image && !do_detect && !do_text) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Specify at least --image <N>, --image-detect, or --extract-text.";
        return 1;
    }

    if (do_image && num_samples <= 0) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: --image requires a positive integer (got {}).", num_samples);
        return 1;
    }

    if (mosaic && mosaic->count < kMinMosaicCount) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: --mosaic requires a positive integer >= 1 (got {}).", mosaic->count);
        return 1;
    }

    if (mosaic && mosaic->start < kMinMosaicStart) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "Error: --mosaic start position P must be >= 1 (got {}).", mosaic->start);
        return 1;
    }

    if (mosaic && !do_detect) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: --mosaic requires --image-detect to be specified.";
        return 1;
    }

    const fs::path input_path =
        fs::absolute(core::from_utf8(vm["input"].as<std::string>()));
    const fs::path output_dir =
        fs::absolute(core::from_utf8(vm["output"].as<std::string>()));

    if (!fs::exists(input_path)) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Input path does not exist: " << core::to_utf8(input_path);
        return 1;
    }

    std::vector<fs::path> video_files;
    if (fs::is_regular_file(input_path)) {
        video_files.push_back(input_path);
    } else if (fs::is_directory(input_path)) {
        video_files = FFmpegUtils::collect_video_files(input_path);
    } else {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Input is neither a file nor a directory: " << core::to_utf8(input_path);
        return 1;
    }

    if (video_files.empty()) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: No video files found in " << core::to_utf8(input_path);
        return 1;
    }

    fs::create_directories(output_dir);

    BOOST_LOG_TRIVIAL(info) << std::format("Found {} video file(s):", video_files.size());
    for (const auto& f : video_files) {
        BOOST_LOG_TRIVIAL(info) << "  - " << core::to_utf8(f.filename());
    }

    // Build extraction strategies in mode order: image -> striking -> text.
    std::vector<std::unique_ptr<FrameExtractor>> extractors;
    extractors.reserve(kMaxExtractors);
    if (do_image) {
        extractors.push_back(std::make_unique<EquallySpacedExtractor>(num_samples));
    }
    if (do_detect) {
        extractors.push_back(std::make_unique<StrikingFrameExtractor>(mosaic));
    }
    if (do_text) {
        extractors.push_back(std::make_unique<TextCropExtractor>());
    }

    std::optional<core::ScopedProfilerEnable> profile_guard;
    if (do_profile) {
        core::Profiler::global().reset();
        profile_guard.emplace(core::Profiler::global(), true);
    }

    const Ffmpeg runner;
    bool all_ok = true;

    for (const auto& video : video_files) {
        const fs::path video_out_dir = output_dir / video.stem();
        fs::create_directories(video_out_dir);

        BOOST_LOG_TRIVIAL(info) << std::format(
            "=== Processing: {} ===", core::to_utf8(video.filename()));

        // Execute every mode without short-circuiting earlier failures.
        for (const auto& ex : extractors) {
            if (!ex->run(runner, video, video_out_dir)) {
                all_ok = false;
            }
        }
    }

    if (do_profile) {
        BOOST_LOG_TRIVIAL(info) << "\n=== Extraction Profile ===\n"
                                << core::Profiler::global().format_summary();
    }

    const std::string output_utf8 = core::to_utf8(output_dir);
    if (all_ok) {
        BOOST_LOG_TRIVIAL(info) << "Success! All frames extracted to: " << output_utf8;
    } else {
        BOOST_LOG_TRIVIAL(info)
            << "Completed with some warnings. Check output in: " << output_utf8;
    }

    return all_ok ? 0 : 1;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    ExtractApp app;
    return app.run_cli(argc, argv);
}
#endif
