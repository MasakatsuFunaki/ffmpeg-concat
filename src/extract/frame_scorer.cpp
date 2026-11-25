#include "extract/frame_scorer.h"

#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

double FrameScorer::score(const fs::path& image_path) const {
    cv::Mat frame = cv::imread(image_path.string());
    if (frame.empty()) return 0.0;

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    // 1. Sharpness: Laplacian variance (blurry frames score low)
    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_64F);
    cv::Scalar lap_mean, lap_stddev;
    cv::meanStdDev(laplacian, lap_mean, lap_stddev);
    double sharpness = lap_stddev.val[0] * lap_stddev.val[0];

    // 2. Color saturation: mean of S channel in HSV (colorful frames score high)
    cv::Mat hsv;
    cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Mat> hsv_channels;
    cv::split(hsv, hsv_channels);
    double saturation = cv::mean(hsv_channels[1]).val[0];

    // 3. Contrast: standard deviation of grayscale (flat frames score low)
    cv::Scalar gray_mean, gray_stddev;
    cv::meanStdDev(gray, gray_mean, gray_stddev);
    double contrast = gray_stddev.val[0];

    // 4. Edge density: fraction of edge pixels via Canny (detail-rich frames score high)
    cv::Mat edges;
    cv::Canny(gray, edges, 100, 200);
    double edge_density = static_cast<double>(cv::countNonZero(edges))
                        / (edges.rows * edges.cols);

    // Combine scores (normalize each metric roughly to [0-100] range)
    double score = (sharpness / 50.0)
                 + (saturation / 2.55)
                 + (contrast / 1.28)
                 + (edge_density * 200.0);

    return score;
}
