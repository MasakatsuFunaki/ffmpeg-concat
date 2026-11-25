#include "extract.h"

#include "extract/frame_extractor.h"
#include "extract/equally_spaced_extractor.h"
#include "extract/striking_frame_extractor.h"
#include "extract/text_crop_extractor.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

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
    bool do_image  = vm.count("image") > 0;
    bool do_detect = vm.count("image-detect") > 0;
    bool do_text   = vm.count("extract-text") > 0;
    bool do_mosaic = vm.count("mosaic") > 0;
    int num_samples = do_image ? vm["image"].as<int>() : 0;
    int mosaic_count = 0;
    int mosaic_start = 1;
    if (do_mosaic) {
        auto mosaic_args = vm["mosaic"].as<std::vector<int>>();
        if (mosaic_args.empty() || mosaic_args.size() > 2) {
            BOOST_LOG_TRIVIAL(error) << "Error: --mosaic expects 1 or 2 arguments: <N> [P] (got " << mosaic_args.size() << ").";
            return 1;
        }
        mosaic_count = mosaic_args[0];
        if (mosaic_args.size() == 2) {
            mosaic_start = mosaic_args[1];
        }
    }

    if (!do_image && !do_detect && !do_text) {
        BOOST_LOG_TRIVIAL(error) << "Error: Specify at least --image <N>, --image-detect, or --extract-text.";
        return 1;
    }

    if (do_image && num_samples <= 0) {
        BOOST_LOG_TRIVIAL(error) << "Error: --image requires a positive integer (got " << num_samples << ").";
        return 1;
    }

    if (do_mosaic && mosaic_count < 1) {
        BOOST_LOG_TRIVIAL(error) << "Error: --mosaic requires a positive integer >= 1 (got " << mosaic_count << ").";
        return 1;
    }

    if (do_mosaic && mosaic_start < 1) {
        BOOST_LOG_TRIVIAL(error) << "Error: --mosaic start position P must be >= 1 (got " << mosaic_start << ").";
        return 1;
    }

    if (do_mosaic && !do_detect) {
        BOOST_LOG_TRIVIAL(error) << "Error: --mosaic requires --image-detect to be specified.";
        return 1;
    }

    fs::path input_path  = fs::absolute(vm["input"].as<std::string>());
    fs::path output_dir  = fs::absolute(vm["output"].as<std::string>());

    if (!fs::exists(input_path)) {
        BOOST_LOG_TRIVIAL(error) << "Error: Input path does not exist: " << input_path;
        return 1;
    }

    fs::create_directories(output_dir);

    std::vector<fs::path> video_files;
    if (fs::is_regular_file(input_path)) {
        video_files.push_back(input_path);
    } else if (fs::is_directory(input_path)) {
        video_files = FFmpegUtils::collect_video_files(input_path);
    } else {
        BOOST_LOG_TRIVIAL(error) << "Error: Input is neither a file nor a directory: " << input_path;
        return 1;
    }

    if (video_files.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Error: No video files found in " << input_path;
        return 1;
    }

    BOOST_LOG_TRIVIAL(info) << "Found " << video_files.size() << " video file(s):";
    for (const auto& f : video_files) {
        BOOST_LOG_TRIVIAL(info) << "  - " << f.filename().string();
    }

    // Build the extraction strategies in mode order: image -> striking -> text.
    std::vector<std::unique_ptr<FrameExtractor>> extractors;
    if (do_image) {
        extractors.push_back(std::make_unique<EquallySpacedExtractor>(num_samples));
    }
    if (do_detect) {
        std::optional<MosaicSpec> mosaic;
        if (do_mosaic) mosaic = MosaicSpec{mosaic_count, mosaic_start};
        extractors.push_back(std::make_unique<StrikingFrameExtractor>(mosaic));
    }
    if (do_text) {
        extractors.push_back(std::make_unique<TextCropExtractor>());
    }

    Ffmpeg runner;
    bool all_ok = true;

    for (const auto& video : video_files) {
        std::string video_stem = video.stem().string();
        fs::path video_out_dir = output_dir / video_stem;
        fs::create_directories(video_out_dir);

        BOOST_LOG_TRIVIAL(info) << "=== Processing: " << video.filename().string() << " ===";

        // Run every mode regardless of earlier failures (no short-circuit).
        for (auto& ex : extractors) {
            bool ok = ex->run(runner, video, video_out_dir);
            if (!ok) all_ok = false;
        }
    }

    if (all_ok) {
        BOOST_LOG_TRIVIAL(info) << "Success! All frames extracted to: " << output_dir;
    } else {
        BOOST_LOG_TRIVIAL(info) << "Completed with some warnings. Check output in: " << output_dir;
    }

    return all_ok ? 0 : 1;
}

#ifndef TESTING_NO_MAIN
int main(int argc, char* argv[]) {
    ExtractApp app;
    return app.run(argc, argv);
}
#endif
