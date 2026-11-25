#pragma once

// ---------------------------------------------------------------------------
// TextRegionDetector – plain class (deliberately NOT a pattern; one cohesive
// algorithm) that locates text-like blocks in an image: MSER region detection,
// per-region filtering, an iterative merge into text lines/blocks, and a final
// block filter with padding. Returns bounding rects of the text blocks.
// ---------------------------------------------------------------------------

#include <vector>

#include <opencv2/core.hpp>

class TextRegionDetector {
public:
    // Detect text regions in an image using MSER, returns bounding rects of text blocks.
    std::vector<cv::Rect> detect(const cv::Mat& image) const;
};
