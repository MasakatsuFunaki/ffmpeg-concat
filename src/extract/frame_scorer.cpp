#include "extract/frame_scorer.h"

#include <array>
#include <cstddef>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr std::size_t kHsvChannelCount = 3;
constexpr std::size_t kSaturationChannel = 1;
constexpr double kCannyLowThreshold = 100.0;
constexpr double kCannyHighThreshold = 200.0;
constexpr double kSharpnessDivisor = 50.0;
constexpr double kSaturationDivisor = 2.55;
constexpr double kContrastDivisor = 1.28;
constexpr double kEdgeDensityWeight = 200.0;

}  // namespace

double FrameScorer::score(const fs::path& image_path) const {
    const cv::Mat frame = cv::imread(image_path.string());
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0) {
        return 0.0;
    }

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_64F);
    cv::Scalar lap_mean;
    cv::Scalar lap_stddev;
    cv::meanStdDev(laplacian, lap_mean, lap_stddev);
    const double sharpness = lap_stddev.val[0] * lap_stddev.val[0];

    cv::Mat hsv;
    cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
    std::array<cv::Mat, kHsvChannelCount> hsv_channels;
    cv::split(hsv, hsv_channels.data());
    const double saturation = cv::mean(hsv_channels[kSaturationChannel]).val[0];

    cv::Scalar gray_mean;
    cv::Scalar gray_stddev;
    cv::meanStdDev(gray, gray_mean, gray_stddev);
    const double contrast = gray_stddev.val[0];

    cv::Mat edges;
    cv::Canny(gray, edges, kCannyLowThreshold, kCannyHighThreshold);
    const double total_pixels =
        static_cast<double>(edges.rows) * static_cast<double>(edges.cols);
    const double edge_density =
        (total_pixels > 0.0)
            ? static_cast<double>(cv::countNonZero(edges)) / total_pixels
            : 0.0;

    return (sharpness / kSharpnessDivisor) +
           (saturation / kSaturationDivisor) +
           (contrast / kContrastDivisor) +
           (edge_density * kEdgeDensityWeight);
}
