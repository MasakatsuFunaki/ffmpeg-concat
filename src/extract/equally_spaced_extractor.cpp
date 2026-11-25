#include "extract/equally_spaced_extractor.h"

#include "ffmpeg/ffmpeg.h"

#include <iomanip>
#include <sstream>

#include <boost/log/trivial.hpp>

EquallySpacedExtractor::EquallySpacedExtractor(int num_samples)
    : num_samples_(num_samples) {}

bool EquallySpacedExtractor::run(const Ffmpeg& ff, const fs::path& video,
                                 const fs::path& video_out_dir) {
    fs::path output_dir = video_out_dir / "image";
    fs::create_directories(output_dir);

    if (!extract(ff, video, output_dir)) {
        BOOST_LOG_TRIVIAL(warning) << "Warning: Failed to extract equally-spaced frames from "
                  << video.filename();
        return false;
    }
    return true;
}

bool EquallySpacedExtractor::extract(const Ffmpeg& ff, const fs::path& video_file,
                                     const fs::path& output_dir) {
    double duration = ff.duration(video_file);
    if (duration <= 0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine duration of " << video_file;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(2)
              << "  Duration: " << duration << " s, extracting "
              << num_samples_ << " equally-spaced frames";

    double interval = duration / num_samples_;
    bool all_ok = true;

    for (int i = 0; i < num_samples_; ++i) {
        double timestamp = (i + 0.5) * interval;  // center of each interval
        if (timestamp >= duration) timestamp = duration - 0.01;

        std::ostringstream name;
        name << "frame_" << std::setfill('0') << std::setw(4) << (i + 1) << ".jpg";
        fs::path output_file = output_dir / name.str();

        if (!ff.extract_frame(video_file, output_file, timestamp)) {
            BOOST_LOG_TRIVIAL(warning) << "  Warning: Failed to extract frame at " << timestamp << " s";
            all_ok = false;
        }
    }

    int count = 0;
    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (entry.is_regular_file()) count++;
    }
    BOOST_LOG_TRIVIAL(info) << "  Extracted " << count << " frames to " << output_dir;

    return all_ok;
}
