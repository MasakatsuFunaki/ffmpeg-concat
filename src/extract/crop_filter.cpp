#include "extract/crop_filter.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

// 1. Contrast check: reject near-uniform patches (plain backgrounds)
bool ContrastFilter::accept(const cv::Mat& crop_gray) const {
    cv::Scalar mean_val, stddev_val;
    cv::meanStdDev(crop_gray, mean_val, stddev_val);
    return stddev_val.val[0] >= 25.0;  // too uniform when below
}

// 2. Edge density check: text has many edge pixels from strokes
bool EdgeDensityFilter::accept(const cv::Mat& crop_gray) const {
    cv::Mat edges;
    cv::Canny(crop_gray, edges, 80, 160);
    double edge_ratio = static_cast<double>(cv::countNonZero(edges))
                      / (edges.rows * edges.cols);
    return edge_ratio >= 0.05;  // too few edges for text when below
}

// 3. Binary text check: threshold and count connected components.
//    Real text produces many small separated components (characters).
bool ComponentCountFilter::accept(const cv::Mat& crop_gray) const {
    cv::Mat binary;
    cv::threshold(crop_gray, binary, 0, 255,
                  cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::Mat labels;
    int num_components = cv::connectedComponents(binary, labels);
    // Need at least a few components (characters) relative to crop width
    double components_per_100px = (num_components - 1) * 100.0 / crop_gray.cols;
    return components_per_100px >= 1.0;  // too few text-like components when below
}
