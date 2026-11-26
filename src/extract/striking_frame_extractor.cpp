#include "extract/striking_frame_extractor.h"

#include "extract/crop_deduplicator.h"
#include "extract/fingerprint_cache.h"
#include "extract/frame_scorer.h"
#include "extract/mosaic_builder.h"
#include "core/format_util.h"
#include "core/scoped_handle.h"
#include "ffmpeg/ffmpeg.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

constexpr int kMaxStrikingFrames = 100;
constexpr int kMaxCandidates = 500;
constexpr double kMinSampleIntervalSec = 0.5;
constexpr double kIntervalCenterOffset = 0.5;
constexpr double kDedupSimilarityThreshold = 0.85;
constexpr int kIndexWidth = 4;
constexpr std::string_view kStrikingPrefix = "striking_";
constexpr std::string_view kCandidatePrefix = "cand_";
constexpr std::string_view kJpgExtension = ".jpg";

struct FrameInfo {
    fs::path path;
    double timestamp = 0.0;
    double score = 0.0;
};

}  // namespace

StrikingFrameExtractor::StrikingFrameExtractor(std::optional<MosaicSpec> mosaic) noexcept
    : mosaic_(mosaic) {}

bool StrikingFrameExtractor::run(const Ffmpeg& ff,
                                 const fs::path& video,
                                 const fs::path& video_out_dir) {
    const fs::path detect_dir = video_out_dir / "detect";
    fs::create_directories(detect_dir);

    if (!detect(ff, video, detect_dir)) {
        BOOST_LOG_TRIVIAL(warning)
            << "Warning: Failed to detect striking frames from " << video.filename();
        return false;
    }

    if (mosaic_) {
        const fs::path mosaic_dir = video_out_dir / "mosaics";
        const MosaicBuilder builder;
        if (!builder.build(detect_dir, mosaic_dir, mosaic_->count, mosaic_->start)) {
            BOOST_LOG_TRIVIAL(warning)
                << "Warning: Failed to create mosaic from " << video.filename();
            return false;
        }
    }

    return true;
}

bool StrikingFrameExtractor::detect(const Ffmpeg& ff,
                                    const fs::path& video_file,
                                    const fs::path& output_dir) const {
    const fs::path hash_file = output_dir / ".detect_hash";
    const std::string existing_fp =
        FingerprintCache::compute_dir_fingerprint(output_dir, kStrikingPrefix);
    if (!existing_fp.empty()) {
        const std::string stored_fp = FingerprintCache::load(hash_file);
        if (!stored_fp.empty() && stored_fp == existing_fp) {
            BOOST_LOG_TRIVIAL(info)
                << "  Detect cache hit — striking frames unchanged, skipping extraction";
            return true;
        }
    }

    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == kJpgExtension &&
            entry.path().filename().string().starts_with(kStrikingPrefix)) {
            std::error_code ec;
            fs::remove(entry.path(), ec);
        }
    }

    const double duration = ff.duration(video_file);
    if (duration <= 0.0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine duration of " << video_file;
        return false;
    }

    const double sample_interval =
        std::max(kMinSampleIntervalSec, duration / static_cast<double>(kMaxCandidates));
    const int num_candidates =
        std::clamp(static_cast<int>(duration / sample_interval), 1, kMaxCandidates);

    BOOST_LOG_TRIVIAL(info) << std::format(
        "  Duration: {:.2f} s, sampling {} candidate frames (interval: {:.2f} s)",
        duration, num_candidates, sample_interval);

    const fs::path temp_dir = output_dir / "_temp_candidates";
    fs::create_directories(temp_dir);
    const auto cleanup_temp = core::make_scope_guard([&temp_dir]() noexcept {
        try {
            std::error_code ec;
            fs::remove_all(temp_dir, ec);
        } catch (...) {
        }
    });

    std::vector<FrameInfo> candidates;
    candidates.reserve(static_cast<std::size_t>(num_candidates));

    for (const int i : std::views::iota(0, num_candidates)) {
        const double timestamp = (static_cast<double>(i) + kIntervalCenterOffset) * sample_interval;
        if (timestamp >= duration) {
            break;
        }

        const fs::path frame_path =
            temp_dir /
            core::format_zero_padded(kCandidatePrefix, i, kIndexWidth, kJpgExtension);

        if (ff.extract_frame(video_file, frame_path, timestamp)) {
            candidates.push_back(FrameInfo{frame_path, timestamp, 0.0});
        }
    }

    BOOST_LOG_TRIVIAL(info) << "  Extracted " << candidates.size()
                            << " candidate frames, scoring with OpenCV...";

    const FrameScorer scorer;
    for (auto& cand : candidates) {
        cand.score = scorer.score(cand.path);
    }

    std::ranges::sort(candidates, std::ranges::greater{}, &FrameInfo::score);

    const CropDeduplicator dedup(kDedupSimilarityThreshold);
    std::vector<FrameInfo> selected;
    std::vector<cv::Mat> selected_images;
    selected.reserve(static_cast<std::size_t>(kMaxStrikingFrames));
    selected_images.reserve(static_cast<std::size_t>(kMaxStrikingFrames));

    for (const auto& cand : candidates) {
        if (static_cast<int>(selected.size()) >= kMaxStrikingFrames) {
            break;
        }
        cv::Mat img = cv::imread(cand.path.string());
        if (img.empty()) {
            continue;
        }
        if (!dedup.is_unique(img, selected_images)) {
            continue;
        }
        selected.push_back(cand);
        selected_images.push_back(std::move(img));
    }

    std::ranges::sort(selected, {}, &FrameInfo::timestamp);

    for (const std::size_t i : std::views::iota(std::size_t{0}, selected.size())) {
        const fs::path dest =
            output_dir / core::format_zero_padded(
                             kStrikingPrefix, static_cast<int>(i) + 1, kIndexWidth, kJpgExtension);

        std::error_code ec;
        fs::copy_file(selected[i].path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "  Warning: Failed to copy frame: " << ec.message();
        }
    }

    BOOST_LOG_TRIVIAL(info) << "  Selected " << selected.size() << " striking frames (from "
                            << candidates.size() << " candidates)";

    if (!selected.empty()) {
        const std::string fp =
            FingerprintCache::compute_dir_fingerprint(output_dir, kStrikingPrefix);
        FingerprintCache::save(hash_file, fp);
    }

    return !selected.empty();
}
