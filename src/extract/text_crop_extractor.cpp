#include "extract/text_crop_extractor.h"

#include "extract/crop_deduplicator.h"
#include "extract/crop_filter.h"
#include "extract/text_region_detector.h"
#include "core/format_util.h"
#include "core/scoped_handle.h"
#include "ffmpeg/ffmpeg.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr double kSampleIntervalSec = 1.0;
constexpr double kIntervalCenterOffset = 0.5;
constexpr double kDedupSimilarityThreshold = 0.92;
constexpr int kMinCropWidth = 20;
constexpr int kMinCropHeight = 10;
constexpr int kIndexWidth = 4;
constexpr std::size_t kFilterChainCount = 3;
constexpr std::string_view kTempPrefix = "tmp_";
constexpr std::string_view kTextPrefix = "text_";
constexpr std::string_view kJpgExtension = ".jpg";

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
        const double duration = ff.duration(video_file);
        if (duration <= 0.0) {
            BOOST_LOG_TRIVIAL(error) << "Error: Could not determine duration of " << video_file;
            return false;
        }

        const int num_candidates =
            std::max(1, static_cast<int>(duration / kSampleIntervalSec));

        BOOST_LOG_TRIVIAL(info) << std::format(
            "  [text] Duration: {:.2f} s, sampling {} frames for text detection",
            duration, num_candidates);

        const fs::path temp_dir = output_dir / "_temp_text";
        fs::create_directories(temp_dir);
        const auto cleanup_temp = core::make_scope_guard([&temp_dir]() noexcept {
            try {
                std::error_code ec;
                fs::remove_all(temp_dir, ec);
            } catch (...) {
            }
        });

        const CropDeduplicator dedup(kDedupSimilarityThreshold);
        std::vector<cv::Mat> saved_crops;
        int text_count = 0;

        for (const int i : std::views::iota(0, num_candidates)) {
            const double timestamp =
                (static_cast<double>(i) + kIntervalCenterOffset) * kSampleIntervalSec;
            if (timestamp >= duration) {
                break;
            }

            const fs::path frame_path =
                temp_dir /
                core::format_zero_padded(kTempPrefix, i, kIndexWidth, kJpgExtension);

            if (!ff.extract_frame(video_file, frame_path, timestamp)) {
                continue;
            }

            const auto cleanup_frame = core::make_scope_guard([&frame_path]() noexcept {
                try {
                    std::error_code ec;
                    fs::remove(frame_path, ec);
                } catch (...) {
                }
            });

            const cv::Mat frame = cv::imread(frame_path.string());
            if (frame.empty()) {
                continue;
            }

            const std::vector<cv::Rect> text_rects = detector.detect(frame);
            const cv::Rect frame_bounds(0, 0, frame.cols, frame.rows);

            for (const auto& rect : text_rects) {
                const cv::Rect safe_rect = rect & frame_bounds;
                if (safe_rect.width < kMinCropWidth || safe_rect.height < kMinCropHeight) {
                    continue;
                }

                cv::Mat crop = frame(safe_rect).clone();
                cv::Mat crop_gray;
                cv::cvtColor(crop, crop_gray, cv::COLOR_BGR2GRAY);

                const bool passed = std::ranges::all_of(
                    filters, [&crop_gray](const std::unique_ptr<CropFilter>& filter) {
                        return filter->accept(crop_gray);
                    });
                if (!passed) {
                    continue;
                }

                if (!dedup.is_unique(crop, saved_crops)) {
                    continue;
                }

                ++text_count;
                const fs::path dest =
                    output_dir /
                    core::format_zero_padded(kTextPrefix, text_count, kIndexWidth, kJpgExtension);
                cv::imwrite(dest.string(), crop);
                saved_crops.push_back(std::move(crop));
            }
        }

        BOOST_LOG_TRIVIAL(info) << "  [text] Extracted " << text_count
                                << " unique text crops to " << output_dir;
        return text_count > 0;
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
            << "Warning: Failed to extract text frames from " << video.filename();
        return false;
    }
    return true;
}
