#include "extract/striking_frame_extractor.h"

#include "extract/crop_deduplicator.h"
#include "extract/fingerprint_cache.h"
#include "extract/frame_scorer.h"
#include "extract/image_io.h"
#include "extract/mosaic_builder.h"
#include "av/frame_sampler.h"
#include "core/format_util.h"
#include "core/path_util.h"
#include "core/profiler.h"
#include "core/thread_pool.h"
#include "ffmpeg/ffmpeg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr int kMaxStrikingFrames = 100;
constexpr std::size_t kSinglePassMaxCandidates = static_cast<std::size_t>(kMaxStrikingFrames);
constexpr int kMaxCandidates = 500;
constexpr double kMinSampleIntervalSec = 0.5;
constexpr double kIntervalCenterOffset = 0.5;
constexpr double kDedupSimilarityThreshold = 0.85;
constexpr int kIndexWidth = 4;
constexpr std::size_t kMinBoundedBatchSize = 8;
constexpr std::size_t kBatchWorkerMultiplier = 2;
constexpr int kThumbnailWidth = 128;
constexpr int kThumbnailHeight = 64;
constexpr int kChannelsGray = 1;
constexpr std::string_view kStrikingPrefix = "striking_";
constexpr std::string_view kJpgExtension = ".jpg";

struct InMemoryCandidate {
    av::FrameBuffer frame;
    double timestamp = 0.0;
    double score = 0.0;
};

struct ScoredThumbnailCandidate {
    cv::Mat thumbnail;  // 128x64 CV_8UC1 (8 KB)
    double timestamp = 0.0;
    double score = 0.0;
};

struct PassOutcome {
    std::size_t candidate_count = 0;
    std::size_t written_count = 0;
};

[[nodiscard]] cv::Mat make_dedup_thumbnail(const cv::Mat& frame) {
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0) {
        return {};
    }
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(kThumbnailWidth, kThumbnailHeight));
    if (resized.channels() == kChannelsGray) {
        return resized;
    }
    cv::Mat gray;
    cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

[[nodiscard]] PassOutcome run_single_pass(av::FrameSampler& sampler,
                                          std::span<const double> timestamps,
                                          const fs::path& output_dir,
                                          core::ThreadPool& pool) {
    std::vector<InMemoryCandidate> candidates;
    candidates.reserve(timestamps.size());

    {
        core::ScopedProfile decode_prof("extract.striking.decode");
        [[maybe_unused]] const std::size_t got = sampler.sample_timestamps(
            timestamps, [&candidates, timestamps](std::size_t idx, av::FrameBuffer&& fb) {
                if (idx < timestamps.size() && !fb.empty()) {
                    candidates.push_back(InMemoryCandidate{std::move(fb), timestamps[idx], 0.0});
                }
            });
    }

    BOOST_LOG_TRIVIAL(info) << "  Extracted " << candidates.size()
                            << " candidate frames, scoring with OpenCV...";
    if (candidates.empty()) {
        return {};
    }

    const FrameScorer scorer;
    {
        core::ScopedProfile score_prof("extract.striking.score");
        pool.parallel_for(candidates.size(), [&candidates, &scorer](std::size_t i) {
            const cv::Mat view = extract_io::as_mat_view(candidates[i].frame);
            candidates[i].score = scorer.score(view);
        });
    }

    std::ranges::sort(candidates,
                      [](const InMemoryCandidate& a, const InMemoryCandidate& b) noexcept {
                          if (a.score != b.score) {
                              return a.score > b.score;
                          }
                          return a.timestamp < b.timestamp;
                      });

    std::vector<std::size_t> selected_indices;
    selected_indices.reserve(
        std::min(candidates.size(), static_cast<std::size_t>(kMaxStrikingFrames)));

    {
        core::ScopedProfile dedup_prof("extract.striking.dedup");
        CropDeduplicator dedup(kDedupSimilarityThreshold);
        for (const std::size_t i : std::views::iota(std::size_t{0}, candidates.size())) {
            if (static_cast<int>(selected_indices.size()) >= kMaxStrikingFrames) {
                break;
            }
            const cv::Mat view = extract_io::as_mat_view(candidates[i].frame);
            if (view.empty()) {
                continue;
            }
            if (dedup.try_add(view)) {
                selected_indices.push_back(i);
            }
        }
    }

    std::ranges::sort(selected_indices, [&candidates](std::size_t a, std::size_t b) noexcept {
        return candidates[a].timestamp < candidates[b].timestamp;
    });

    std::vector<std::uint8_t> write_ok(selected_indices.size(), 0U);
    {
        core::ScopedProfile write_prof("extract.striking.write");
        pool.parallel_for(
            selected_indices.size(),
            [&candidates, &selected_indices, &output_dir, &write_ok](std::size_t i) {
                const fs::path dest =
                    output_dir / core::format_zero_padded(kStrikingPrefix,
                                                          static_cast<int>(i) + 1,
                                                          kIndexWidth,
                                                          kJpgExtension);
                const cv::Mat view =
                    extract_io::as_mat_view(candidates[selected_indices[i]].frame);
                write_ok[i] = extract_io::write_jpeg(dest, view) ? 1U : 0U;
            });
    }

    std::size_t written_count = 0;
    for (const std::size_t i : std::views::iota(std::size_t{0}, write_ok.size())) {
        if (write_ok[i] != 0U) {
            ++written_count;
        } else {
            BOOST_LOG_TRIVIAL(warning) << std::format(
                "  Warning: Failed to write striking frame {:04d}", static_cast<int>(i) + 1);
        }
    }

    return PassOutcome{candidates.size(), written_count};
}

[[nodiscard]] PassOutcome run_bounded_pass(av::FrameSampler& sampler,
                                           std::span<const double> timestamps,
                                           const fs::path& output_dir,
                                           core::ThreadPool& pool) {
    const std::size_t batch_size =
        std::max(kMinBoundedBatchSize, pool.worker_count() * kBatchWorkerMultiplier);
    const FrameScorer scorer;

    std::vector<ScoredThumbnailCandidate> candidates;
    candidates.reserve(timestamps.size());

    for (std::size_t offset = 0; offset < timestamps.size(); offset += batch_size) {
        const std::size_t count = std::min(batch_size, timestamps.size() - offset);
        const std::span<const double> batch_ts = timestamps.subspan(offset, count);

        std::vector<std::pair<double, av::FrameBuffer>> batch_frames;
        batch_frames.reserve(count);
        {
            core::ScopedProfile decode_prof("extract.striking.decode");
            [[maybe_unused]] const std::size_t got = sampler.sample_timestamps(
                batch_ts, [&batch_frames, batch_ts](std::size_t idx, av::FrameBuffer&& fb) {
                    if (idx < batch_ts.size() && !fb.empty()) {
                        batch_frames.emplace_back(batch_ts[idx], std::move(fb));
                    }
                });
        }
        if (batch_frames.empty()) {
            continue;
        }

        std::vector<ScoredThumbnailCandidate> batch_scored(batch_frames.size());
        {
            core::ScopedProfile score_prof("extract.striking.score");
            pool.parallel_for(
                batch_frames.size(), [&batch_frames, &batch_scored, &scorer](std::size_t i) {
                    const cv::Mat view = extract_io::as_mat_view(batch_frames[i].second);
                    batch_scored[i] = ScoredThumbnailCandidate{
                        make_dedup_thumbnail(view),
                        batch_frames[i].first,
                        scorer.score(view),
                    };
                });
        }

        for (auto& entry : batch_scored) {
            candidates.push_back(std::move(entry));
        }
    }

    BOOST_LOG_TRIVIAL(info) << "  Extracted " << candidates.size()
                            << " candidate frames, scored with OpenCV";
    if (candidates.empty()) {
        return {};
    }

    std::ranges::sort(candidates,
                      [](const ScoredThumbnailCandidate& a,
                         const ScoredThumbnailCandidate& b) noexcept {
                          if (a.score != b.score) {
                              return a.score > b.score;
                          }
                          return a.timestamp < b.timestamp;
                      });

    std::vector<double> selected_timestamps;
    selected_timestamps.reserve(static_cast<std::size_t>(kMaxStrikingFrames));
    {
        core::ScopedProfile dedup_prof("extract.striking.dedup");
        CropDeduplicator dedup(kDedupSimilarityThreshold);
        for (const auto& cand : candidates) {
            if (static_cast<int>(selected_timestamps.size()) >= kMaxStrikingFrames) {
                break;
            }
            if (cand.thumbnail.empty()) {
                continue;
            }
            if (dedup.try_add(cand.thumbnail)) {
                selected_timestamps.push_back(cand.timestamp);
            }
        }
    }

    std::ranges::sort(selected_timestamps);

    int next_output_index = 1;
    std::size_t written_count = 0;

    for (std::size_t offset = 0; offset < selected_timestamps.size(); offset += batch_size) {
        const std::size_t count = std::min(batch_size, selected_timestamps.size() - offset);
        const std::span<const double> chunk_ts{selected_timestamps.data() + offset, count};

        std::vector<av::FrameBuffer> decoded;
        decoded.reserve(count);
        {
            core::ScopedProfile decode_prof("extract.striking.decode");
            [[maybe_unused]] const std::size_t got = sampler.sample_timestamps(
                chunk_ts, [&decoded](std::size_t, av::FrameBuffer&& fb) {
                    if (!fb.empty()) {
                        decoded.push_back(std::move(fb));
                    }
                });
        }
        if (decoded.empty()) {
            continue;
        }

        const int base_index = next_output_index;
        next_output_index += static_cast<int>(decoded.size());

        std::vector<std::uint8_t> write_ok(decoded.size(), 0U);
        {
            core::ScopedProfile write_prof("extract.striking.write");
            pool.parallel_for(
                decoded.size(), [&decoded, &output_dir, base_index, &write_ok](std::size_t i) {
                    const fs::path dest =
                        output_dir / core::format_zero_padded(kStrikingPrefix,
                                                              base_index + static_cast<int>(i),
                                                              kIndexWidth,
                                                              kJpgExtension);
                    const cv::Mat view = extract_io::as_mat_view(decoded[i]);
                    write_ok[i] = extract_io::write_jpeg(dest, view) ? 1U : 0U;
                });
        }

        for (const std::size_t i : std::views::iota(std::size_t{0}, write_ok.size())) {
            if (write_ok[i] != 0U) {
                ++written_count;
            } else {
                BOOST_LOG_TRIVIAL(warning) << std::format(
                    "  Warning: Failed to write striking frame {:04d}",
                    base_index + static_cast<int>(i));
            }
        }
    }

    return PassOutcome{candidates.size(), written_count};
}

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
            << "Warning: Failed to detect striking frames from "
            << core::to_utf8(video.filename());
        return false;
    }

    if (mosaic_) {
        const fs::path mosaic_dir = video_out_dir / "mosaics";
        const MosaicBuilder builder;
        if (!builder.build(detect_dir, mosaic_dir, mosaic_->count, mosaic_->start)) {
            BOOST_LOG_TRIVIAL(warning)
                << "Warning: Failed to create mosaic from " << core::to_utf8(video.filename());
            return false;
        }
    }

    return true;
}

bool StrikingFrameExtractor::detect(const Ffmpeg& ff,
                                    const fs::path& video_file,
                                    const fs::path& output_dir) const {
    core::ScopedProfile total_prof("extract.striking.total");

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
            core::to_utf8(entry.path().filename()).starts_with(kStrikingPrefix)) {
            std::error_code ec;
            fs::remove(entry.path(), ec);
        }
    }

    av::FrameSampler sampler;
    if (!sampler.open(video_file)) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Could not open video " << core::to_utf8(video_file);
        return false;
    }

    double duration = sampler.duration_sec();
    if (duration <= 0.0) {
        duration = ff.duration(video_file);
    }
    if (duration <= 0.0) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Could not determine duration of " << core::to_utf8(video_file);
        return false;
    }

    const double sample_interval =
        std::max(kMinSampleIntervalSec, duration / static_cast<double>(kMaxCandidates));
    const int num_candidates =
        std::clamp(static_cast<int>(duration / sample_interval), 1, kMaxCandidates);

    BOOST_LOG_TRIVIAL(info) << std::format(
        "  Duration: {:.2f} s, sampling {} candidate frames (interval: {:.2f} s)",
        duration, num_candidates, sample_interval);

    std::vector<double> timestamps;
    timestamps.reserve(static_cast<std::size_t>(num_candidates));
    for (const int i : std::views::iota(0, num_candidates)) {
        const double timestamp =
            (static_cast<double>(i) + kIntervalCenterOffset) * sample_interval;
        if (timestamp >= duration) {
            break;
        }
        timestamps.push_back(timestamp);
    }

    if (timestamps.empty()) {
        return false;
    }

    const std::size_t worker_count =
        std::min(timestamps.size(), core::ThreadPool::default_thread_count());
    core::ThreadPool pool(worker_count);

    const PassOutcome outcome =
        (timestamps.size() <= kSinglePassMaxCandidates)
            ? run_single_pass(sampler, timestamps, output_dir, pool)
            : run_bounded_pass(sampler, timestamps, output_dir, pool);

    BOOST_LOG_TRIVIAL(info) << "  Selected " << outcome.written_count << " striking frames (from "
                            << outcome.candidate_count << " candidates)";

    if (outcome.written_count > 0) {
        const std::string fp =
            FingerprintCache::compute_dir_fingerprint(output_dir, kStrikingPrefix);
        FingerprintCache::save(hash_file, fp);
    }

    return outcome.written_count > 0;
}
