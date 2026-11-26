#include "extract/text_crop_extractor.h"

#include "extract/crop_deduplicator.h"
#include "extract/crop_filter.h"
#include "extract/image_io.h"
#include "extract/text_region_detector.h"
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
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr double kSampleIntervalSec = 1.0;
constexpr double kIntervalCenterOffset = 0.5;
constexpr double kDedupSimilarityThreshold = 0.92;
constexpr int kMinCropWidth = 20;
constexpr int kMinCropHeight = 10;
constexpr int kIndexWidth = 4;
constexpr std::size_t kFilterChainCount = 3;
constexpr std::size_t kMinBatchSize = 4;
constexpr std::size_t kBatchWorkerMultiplier = 2;
constexpr std::string_view kTextPrefix = "text_";
constexpr std::string_view kJpgExtension = ".jpg";

struct PendingCrop {
    fs::path dest;
    cv::Mat crop;
};

}  // namespace

struct TextCropExtractor::Impl {
    std::vector<std::unique_ptr<CropFilter>> filters;
    TextRegionDetector detector;

    Impl() {
        // Chain of Responsibility, evaluated in reject-and-stop order.
        filters.reserve(kFilterChainCount);
        filters.push_back(std::make_unique<ContrastFilter>());
        filters.push_back(std::make_unique<EdgeDensityFilter>());
        filters.push_back(std::make_unique<ComponentCountFilter>());
    }

    [[nodiscard]] bool extract(const Ffmpeg& ff,
                               const fs::path& video_file,
                               const fs::path& output_dir) const {
        core::ScopedProfile total_prof("extract.text.total");

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

        const int num_candidates =
            std::max(1, static_cast<int>(duration / kSampleIntervalSec));

        BOOST_LOG_TRIVIAL(info) << std::format(
            "  [text] Duration: {:.2f} s, sampling {} frames for text detection",
            duration, num_candidates);

        std::vector<double> timestamps;
        timestamps.reserve(static_cast<std::size_t>(num_candidates));
        for (const int i : std::views::iota(0, num_candidates)) {
            const double timestamp =
                (static_cast<double>(i) + kIntervalCenterOffset) * kSampleIntervalSec;
            if (timestamp >= duration) {
                break;
            }
            timestamps.push_back(timestamp);
        }

        if (timestamps.empty()) {
            BOOST_LOG_TRIVIAL(info)
                << "  [text] Extracted 0 unique text crops to " << core::to_utf8(output_dir);
            return false;
        }

        const std::size_t worker_count =
            std::min(timestamps.size(), core::ThreadPool::default_thread_count());
        core::ThreadPool pool(worker_count);
        const std::size_t batch_size =
            std::max(kMinBatchSize, pool.worker_count() * kBatchWorkerMultiplier);

        CropDeduplicator dedup(kDedupSimilarityThreshold);
        int text_count = 0;
        int written_count = 0;

        for (std::size_t offset = 0; offset < timestamps.size(); offset += batch_size) {
            const std::size_t count = std::min(batch_size, timestamps.size() - offset);
            const std::span<const double> batch_ts{timestamps.data() + offset, count};

            std::vector<av::FrameBuffer> decoded(count);
            {
                core::ScopedProfile decode_prof("extract.text.decode");
                [[maybe_unused]] const std::size_t got = sampler.sample_timestamps(
                    batch_ts, [&decoded](std::size_t local_idx, av::FrameBuffer&& fb) {
                        if (local_idx < decoded.size()) {
                            decoded[local_idx] = std::move(fb);
                        }
                    });
            }

            std::vector<std::vector<cv::Mat>> batch_crops(decoded.size());
            {
                core::ScopedProfile detect_prof("extract.text.detect_filter");
                pool.parallel_for(
                    decoded.size(), [this, &decoded, &batch_crops](std::size_t local_idx) {
                        const cv::Mat frame = extract_io::as_mat_view(decoded[local_idx]);
                        if (frame.empty()) {
                            return;
                        }

                        const std::vector<cv::Rect> text_rects = detector.detect(frame);
                        const cv::Rect frame_bounds(0, 0, frame.cols, frame.rows);

                        std::vector<cv::Mat> passing;
                        passing.reserve(text_rects.size());
                        for (const auto& rect : text_rects) {
                            const cv::Rect safe_rect = rect & frame_bounds;
                            if (safe_rect.width < kMinCropWidth ||
                                safe_rect.height < kMinCropHeight) {
                                continue;
                            }

                            cv::Mat crop = frame(safe_rect).clone();
                            cv::Mat crop_gray;
                            cv::cvtColor(crop, crop_gray, cv::COLOR_BGR2GRAY);

                            const bool passed = std::ranges::all_of(
                                filters,
                                [&crop_gray](const std::unique_ptr<CropFilter>& filter) {
                                    return filter->accept(crop_gray);
                                });
                            if (!passed) {
                                continue;
                            }

                            passing.push_back(std::move(crop));
                        }
                        batch_crops[local_idx] = std::move(passing);
                    });
            }

            std::vector<PendingCrop> accepted;
            {
                core::ScopedProfile dedup_prof("extract.text.dedup");
                for (auto& frame_crops : batch_crops) {
                    for (auto& crop : frame_crops) {
                        if (!dedup.try_add(crop)) {
                            continue;
                        }
                        ++text_count;
                        fs::path dest =
                            output_dir / core::format_zero_padded(kTextPrefix,
                                                                  text_count,
                                                                  kIndexWidth,
                                                                  kJpgExtension);
                        accepted.push_back(PendingCrop{std::move(dest), std::move(crop)});
                    }
                }
            }

            if (accepted.empty()) {
                continue;
            }

            std::vector<std::uint8_t> write_ok(accepted.size(), 0U);
            {
                core::ScopedProfile write_prof("extract.text.write");
                pool.parallel_for(accepted.size(), [&accepted, &write_ok](std::size_t i) {
                    write_ok[i] =
                        extract_io::write_jpeg(accepted[i].dest, accepted[i].crop) ? 1U : 0U;
                });
            }

            for (const std::size_t i : std::views::iota(std::size_t{0}, write_ok.size())) {
                if (write_ok[i] != 0U) {
                    ++written_count;
                } else {
                    BOOST_LOG_TRIVIAL(warning)
                        << "  [text] Warning: Failed to write crop "
                        << core::to_utf8(accepted[i].dest);
                }
            }
        }

        BOOST_LOG_TRIVIAL(info) << "  [text] Extracted " << written_count
                                << " unique text crops to " << core::to_utf8(output_dir);
        return written_count > 0;
    }
};

TextCropExtractor::TextCropExtractor() : impl_(std::make_unique<Impl>()) {}
TextCropExtractor::~TextCropExtractor() = default;
TextCropExtractor::TextCropExtractor(TextCropExtractor&&) noexcept = default;
TextCropExtractor& TextCropExtractor::operator=(TextCropExtractor&&) noexcept = default;

bool TextCropExtractor::run(const Ffmpeg& ff,
                            const fs::path& video,
                            const fs::path& video_out_dir) {
    if (!impl_) {
        return false;
    }

    const fs::path output_dir = video_out_dir / "text";
    fs::create_directories(output_dir);

    if (!impl_->extract(ff, video, output_dir)) {
        BOOST_LOG_TRIVIAL(warning)
            << "Warning: Failed to extract text frames from "
            << core::to_utf8(video.filename());
        return false;
    }
    return true;
}
