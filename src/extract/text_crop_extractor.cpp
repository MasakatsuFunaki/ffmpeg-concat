#include "extract/text_crop_extractor.h"

#include "extract/crop_deduplicator.h"
#include "ffmpeg/ffmpeg.h"

#include <iomanip>
#include <sstream>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

TextCropExtractor::TextCropExtractor() {
    // Chain of Responsibility, evaluated in order (reject-and-stop).
    filters_.push_back(std::make_unique<ContrastFilter>());
    filters_.push_back(std::make_unique<EdgeDensityFilter>());
    filters_.push_back(std::make_unique<ComponentCountFilter>());
}

bool TextCropExtractor::run(const Ffmpeg& ff, const fs::path& video,
                            const fs::path& video_out_dir) {
    fs::path output_dir = video_out_dir / "text";
    fs::create_directories(output_dir);

    if (!extract(ff, video, output_dir)) {
        BOOST_LOG_TRIVIAL(warning) << "Warning: Failed to extract text frames from "
                  << video.filename();
        return false;
    }
    return true;
}

bool TextCropExtractor::extract(const Ffmpeg& ff, const fs::path& video_file,
                                const fs::path& output_dir) {
    static constexpr double SAMPLE_INTERVAL = 1.0; // sample every second

    double duration = ff.duration(video_file);
    if (duration <= 0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine duration of " << video_file;
        return false;
    }

    // Sample one frame every second
    double sample_interval = SAMPLE_INTERVAL;
    int num_candidates = static_cast<int>(duration / sample_interval);
    if (num_candidates <= 0) num_candidates = 1;

    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(2)
              << "  [text] Duration: " << duration << " s, sampling "
              << num_candidates << " frames for text detection";

    fs::path temp_dir = output_dir / "_temp_text";
    fs::create_directories(temp_dir);

    CropDeduplicator dedup(0.92);
    std::vector<cv::Mat> saved_crops;
    int text_count = 0;

    for (int i = 0; i < num_candidates; ++i) {
        double timestamp = (i + 0.5) * sample_interval;
        if (timestamp >= duration) break;

        std::ostringstream name;
        name << "tmp_" << std::setfill('0') << std::setw(4) << i << ".jpg";
        fs::path frame_path = temp_dir / name.str();

        if (!ff.extract_frame(video_file, frame_path, timestamp))
            continue;

        cv::Mat frame = cv::imread(frame_path.string());
        if (frame.empty()) continue;

        // Detect text regions in this frame
        std::vector<cv::Rect> text_rects = detector_.detect(frame);

        for (const auto& rect : text_rects) {
            // Clamp rect to image bounds
            cv::Rect safe_rect = rect & cv::Rect(0, 0, frame.cols, frame.rows);
            if (safe_rect.width < 20 || safe_rect.height < 10) continue;

            cv::Mat crop = frame(safe_rect).clone();

            // Reject crops that don't actually contain text-like content:
            // Real text has high contrast and lots of edges from character strokes.
            // The crop is converted to gray ONCE and passed through the filter chain,
            // which short-circuits on the first rejection (perf parity with the
            // original if(...) continue cascade).
            cv::Mat crop_gray;
            cv::cvtColor(crop, crop_gray, cv::COLOR_BGR2GRAY);

            bool passed = true;
            for (const auto& filter : filters_) {
                if (!filter->accept(crop_gray)) {
                    passed = false;
                    break;
                }
            }
            if (!passed) continue;

            // Check uniqueness against previously saved crops
            if (!dedup.is_unique(crop, saved_crops))
                continue;

            // Save the unique text crop
            ++text_count;
            std::ostringstream out_name;
            out_name << "text_" << std::setfill('0') << std::setw(4) << text_count << ".jpg";
            fs::path dest = output_dir / out_name.str();
            cv::imwrite(dest.string(), crop);
            saved_crops.push_back(crop);
        }

        // Delete temp frame to save disk space
        std::error_code ec;
        fs::remove(frame_path, ec);
    }

    // Clean up temp directory
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    BOOST_LOG_TRIVIAL(info) << "  [text] Extracted " << text_count << " unique text crops to " << output_dir;
    return text_count > 0;
}
