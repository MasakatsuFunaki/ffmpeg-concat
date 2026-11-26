#include "extract/frame_scorer.h"

#include "extract/image_io.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr int kChannelsGray = 1;
constexpr int kChannelsBgr = 3;
constexpr int kChannelsBgra = 4;
constexpr int kSaturationChannel = 1;
constexpr double kCannyLowThreshold = 100.0;
constexpr double kCannyHighThreshold = 200.0;
constexpr double kSharpnessDivisor = 50.0;
constexpr double kSaturationDivisor = 2.55;
constexpr double kContrastDivisor = 1.28;
constexpr double kEdgeDensityWeight = 200.0;

}  // namespace

double FrameScorer::score(const fs::path& image_path) const {
    const cv::Mat frame = extract_io::read_image(image_path);
    return score(frame);
}

double FrameScorer::score(const cv::Mat& frame) const {
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0 || frame.depth() != CV_8U) {
        return 0.0;
    }

    cv::Mat gray;
    double saturation = 0.0;
    const int channels = frame.channels();

    if (channels == kChannelsBgr) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        cv::Mat hsv;
        cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
        saturation = cv::mean(hsv).val[kSaturationChannel];
    } else if (channels == kChannelsGray) {
        gray = frame;
    } else if (channels == kChannelsBgra) {
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::Mat hsv;
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        saturation = cv::mean(hsv).val[kSaturationChannel];
    } else {
        return 0.0;
    }

    // 3x3 Laplacian on CV_8U stays within [-1020, 1020], lossless in CV_16S.
    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_16S);
    cv::Scalar lap_mean;
    cv::Scalar lap_stddev;
    cv::meanStdDev(laplacian, lap_mean, lap_stddev);
    const double sharpness = lap_stddev.val[0] * lap_stddev.val[0];

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
