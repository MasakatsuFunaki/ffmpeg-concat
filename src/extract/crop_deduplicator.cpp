#include "extract/crop_deduplicator.h"

#include <algorithm>
#include <ranges>

#include <opencv2/imgproc.hpp>

namespace {

constexpr int kCompareWidth = 128;
constexpr int kCompareHeight = 64;
constexpr int kChannelsGray = 1;
constexpr int kChannelsBgr = 3;
constexpr int kChannelsBgra = 4;

[[nodiscard]] cv::Mat to_gray_thumbnail(const cv::Mat& image) {
    if (image.empty() || image.cols <= 0 || image.rows <= 0) {
        return {};
    }

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(kCompareWidth, kCompareHeight));

    if (resized.channels() == kChannelsGray) {
        return resized;
    }

    cv::Mat gray;
    if (resized.channels() == kChannelsBgr) {
        cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
        return gray;
    }
    if (resized.channels() == kChannelsBgra) {
        cv::cvtColor(resized, gray, cv::COLOR_BGRA2GRAY);
        return gray;
    }
    return {};
}

}  // namespace

CropDeduplicator::CropDeduplicator(double threshold) noexcept : threshold_(threshold) {}

bool CropDeduplicator::is_unique(const cv::Mat& crop,
                                 std::span<const cv::Mat> saved_crops) const {
    if (crop.empty() || crop.cols <= 0 || crop.rows <= 0) {
        return false;
    }
    if (saved_crops.empty()) {
        return true;
    }

    const cv::Mat gray_new = to_gray_thumbnail(crop);
    if (gray_new.empty()) {
        return false;
    }

    const auto is_too_similar = [this, &gray_new](const cv::Mat& saved) {
        const cv::Mat gray_saved = to_gray_thumbnail(saved);
        if (gray_saved.empty() || gray_saved.type() != gray_new.type()) {
            return false;
        }

        cv::Mat result;
        cv::matchTemplate(gray_new, gray_saved, result, cv::TM_CCOEFF_NORMED);
        const double similarity = static_cast<double>(result.at<float>(0, 0));
        return similarity >= threshold_;
    };

    return std::ranges::none_of(saved_crops, is_too_similar);
}
