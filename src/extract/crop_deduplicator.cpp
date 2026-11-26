#include "extract/crop_deduplicator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

constexpr int kCompareWidth = 128;
constexpr int kCompareHeight = 64;
constexpr std::size_t kThumbnailPixels =
    static_cast<std::size_t>(kCompareWidth) * static_cast<std::size_t>(kCompareHeight);
constexpr int kChannelsGray = 1;
constexpr int kChannelsBgr = 3;
constexpr int kChannelsBgra = 4;
constexpr double kFlatNormEpsilon = 1e-5;
constexpr std::size_t kDotUnrollWidth = 4;

static_assert(kThumbnailPixels % kDotUnrollWidth == 0);

struct Thumbnail {
    cv::Mat gray;             // Contiguous 128x64 CV_8UC1 for flat-image fallback.
    std::vector<float> unit;  // Centered L2-unit-normalized pixels (empty iff is_flat).
    bool is_flat = false;
};

[[nodiscard]] cv::Mat to_gray_thumbnail(const cv::Mat& image) {
    if (image.empty() || image.cols <= 0 || image.rows <= 0 || image.depth() != CV_8U) {
        return {};
    }

    const int ch = image.channels();
    if (ch != kChannelsGray && ch != kChannelsBgr && ch != kChannelsBgra) {
        return {};
    }

    cv::Mat gray;
    if (image.cols == kCompareWidth && image.rows == kCompareHeight) {
        if (ch == kChannelsGray) {
            gray = image.clone();
        } else if (ch == kChannelsBgr) {
            cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        } else {
            cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
        }
    } else {
        cv::Mat resized;
        cv::resize(image, resized, cv::Size(kCompareWidth, kCompareHeight));
        if (ch == kChannelsGray) {
            gray = std::move(resized);
        } else if (ch == kChannelsBgr) {
            cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
        } else {
            cv::cvtColor(resized, gray, cv::COLOR_BGRA2GRAY);
        }
    }

    if (!gray.isContinuous()) {
        gray = gray.clone();
    }
    return gray;
}

[[nodiscard]] std::optional<Thumbnail> build_thumbnail(const cv::Mat& image) {
    cv::Mat gray = to_gray_thumbnail(image);
    if (gray.empty()) {
        return std::nullopt;
    }

    const auto* pixels = gray.ptr<std::uint8_t>(0);
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < kThumbnailPixels; ++i) {
        sum += pixels[i];
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(kThumbnailPixels);

    Thumbnail thumb;
    thumb.unit.resize(kThumbnailPixels);
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < kThumbnailPixels; ++i) {
        const double centered = static_cast<double>(pixels[i]) - mean;
        sum_sq += centered * centered;
        thumb.unit[i] = static_cast<float>(centered);
    }

    const double norm = std::sqrt(sum_sq);
    if (norm < kFlatNormEpsilon) {
        thumb.is_flat = true;
        thumb.unit.clear();
    } else {
        thumb.is_flat = false;
        const float inv_norm = static_cast<float>(1.0 / norm);
        for (float& value : thumb.unit) {
            value *= inv_norm;
        }
    }
    thumb.gray = std::move(gray);
    return thumb;
}

[[nodiscard]] double compute_similarity(const Thumbnail& a, const Thumbnail& b) {
    if (a.is_flat || b.is_flat) {
        cv::Mat result;
        cv::matchTemplate(a.gray, b.gray, result, cv::TM_CCOEFF_NORMED);
        return static_cast<double>(result.at<float>(0, 0));
    }

    const float* pa = a.unit.data();
    const float* pb = b.unit.data();
    double acc0 = 0.0;
    double acc1 = 0.0;
    double acc2 = 0.0;
    double acc3 = 0.0;

    for (std::size_t i = 0; i < kThumbnailPixels; i += kDotUnrollWidth) {
        acc0 += static_cast<double>(pa[i]) * static_cast<double>(pb[i]);
        acc1 += static_cast<double>(pa[i + 1]) * static_cast<double>(pb[i + 1]);
        acc2 += static_cast<double>(pa[i + 2]) * static_cast<double>(pb[i + 2]);
        acc3 += static_cast<double>(pa[i + 3]) * static_cast<double>(pb[i + 3]);
    }
    return (acc0 + acc1) + (acc2 + acc3);
}

}  // namespace

struct CropDeduplicator::Impl {
    std::vector<Thumbnail> thumbnails;
};

CropDeduplicator::CropDeduplicator(double threshold) noexcept
    : threshold_(threshold), impl_(nullptr) {}

CropDeduplicator::~CropDeduplicator() = default;
CropDeduplicator::CropDeduplicator(CropDeduplicator&&) noexcept = default;
CropDeduplicator& CropDeduplicator::operator=(CropDeduplicator&&) noexcept = default;

bool CropDeduplicator::try_add(const cv::Mat& crop) {
    auto candidate = build_thumbnail(crop);
    if (!candidate) {
        return false;
    }

    if (impl_) {
        const bool duplicate = std::ranges::any_of(
            impl_->thumbnails, [this, &candidate](const Thumbnail& saved) {
                return compute_similarity(*candidate, saved) >= threshold_;
            });
        if (duplicate) {
            return false;
        }
    } else {
        impl_ = std::make_unique<Impl>();
    }

    impl_->thumbnails.push_back(std::move(*candidate));
    return true;
}

void CropDeduplicator::clear_cache() noexcept {
    if (impl_) {
        impl_->thumbnails.clear();
    }
}

bool CropDeduplicator::is_unique(const cv::Mat& crop,
                                 std::span<const cv::Mat> saved_crops) const {
    if (crop.empty() || crop.cols <= 0 || crop.rows <= 0 || crop.depth() != CV_8U) {
        return false;
    }
    const int ch = crop.channels();
    if (ch != kChannelsGray && ch != kChannelsBgr && ch != kChannelsBgra) {
        return false;
    }
    if (saved_crops.empty()) {
        return true;
    }

    const auto candidate = build_thumbnail(crop);
    if (!candidate) {
        return false;
    }

    const auto is_too_similar = [this, &candidate](const cv::Mat& saved) {
        const auto saved_thumb = build_thumbnail(saved);
        if (!saved_thumb) {
            return false;
        }
        return compute_similarity(*candidate, *saved_thumb) >= threshold_;
    };

    return std::ranges::none_of(saved_crops, is_too_similar);
}

bool CropDeduplicator::is_unique_against_cached(const cv::Mat& crop) const {
    const auto candidate = build_thumbnail(crop);
    if (!candidate) {
        return false;
    }
    if (!impl_ || impl_->thumbnails.empty()) {
        return true;
    }

    return std::ranges::none_of(
        impl_->thumbnails, [this, &candidate](const Thumbnail& saved) {
            return compute_similarity(*candidate, saved) >= threshold_;
        });
}

std::size_t CropDeduplicator::cached_count() const noexcept {
    return impl_ ? impl_->thumbnails.size() : std::size_t{0};
}
