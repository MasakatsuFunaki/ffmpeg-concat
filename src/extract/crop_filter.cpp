#include "extract/crop_filter.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr double kMinContrastStddev = 25.0;
constexpr double kCannyLowThreshold = 80.0;
constexpr double kCannyHighThreshold = 160.0;
constexpr double kMinEdgeRatio = 0.05;
constexpr double kOtsuThreshold = 0.0;
constexpr double kBinaryMaxValue = 255.0;
constexpr double kWidthNormalizationPx = 100.0;
constexpr double kMinComponentsPer100Px = 1.0;

[[nodiscard]] bool is_non_empty_crop(const cv::Mat& crop_gray) noexcept {
    return !crop_gray.empty() && crop_gray.cols > 0 && crop_gray.rows > 0;
}

}  // namespace

CropFilter::~CropFilter() = default;

bool ContrastFilter::accept(const cv::Mat& crop_gray) const {
    if (!is_non_empty_crop(crop_gray)) {
        return false;
    }

    cv::Scalar mean_val;
    cv::Scalar stddev_val;
    cv::meanStdDev(crop_gray, mean_val, stddev_val);
    return stddev_val.val[0] >= kMinContrastStddev;
}

bool EdgeDensityFilter::accept(const cv::Mat& crop_gray) const {
    if (!is_non_empty_crop(crop_gray)) {
        return false;
    }

    cv::Mat edges;
    cv::Canny(crop_gray, edges, kCannyLowThreshold, kCannyHighThreshold);
    const double total_pixels =
        static_cast<double>(edges.rows) * static_cast<double>(edges.cols);
    if (total_pixels <= 0.0) {
        return false;
    }

    const double edge_ratio = static_cast<double>(cv::countNonZero(edges)) / total_pixels;
    return edge_ratio >= kMinEdgeRatio;
}

bool ComponentCountFilter::accept(const cv::Mat& crop_gray) const {
    if (!is_non_empty_crop(crop_gray)) {
        return false;
    }

    cv::Mat binary;
    cv::threshold(crop_gray, binary, kOtsuThreshold, kBinaryMaxValue,
                  cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::Mat labels;
    const int num_components = cv::connectedComponents(binary, labels);
    const double components_per_100px =
        static_cast<double>(num_components - 1) * kWidthNormalizationPx /
        static_cast<double>(crop_gray.cols);
    return components_per_100px >= kMinComponentsPer100Px;
}
