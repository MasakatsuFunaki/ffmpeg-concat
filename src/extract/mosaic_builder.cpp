#include "extract/mosaic_builder.h"

#include "extract/fingerprint_cache.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

bool MosaicBuilder::build(const fs::path& detect_dir,
                          const fs::path& mosaic_dir,
                          int mosaic_count,
                          int mosaic_start) const {
    fs::path mosaic_file = mosaic_dir / "mosaic.jpg";
    fs::path hash_file = mosaic_dir / ".mosaic_hash";

    // Build current fingerprint: detect images + mosaic count + start position
    std::string detect_fp = FingerprintCache::compute_dir_fingerprint(detect_dir, "striking_");
    std::string current_fp = "n=" + std::to_string(mosaic_count) +
                             ",p=" + std::to_string(mosaic_start) + "\n" + detect_fp;

    // Cache check: if mosaic exists and fingerprint matches, skip
    if (fs::exists(mosaic_file)) {
        std::string stored_fp = FingerprintCache::load(hash_file);
        if (!stored_fp.empty() && stored_fp == current_fp) {
            BOOST_LOG_TRIVIAL(info) << "  Mosaic cache hit — inputs unchanged, skipping regeneration";
            return true;
        }
        // Inputs changed → regenerate
        BOOST_LOG_TRIVIAL(info) << "  Mosaic inputs changed, regenerating...";
    }

    // Collect striking frames sorted by name (already time-ordered)
    std::vector<fs::path> striking_files;
    for (const auto& entry : fs::directory_iterator(detect_dir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".jpg" &&
            entry.path().filename().string().find("striking_") == 0) {
            striking_files.push_back(entry.path());
        }
    }
    std::sort(striking_files.begin(), striking_files.end());

    if (striking_files.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "  No striking frames found for mosaic";
        return false;
    }

    int total = static_cast<int>(striking_files.size());

    // Validate mosaic_start (1-indexed) is within range
    if (mosaic_start > total) {
        BOOST_LOG_TRIVIAL(error) << "  Error: --mosaic start position P=" << mosaic_start
                  << " is out of range (only " << total << " striking frame(s) available).";
        return false;
    }

    // Slice: only consider frames from position mosaic_start onward (1-indexed)
    std::vector<fs::path> subset(striking_files.begin() + (mosaic_start - 1),
                                 striking_files.end());
    int available = static_cast<int>(subset.size());

    if (available < mosaic_count) {
        BOOST_LOG_TRIVIAL(error) << "  Error: Not enough striking frames from position P=" << mosaic_start
                  << " for mosaic of N=" << mosaic_count
                  << " (only " << available << " frame(s) available from that position).";
        return false;
    }

    // Select mosaic_count images evenly distributed across the subset
    int n = mosaic_count;

    std::vector<cv::Mat> images;
    for (int i = 0; i < n; ++i) {
        int idx = (n == 1) ? 0
                : static_cast<int>(std::round(
                      static_cast<double>(i) * (available - 1) / (n - 1)));
        idx = std::min(idx, available - 1);
        cv::Mat img = cv::imread(subset[idx].string());
        if (!img.empty()) {
            images.push_back(img);
        }
    }

    if (images.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "  No images could be loaded for mosaic";
        return false;
    }

    // Mosaic canvas: HD resolution
    int canvas_w = 1920;
    int canvas_h = 1080;
    cv::Mat canvas(canvas_h, canvas_w, CV_8UC3, cv::Scalar(0, 0, 0));

    int actual_n = static_cast<int>(images.size());

    // Uniform grid: cols = ceil(sqrt(N)), rows = ceil(N/cols)
    // Last row images stretch wider to fill the full canvas width
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(actual_n))));
    int rows = (actual_n + cols - 1) / cols;
    int cell_h = canvas_h / rows;

    int img_idx = 0;
    for (int r = 0; r < rows; ++r) {
        int items_in_row = std::min(cols, actual_n - r * cols);
        int cell_w = canvas_w / items_in_row;
        int y = r * cell_h;
        int h = (r == rows - 1) ? (canvas_h - y) : cell_h;

        for (int c = 0; c < items_in_row; ++c) {
            int x = c * cell_w;
            int w = (c == items_in_row - 1) ? (canvas_w - x) : cell_w;

            cv::Mat cell;
            cv::resize(images[img_idx], cell, cv::Size(w, h));
            cell.copyTo(canvas(cv::Rect(x, y, w, h)));
            ++img_idx;
        }
    }

    fs::create_directories(mosaic_dir);
    std::string output_path = (mosaic_dir / "mosaic.jpg").string();
    bool ok = cv::imwrite(output_path, canvas);

    if (ok) {
        BOOST_LOG_TRIVIAL(info) << "  Created mosaic (" << actual_n
                  << " images) at " << output_path;
        FingerprintCache::save(hash_file, current_fp);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "  Failed to write mosaic to " << output_path;
    }

    return ok;
}
