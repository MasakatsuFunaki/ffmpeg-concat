#pragma once

// TextRegionDetector -- locates text-like bounding blocks in an image via MSER
// region detection, geometric filtering, iterative spatial merging, and padding.

#include <vector>

#include <opencv2/core.hpp>

class TextRegionDetector {
public:
    [[nodiscard]] std::vector<cv::Rect> detect(const cv::Mat& image) const;
};
