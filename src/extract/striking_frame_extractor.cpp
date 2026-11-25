#include "extract/striking_frame_extractor.h"

#include "extract/crop_deduplicator.h"
#include "extract/fingerprint_cache.h"
#include "extract/frame_scorer.h"
#include "extract/mosaic_builder.h"
#include "ffmpeg/ffmpeg.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

StrikingFrameExtractor::StrikingFrameExtractor(std::optional<MosaicSpec> mosaic)
    : mosaic_(mosaic) {}

bool StrikingFrameExtractor::run(const Ffmpeg& ff, const fs::path& video,
                                 const fs::path& video_out_dir) {
    fs::path detect_dir = video_out_dir / "detect";
    fs::create_directories(detect_dir);

    if (!detect(ff, video, detect_dir)) {
        BOOST_LOG_TRIVIAL(warning) << "Warning: Failed to detect striking frames from "
                  << video.filename();
        return false;
    }

    if (mosaic_) {
        fs::path mosaic_dir = video_out_dir / "mosaics";
        MosaicBuilder builder;
        if (!builder.build(detect_dir, mosaic_dir, mosaic_->count, mosaic_->start)) {
            BOOST_LOG_TRIVIAL(warning) << "Warning: Failed to create mosaic from "
                      << video.filename();
            return false;
        }
    }

    return true;
}

bool StrikingFrameExtractor::detect(const Ffmpeg& ff, const fs::path& video_file,
                                    const fs::path& output_dir) {
    static constexpr int MAX_STRIKING  = 100;
    static constexpr int MAX_CANDIDATES = 500;

    // Cache check: if detect was previously run and output is intact, skip
    fs::path hash_file = output_dir / ".detect_hash";
    std::string existing_fp = FingerprintCache::compute_dir_fingerprint(output_dir, "striking_");
    if (!existing_fp.empty()) {
        std::string stored_fp = FingerprintCache::load(hash_file);
        if (!stored_fp.empty() && stored_fp == existing_fp) {
            BOOST_LOG_TRIVIAL(info) << "  Detect cache hit — striking frames unchanged, skipping extraction";
            return true;
        }
    }

    // Remove old striking files before re-extracting
    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".jpg" &&
            entry.path().filename().string().find("striking_") == 0) {
            std::error_code ec;
            fs::remove(entry.path(), ec);
        }
    }

    double duration = ff.duration(video_file);
    if (duration <= 0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine duration of " << video_file;
        return false;
    }

    // Sample at most MAX_CANDIDATES frames spread across the video
    double sample_interval = std::max(0.5, duration / MAX_CANDIDATES);
    int num_candidates = static_cast<int>(duration / sample_interval);
    if (num_candidates <= 0) num_candidates = 1;
    if (num_candidates > MAX_CANDIDATES) num_candidates = MAX_CANDIDATES;

    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(2)
              << "  Duration: " << duration << " s, sampling "
              << num_candidates << " candidate frames (interval: "
              << sample_interval << " s)";

    // Temp directory for candidate frames
    fs::path temp_dir = output_dir / "_temp_candidates";
    fs::create_directories(temp_dir);

    struct FrameInfo {
        fs::path path;
        double timestamp;
        double score;
    };
    std::vector<FrameInfo> candidates;

    for (int i = 0; i < num_candidates; ++i) {
        double timestamp = (i + 0.5) * sample_interval;
        if (timestamp >= duration) break;

        std::ostringstream name;
        name << "cand_" << std::setfill('0') << std::setw(4) << i << ".jpg";
        fs::path frame_path = temp_dir / name.str();

        if (ff.extract_frame(video_file, frame_path, timestamp)) {
            candidates.push_back({frame_path, timestamp, 0.0});
        }
    }

    BOOST_LOG_TRIVIAL(info) << "  Extracted " << candidates.size()
              << " candidate frames, scoring with OpenCV...";

    // Score each candidate frame for visual interestingness
    FrameScorer scorer;
    for (auto& cand : candidates) {
        cand.score = scorer.score(cand.path);
    }

    // Sort by score descending (most striking first)
    std::sort(candidates.begin(), candidates.end(),
              [](const FrameInfo& a, const FrameInfo& b) { return a.score > b.score; });

    // Keep top MAX_STRIKING, skipping near-duplicates
    CropDeduplicator dedup(0.85);
    std::vector<FrameInfo> selected;
    std::vector<cv::Mat> selected_images;
    for (const auto& cand : candidates) {
        if (static_cast<int>(selected.size()) >= MAX_STRIKING) break;
        cv::Mat img = cv::imread(cand.path.string());
        if (img.empty()) continue;
        if (!dedup.is_unique(img, selected_images))
            continue;
        selected.push_back(cand);
        selected_images.push_back(img);
    }
    int num_keep = static_cast<int>(selected.size());

    // Re-sort selected frames by timestamp for sequential naming
    std::sort(selected.begin(), selected.end(),
              [](const FrameInfo& a, const FrameInfo& b) { return a.timestamp < b.timestamp; });

    // Copy selected frames to output directory
    for (int i = 0; i < num_keep; ++i) {
        std::ostringstream name;
        name << "striking_" << std::setfill('0') << std::setw(4) << (i + 1) << ".jpg";
        fs::path dest = output_dir / name.str();

        std::error_code ec;
        fs::copy_file(selected[i].path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "  Warning: Failed to copy frame: " << ec.message();
        }
    }

    // Clean up temp directory
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    BOOST_LOG_TRIVIAL(info) << "  Selected " << num_keep << " striking frames (from "
              << candidates.size() << " candidates)";

    // Save fingerprint for cache validation on next run
    if (num_keep > 0) {
        std::string fp = FingerprintCache::compute_dir_fingerprint(output_dir, "striking_");
        FingerprintCache::save(hash_file, fp);
    }

    return num_keep > 0;
}
